// rfgphysx_host: the 64-bit PhysX side of rfgphys (research/docs/06 §3, §13).
//
// rfgphys.asi (inside the 32-bit game) creates a shared-memory mapping and starts this process with its name. The
// host steps one PhysX scene at a fixed rate, applies the game's commands before each step and publishes a snapshot
// of every body after it (src/shared/rfgphysx_ipc.h). It never waits for the game, and it exits when the game asks,
// when the game process is gone, or when the game's job object closes.
//
//   rfgphysx_host.exe --map <mapping name> [--log <file>] [--steps-csv <file>]
#include <PxPhysicsAPI.h>
#include <cudamanager/PxCudaContext.h>
#include <gpu/PxGpu.h>

#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <vector>

#include "rfgphysx_ipc.h"

using namespace physx;
namespace ipc = rfgphysx;

namespace {

// Contact filtering: simulation filter data word0 says what a shape is (ipc::ShapeFlags), word1 what it does not
// touch. Everything else collides (no triggers here).
PxFilterFlags filter_shader(PxFilterObjectAttributes, PxFilterData d0, PxFilterObjectAttributes, PxFilterData d1,
                            PxPairFlags& flags, const void*, PxU32) {
    if ((d0.word0 | d1.word0) & ipc::kShapeGone) return PxFilterFlag::eKILL;
    if ((d0.word0 & d1.word1) || (d1.word0 & d0.word1)) return PxFilterFlag::eSUPPRESS;
    flags = PxPairFlag::eCONTACT_DEFAULT;
    // A loose chunk's contacts are reported (Host::on_contact): its impacts go to the game's destruction
    // (research/docs/08 §12). New touches, and pairs that carry more than its threshold (setContactReportThreshold).
    if ((d0.word0 | d1.word0) & ipc::kShapeChunk)
        flags |= PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_THRESHOLD_FORCE_FOUND |
                 PxPairFlag::eNOTIFY_THRESHOLD_FORCE_PERSISTS |
                 PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}

PxFilterData filter_for(uint32_t shape_flags) {
    if (shape_flags & ipc::kShapePlayer) return PxFilterData(ipc::kShapePlayer, ipc::kShapeHeavy, 0, 0);
    return PxFilterData(shape_flags & (ipc::kShapeHeavy | ipc::kShapeChunk), 0, 0, 0);
}

// ---- log ------------------------------------------------------------------------------------------------------------

FILE* g_log = nullptr;
LARGE_INTEGER g_freq, g_t0;

double now_ms() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) * 1000.0 / double(g_freq.QuadPart);
}

int64_t qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void logf(const char* fmt, ...);

#if RFGPHYS_HOST_ZONES
// A host linked with PhysX's profile configuration (tools/build_host.ps1 -PhysXProfile): PhysX's profiler zones, summed
// by name (a zone's time includes the zones inside it, and threads overlap) and logged with the step statistics.
class ZoneTimes : public PxProfilerCallback {
public:
    void* zoneStart(const char* name, bool detached, uint64_t) override {
        const int64_t now = qpc();
        if (detached) {  // ends on another thread
            AcquireSRWLockExclusive(&lock_);
            open_[name] = now;
            ReleaseSRWLockExclusive(&lock_);
        }
        return reinterpret_cast<void*>(now);
    }
    void zoneEnd(void* start, const char* name, bool detached, uint64_t) override {
        const int64_t now = qpc();
        AcquireSRWLockExclusive(&lock_);
        int64_t from = reinterpret_cast<int64_t>(start);
        if (detached) {
            const auto it = open_.find(name);
            from = it != open_.end() ? it->second : now;
        }
        Zone& z = zones_[name];
        z.ticks += now - from;
        ++z.count;
        ReleaseSRWLockExclusive(&lock_);
    }
    void recordData(int32_t value, const char* name, uint64_t) override {
        AcquireSRWLockExclusive(&lock_);
        int32_t& most = values_[name];
        most = std::max(most, value);
        ReleaseSRWLockExclusive(&lock_);
    }
    // After a step: the zones of one that took long, by name, longest first (only so many a report).
    void end_step(double step_ms) {
        AcquireSRWLockExclusive(&lock_);
        if (step_ms >= 45.0 && slow_logged_ < 4) {
            ++slow_logged_;
            std::vector<std::pair<int64_t, const char*>> top;
            for (const auto& [name, z] : zones_) top.push_back({z.ticks, name});
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string line;
            for (size_t i = 0; i < top.size() && i < 24; ++i) {
                char one[128];
                snprintf(one, sizeof(one), "%s %.1f; ", top[i].second, double(top[i].first) * 1000.0 / double(g_freq.QuadPart));
                line += one;
            }
            logf("  a step of %.0f ms: %.900s", step_ms, line.c_str());
        }
        for (const auto& [name, z] : zones_) {
            Zone& t = totals_[name];
            t.ticks += z.ticks;
            t.count += z.count;
        }
        zones_.clear();
        ReleaseSRWLockExclusive(&lock_);
    }
    void report(uint32_t steps) {
        AcquireSRWLockExclusive(&lock_);
        slow_logged_ = 0;
        for (const auto& [name, most] : values_) logf("  at most in these steps: %s %d", name, most);
        values_.clear();
        ReleaseSRWLockExclusive(&lock_);
        struct Row {
            std::string name;
            int64_t ticks;
            uint64_t count;
        };
        std::unordered_map<std::string, Row> merged;  // one text can have several addresses (the host, the GPU module)
        AcquireSRWLockExclusive(&lock_);
        for (const auto& [name, z] : totals_) {
            Row& r = merged[name];
            r.name = name;
            r.ticks += z.ticks;
            r.count += z.count;
        }
        totals_.clear();
        ReleaseSRWLockExclusive(&lock_);
        std::vector<Row> rows;
        for (auto& [name, r] : merged) rows.push_back(r);
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ticks > b.ticks; });
        const double to_ms = 1000.0 / double(g_freq.QuadPart) / double(std::max(1u, steps));
        std::string line;
        for (size_t i = 0; i < rows.size() && i < 72; ++i) {
            char one[160];
            snprintf(one, sizeof(one), "%s %.2f ms (%.1f); ", rows[i].name.c_str(), double(rows[i].ticks) * to_ms,
                     double(rows[i].count) / double(std::max(1u, steps)));
            line += one;
            if ((i + 1) % 6 == 0 || i + 1 == rows.size() || i == 71) {
                logf("  zones a step: %s", line.c_str());
                line.clear();
            }
        }
    }

private:
    struct Zone {
        int64_t ticks = 0;
        uint64_t count = 0;
    };
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::unordered_map<const char*, Zone> zones_, totals_;  // of this step; of the steps since the last report
    uint32_t slow_logged_ = 0;
    std::unordered_map<const char*, int64_t> open_;
    std::unordered_map<const char*, int32_t> values_;
};
ZoneTimes g_zones;
// The host's own work, among PhysX's zones.
struct HostZone {
    const char* name;
    void* start;
    explicit HostZone(const char* n) : name(n), start(g_zones.zoneStart(n, false, 0)) {}
    ~HostZone() { g_zones.zoneEnd(start, name, false, 0); }
};
#define HOST_ZONE(name) HostZone host_zone(name)
#else
#define HOST_ZONE(name) ((void)0)
#endif

std::wstring g_log_dir;  // the folder of the host's log

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_log) {
        fprintf(g_log, "[%10.3f] %s\n", now_ms() / 1000.0, buf);
        fflush(g_log);
    }
}

// ---- a stuck main loop ----------------------------------------------------------------------------------------------

// The heartbeat thread notices when the main loop has finished no step for a while and logs where it stands: the
// main thread is suspended for a moment, its stack walked, and the frames named from the host's PDB (module + offset
// for PhysX's GPU module, which has no symbols here).
std::atomic<int64_t> g_last_progress_qpc{0};  // the main loop's last finished step (or start)
HANDLE g_main_thread = nullptr;

// Walks a thread's stack from `ctx` and logs its frames.
void log_stack(HANDLE thread, CONTEXT& ctx, bool suspended) {
    DWORD64 frames[48];
    int n = 0;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip, f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp, f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp, f.AddrStack.Mode = AddrModeFlat;
    while (n < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), thread, &f, &ctx, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
           f.AddrPC.Offset)
        frames[n++] = f.AddrPC.Offset;
    if (suspended) ResumeThread(thread);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (int i = 0; i < n; ++i) {
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        char module[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(frames[i]), &mod))
            GetModuleFileNameA(mod, module, MAX_PATH);
        const char* slash = strrchr(module, 0x5C);  // the last backslash
        const char* base = slash ? slash + 1 : module;
        if (SymFromAddr(GetCurrentProcess(), frames[i], &disp, sym))
            logf("  #%-2d %s!%s+0x%llX", i, base, sym->Name, disp);
        else
            logf("  #%-2d %s+0x%llX", i, base, frames[i] - reinterpret_cast<DWORD64>(mod));
    }
}

void log_main_stack(double stuck_ms) {
    logf("main loop stuck: no step finished for %.0f s; its stack:", stuck_ms / 1000.0);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(g_main_thread) == DWORD(-1)) return;
    if (GetThreadContext(g_main_thread, &ctx)) log_stack(g_main_thread, ctx, true);
    else ResumeThread(g_main_thread);
}

// A crash says where: the game only sees the host gone, with its exit code.
LONG WINAPI log_crash(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD& r = *e->ExceptionRecord;
    logf("host crashed: exception 0x%08lX at %p, thread %lu; %s address %p; the stack:", r.ExceptionCode, r.ExceptionAddress,
         GetCurrentThreadId(), r.NumberParameters && r.ExceptionInformation[0] ? "writing" : "reading",
         r.NumberParameters > 1 ? reinterpret_cast<void*>(r.ExceptionInformation[1]) : nullptr);
    log_stack(GetCurrentThread(), *e->ContextRecord, false);
    return EXCEPTION_CONTINUE_SEARCH;
}

struct ErrorCallback : PxErrorCallback {
    // Warnings (one per cooked hull too oblong for the GPU) have a budget of their own, so that errors after them still
    // show; the rest are counted (B5b: 45 failed GPU allocations used up the budget, then the host crashed unexplained).
    std::atomic<int> reported{0}, warned{0}, unlogged{0};
    std::atomic<bool> aborted{false};  // PhysX cannot go on (a CUDA error): every step after it does nothing
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        const bool warning = code == PxErrorCode::eDEBUG_INFO || code == PxErrorCode::eDEBUG_WARNING ||
                             code == PxErrorCode::ePERF_WARNING;
        if (code == PxErrorCode::eABORT) aborted = true;
        if ((warning ? warned : reported)++ < 50) logf("PhysX error %d: %s (%s:%d)", int(code), message, file, line);
        else unlogged++;
    }
};

PxDefaultAllocator g_allocator;
ErrorCallback g_errors;

// ---- shared memory ----------------------------------------------------------------------------------------------------

struct Shared {
    HANDLE mapping = nullptr;
    uint8_t* base = nullptr;
    ipc::Control* control = nullptr;
    uint8_t* ring = nullptr;
    uint32_t ring_mask = 0;
    uint32_t tail = 0;
    uint32_t back = 0;  // the snapshot buffer the host writes next (the game starts with 2, latest with 1)

    bool open(const wchar_t* name) {
        mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (!mapping) return false;
        base = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!base) return false;
        control = reinterpret_cast<ipc::Control*>(base);
        if (control->magic != ipc::kMagic || control->version != ipc::kVersion) return false;
        ring = base + control->ring_offset;
        ring_mask = control->ring_size - 1;
        tail = control->ring_tail.load(std::memory_order_acquire);
        return true;
    }

    ipc::SnapshotHeader* snapshot(uint32_t index) {
        return reinterpret_cast<ipc::SnapshotHeader*>(base + control->snapshot_offset +
                                                      size_t(index) * control->snapshot_stride);
    }

    void message(const char* text) {
        strncpy_s(control->host_message, text, _TRUNCATE);
    }
};

// ---- PhysX scene ------------------------------------------------------------------------------------------------------

struct Body {
    PxRigidDynamic* actor = nullptr;
};

struct Host {
    Shared sh;
    uint32_t max_bodies = 0;
    float dt = 1.0f / 60.0f;

    PxFoundation* foundation = nullptr;
    PxPhysics* physics = nullptr;
    PxCudaContextManager* cuda = nullptr;
    PxDefaultCpuDispatcher* dispatcher = nullptr;
    PxScene* scene = nullptr;
    PxMaterial* material = nullptr;
    bool gpu = false;

    std::vector<Body> bodies;            // by slot
    std::vector<ipc::BodyPose> poses;    // by slot: what the next snapshot publishes
    std::vector<uint32_t> awake_slots;   // slots PhysX moved in the last step
    std::vector<uint32_t> now_awake;     // scratch for step()
    std::vector<uint32_t> freeze_list;   // scratch for step()
    std::vector<uint8_t> was_awake;
    std::vector<uint32_t> awake_since;   // by slot: the step its body last woke (for the restless count in the log)
    static constexpr uint32_t kRestlessSteps = 300;  // awake this long without a break: restless
    // Freezing (06 §12): a pile is one island and never falls asleep, so a piece that stays calm for kFreezeSeconds
    // becomes kinematic until an explosion or the player comes near it. That cuts the pile's island apart, so the
    // rest can sleep, and the game stops moving its render instance.
    static constexpr float kFreezeSeconds = 1.0f;
    static constexpr float kCalmSpeed = 0.15f, kCalmSpin = 0.5f;
    // Or a piece that went nowhere for as long (less than kStillDist, turned less than 12 degrees): one rocking in
    // place in a pile never slows down enough to be calm (a thousand of them rocked in a Wrecking Crew match, the
    // piles bubbling), yet stays where it is.
    static constexpr float kStillDist = 0.06f, kStillCos = 0.99452f;  // cos(12 deg / 2): quaternion dot product
    // Overlaps (pieces coming over from Havok's piles, the player's capsule walked into a pile) are resolved at this
    // speed at most: faster, pieces popped up out of piles and fell back.
    static constexpr float kMaxDepenetration = 1.5f;  // m/s, PxRigidBody::setMaxDepenetrationVelocity of debris
    // Nothing here has any business going faster, whatever the solver gives it: a blast's push is at most 40 m/s, and a chunk
    // is a fall of 80 m. A chunk's turning is held to the same at its far end.
    static constexpr float kPieceMaxSpeed = 60.0f, kChunkMaxSpeed = 40.0f;  // m/s
    // A piece that waited in Havok comes over overlapping some (pieces that moved into it meanwhile, the pieces of its
    // own shatter, which Havok had pushed apart only so far; more than 3 cm deep for most pieces, measured): for its
    // first kGentleSteps that is undone at kGentleDepenetration at most, without piles bubbling up.
    static constexpr float kGentleDepenetration = 0.2f;
    static constexpr uint32_t kGentleSteps = 180;
    std::deque<std::pair<uint32_t, uint32_t>> gentle;  // slot, the step its gentleness ends
    std::vector<uint16_t> calm;
    std::vector<ipc::BodyPose> still_from;  // by slot: where its current going-nowhere window began
    std::vector<uint32_t> still_step;       // and when
    // Pieces pushed upward in their first second here: they came over inside or under other pieces and PhysX pushed
    // them out (the piles bubbling). Ones that came over rising, or that an explosion or a blow moved, do not count.
    static constexpr uint32_t kBubbleSteps = 60;
    static constexpr float kBubbleSpeed = 0.3f;  // m/s upwards
    std::vector<uint32_t> born_step;
    std::vector<uint8_t> born_excused;           // by slot: 1 once it no longer counts
    uint32_t stat_bubbled = 0;
    std::vector<uint8_t> frozen, user_kinematic, heavy;  // heavy: the player's capsule does not touch it
    std::vector<uint8_t> chunk;  // ipc::kBodyChunk: a loose building chunk, dynamic, waking pieces as a pusher does
    // Pushers: kinematic bodies standing for heavy game bodies (vehicles and walkers, ipc::kBodyPusher). A frozen
    // piece is kinematic too, and two kinematic bodies pass through each other, so every step a pusher wakes the
    // frozen pieces it is about to reach, whatever their mass.
    // A pusher wakes them once a point of it has gone kPushStep since it last did: a loose chunk of a building lying
    // in a pile jitters by millimetres, and woke the whole pile every step for nothing (100,000 thaws in a Wrecking
    // Crew match, the awake pieces doubled). How far its points went is told by its shape (pusher_moved), not its
    // origin: a chunk has its building's origin, often 10-20 m away, and a chunk of a metre rolling over at 0.3 m/s
    // swung that origin half a metre a step; it woke everything within metres of it every step (its speed widens
    // the reach), and the piles around it bubbled as the woken pieces settled again.
    // Past what it goes before it next wakes any (kPushStep). Most wakes are these, and nearly all of them go nowhere.
    static constexpr float kPushReach = 0.15f;      // m beyond the pusher's bounds
    static constexpr float kPushLookahead = 4.0f;   // steps of its current speed, also beyond its bounds
    static constexpr float kPushStep = 0.1f;        // m
    static constexpr float kWakeTouching = 0.03f;   // m: a chunk's blocks, all round, when it wakes only ahead of it
    static constexpr float kWakeAheadSpeed = 0.05f; // m/s: a block going slower than this has no ahead
    // N*s per contact point and step: a 250 kg piece (a typical concrete one) gets up to about 1.2 m/s a step from
    // each, enough to be swept aside, too little to be driven through the ground.
    static constexpr float kPusherMaxImpulse = 300.0f;
    static constexpr float kPlayerThawStep = 0.05f; // m
    std::vector<uint8_t> pusher;
    std::vector<uint32_t> pusher_slots;  // may hold stale entries; pusher[] is the truth
    std::vector<ipc::BodyPose> pusher_woke_at;  // per slot: the pose it last woke pieces from
    std::vector<PxVec3> pusher_mid;      // per slot: the middle of its shapes' bounds, in its own frame
    std::vector<float> pusher_half;      // and how far its shapes reach from there
    std::vector<uint32_t> pusher_wakes, pusher_woken;  // per slot, this log window: wake events and pieces woken
    uint32_t stat_pushed_awake = 0, stat_impulses = 0, stat_unsupported_bodies = 0;
    std::vector<uint32_t> frozen_slots;  // may hold stale entries; frozen[] is the truth
    uint32_t frozen_count = 0, stat_frozen = 0, stat_frozen_still = 0, stat_thawed = 0;
    // A frozen piece does not notice when what it rests on goes, and would hang in the air. So removed statics wake
    // the frozen pieces in and above their bounds, and a woken piece that then moves (3 cm within 83 ms: it lost its
    // support or was pushed) wakes the frozen pieces touching it from above, which do the same, ring by ring. A woken
    // piece that stays put passes nothing on (otherwise the player's capsule, which wakes what it comes near ten
    // times a second, kept whole piles churning). Frozen pieces are found in a grid of 2 m cells; entries go stale
    // when they thaw, and the grid is rebuilt now and then.
    static constexpr float kCell = 2.0f, kTouch = 0.05f, kNeighbourReach = 3.0f, kMovedSq = 0.03f * 0.03f;
    // Later, it has to be a real drop: at 5 cm, pieces creeping out of overlaps woke their neighbours for ever, 20,000 wakes
    // from 20,000 pieces looked at, of which 17 had moved in time (research/docs/08 §14).
    static constexpr float kMovedLateSq = 0.2f * 0.2f;
    static constexpr uint32_t kPassOnPerStep = 400;
    static constexpr float kCascadeDelaySeconds = 5.0f / 60.0f;  // falling, 3.4 cm by then
    // A freeze and a thaw are the dearest things a piece can do here (research/docs/08 §14): each takes it out of the
    // broad phase and puts it back, with all its pairs, and any such change sorts the whole broad phase once more. So
    // what rests on a piece is told by bounds that touch (a sphere around each woke dozens at a time), and only if it
    // starts higher up.
    std::vector<PxBounds3> frozen_bounds;  // by slot: its bounds when it froze
    std::vector<uint8_t> moved_late;       // by slot: it had not moved when it was looked at; if it has by the time it freezes, it says so then
    uint32_t stat_cascade_moved = 0, stat_cascade_looked = 0, stat_cascade_late = 0;
    // A pile frozen before it had quite settled comes down when it is woken, and wakes what is on it, until 60,000 pieces are
    // awake (seen in replays). Then a step takes 50-100 ms; the solver's buffers, which grow with its contacts and never
    // shrink, reach 21 GB, and the next allocation kills the host; the woken pieces, out of their piles, use up the render
    // instances (research/docs/08 §15). So wakes wait while this many are awake, and such a pile comes down in its time.
    // Explosions, blows and the player do not wait.
    static constexpr uint32_t kWakesWaitAt = 15000;
    bool wakes_wait() const { return awake_slots.size() + thaw_list.size() >= kWakesWaitAt; }
    std::vector<PxBounds3> unsupported_later;  // what went while wakes waited
    uint32_t stat_wakes_waited = 0;
    // With this many awake all the same (an explosion in a pile), pieces freeze in a quarter of the time: a world second is
    // many seconds by then, and they were coming down from 65,000 awake too slowly to save the host.
    static constexpr uint32_t kHurryFreezingAt = 30000;
    uint32_t stat_hurried_steps = 0;
    // Where pieces that moved lay, and the pieces: what rested on them still has to be told (at most kPassOnPerStep a step).
    std::deque<std::pair<PxBounds3, uint32_t>> resting_on_later;
    // In a step in which no body comes, goes, freezes or thaws, the broad phase sorts its boxes (all 430,000 of them, on
    // three axes) once instead of twice. So they do all that every change_every steps, the piece frozen or thawed a
    // step or two late, and a released body staying in the scene as a ghost until then.
    uint32_t change_every = 1;
    bool applying_deferred = false;
    bool changes_now() const { return step_index % change_every == 0; }
    std::vector<uint8_t> freeze_pending, thaw_pending;  // by slot
    std::vector<uint32_t> thaw_list;
    std::vector<PxRigidDynamic*> ghosts;
    enum ChangeCause { kChangeThaw, kChangeFreeze, kChangeNew, kChangeGone, kChangeBroke, kChangeParts, kChangeStatics, kChangeCauses };
    uint32_t changes = 0;  // bits of ChangeCause, for the next step
    uint32_t stat_steps_changed = 0, stat_change_by[kChangeCauses] = {};
    double stat_ms_changed = 0;
    // Commands that change the broad phase and can wait for the next change: parts and pieces gone, in the game already.
    std::vector<uint8_t> deferred_cmds;
    std::vector<std::pair<PxRigidDynamic*, PxShape*>> parts_to_detach;  // broke off: ghosts go after them
    std::vector<ipc::Impulse> impulses_waiting;  // on bodies that wait to come into the scene
    std::vector<ipc::Vec3> wake_pos;    // per slot: where it was when woken
    std::vector<uint32_t> wake_due;     // per slot: the step its movement is checked
    std::vector<float> body_radius;  // per slot: bounds of its shapes around its origin
    std::unordered_map<uint64_t, std::vector<uint32_t>> frozen_grid;
    std::vector<uint32_t> cascade_next;
    std::vector<uint8_t> cascade_mark;
    uint32_t stat_cascade = 0, stat_unsupported = 0;
    std::unordered_map<uint32_t, std::vector<PxShape*>> shapes;  // body shapes by id: one or more convex parts
    std::vector<PxRigidStatic*> statics;                                  // test planes and boxes
    // The mirrored world, one group per Havok body: its convex pieces as shapes of one static actor (found by key,
    // so a building can lose single pieces), and its meshes and height fields as actors of their own.
    // Detaching a shape goes through every pair of its actor (Sc::ElementSim::getElemInteractionsReverse, PhysX's own comment
    // calls it inefficient for a large compound). One actor to a building, a building that lost its middle took up to 1 s: all
    // the debris stopped just as it came down. So a building's pieces are in actors of this many.
    static constexpr uint32_t kPiecesPerActor = 64;
    struct Group {
        std::vector<PxRigidStatic*> pieces;
        std::unordered_map<uint32_t, std::vector<PxShape*>> by_key;
        std::vector<PxRigidStatic*> actors;
        uint32_t shape_count = 0;
    };
    std::unordered_map<uint32_t, Group> groups;
    uint32_t static_count = 0;  // static shapes of the mirror
    double stat_cook_ms = 0;
    uint32_t stat_cooked = 0, stat_cook_failed = 0;
    std::vector<PxActor*> pending_add;   // bodies created by this batch of commands, added to the scene together
    uint32_t live = 0, slots_used = 0;
    static constexpr uint32_t kPoseBlockShift = 10;
    std::vector<uint8_t> pose_block_used;  // by slot >> kPoseBlockShift: has held a body since the host started

    uint32_t step_index = 0, sequence = 0, last_frame = 0, commands_since_snapshot = 0;
    double sim_time = 0;
    int64_t last_frame_qpc = 0;

    // Statistics for the periodic log line.
    uint32_t stat_steps = 0, stat_commands = 0;
    double stat_step_ms = 0, stat_step_max = 0, stat_work_ms = 0, stat_cmd_latency_ms = 0;
    double stat_since_ms = 0, stat_since_sim = 0;  // the last report: for how fast the world went since
    uint32_t stat_cmd_latency_n = 0;
    FILE* steps_csv = nullptr;

    bool init_physx(uint32_t backend) {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errors);
        if (!foundation) return fail("PxCreateFoundation failed");
#if RFGPHYS_HOST_ZONES
        PxSetProfilerCallback(&g_zones);
        if (backend == ipc::kBackendGpu) PxSetPhysXGpuProfilerCallback(&g_zones);
        logf("PhysX's profile configuration: its profiler zones are logged with the step statistics");
#endif
        if (backend == ipc::kBackendGpu) {
            PxCudaContextManagerDesc desc;
            cuda = PxCreateCudaContextManager(*foundation, desc, PxGetProfilerCallback());
            if (cuda && cuda->contextIsValid()) {
                gpu = true;
                logf("GPU: %s, %d SMs, %.0f MB", cuda->getDeviceName(), cuda->getMultiprocessorCount(),
                     double(cuda->getDeviceTotalMemBytes()) / (1024.0 * 1024.0));
            } else {
                logf("no valid CUDA context: CPU PhysX instead");
                if (cuda) cuda->release();
                cuda = nullptr;
            }
        }
        physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), false, nullptr);
        if (!physics) return fail("PxCreatePhysics failed");
        material = physics->createMaterial(0.8f, 0.6f, 0.1f);

        PxSceneDesc sd(physics->getTolerancesScale());
        sd.gravity = PxVec3(0, -9.81f, 0);
        // GPU dynamics leave PhysX less CPU work to spread (at 2 threads it held up a step of 25 ms by 3-4 ms, research/docs/08
        // §14); the CPU version takes what the game leaves over.
        const uint32_t cpu_threads = sh.control->cpu_threads ? sh.control->cpu_threads : gpu ? 4 : 6;
        dispatcher = PxDefaultCpuDispatcherCreate(cpu_threads);
        sd.cpuDispatcher = dispatcher;
        sd.filterShader = filter_shader;
        contact_cb.host = this;
        sd.simulationEventCallback = &contact_cb;
        sd.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_ACTIVE_ACTORS;
        sd.solverType = PxSolverType::eTGS;
        // Scene queries see the static world only (body shapes are simulation shapes, so thousands of moving
        // pieces cost the query structure nothing); explosions go through the host's own pose table.
        sd.staticStructure = PxPruningStructureType::eDYNAMIC_AABB_TREE;
        if (gpu) {
            sd.cudaContextManager = cuda;
            sd.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS;
            sd.broadPhaseType = PxBroadPhaseType::eGPU;
            sd.gpuDynamicsConfig.maxRigidContactCount = 2u << 20;
            sd.gpuDynamicsConfig.maxRigidPatchCount = 1u << 20;
            sd.gpuDynamicsConfig.foundLostPairsCapacity = 1u << 20;
            sd.gpuDynamicsConfig.tempBufferCapacity = 64ull << 20;
            sd.gpuDynamicsConfig.heapCapacity = 64u << 20;
            sd.gpuMaxNumPartitions = 32;
            // The solver's static contacts (statics and kinematics: frozen pieces too) go through
            // bodies * (the most on any one body / this) kernel threads and buffer entries, per iteration: with the
            // default 16, one loose chunk touching thousands slowed every body (research/docs/08, C3). At the most
            // PhysX allows, a body does more of its own contacts in one pass, in order, which converges no worse.
            sd.gpuMaxNumStaticPartitions = 255;
        } else {
            sd.broadPhaseType = PxBroadPhaseType::ePABP;
        }
        scene = physics->createScene(sd);
        if (!scene) return fail("createScene failed");
        logf("PhysX %d.%d.%d scene ready: %s dynamics, %u CPU threads, TGS solver, %.0f Hz steps, %u body slots, bodies "
             "change every %u steps", PX_PHYSICS_VERSION_MAJOR, PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX,
             gpu ? "GPU" : "CPU", cpu_threads, 1.0 / dt, max_bodies, change_every);
        return true;
    }

    bool fail(const char* what) {
        logf("failed: %s", what);
        sh.message(what);
        sh.control->host_state.store(uint32_t(ipc::HostState::Failed), std::memory_order_release);
        return false;
    }

    // A CUDA failure (a pinned allocation PhysX could not get leaves it writing through a null buffer) puts the context
    // in abort mode and the scene in a corrupt state: from then on it adds no actor and steps nothing (B5b).
    bool gpu_failed() const {
        return g_errors.aborted || (gpu && cuda && cuda->getCudaContext() && cuda->getCudaContext()->isInAbortMode());
    }

    // ---- commands ----

    void reset() {
        flush_pending();
        for (uint32_t s = 0; s < slots_used; ++s) release_body(s);
        for (PxRigidDynamic* a : ghosts) a->release();
        ghosts.clear();
        thaw_list.clear();
        freeze_list.clear();
        deferred_cmds.clear();
        impulses_waiting.clear();
        parts_to_detach.clear();
        unsupported_later.clear();
        resting_on_later.clear();
        pending_chunks.clear();
        crush_owed.clear();
        chunk_support.clear();
        for (PxRigidStatic* st : statics) st->release();
        statics.clear();
        for (auto& [g, group] : groups) release_group(group);
        groups.clear();
        static_count = 0;
        sh.control->statics.store(0, std::memory_order_relaxed);
        for (auto& [id, parts] : shapes)
            for (PxShape* shape : parts) shape->release();
        shapes.clear();
        clear_hull_cache();
        awake_slots.clear();
        std::fill(was_awake.begin(), was_awake.end(), uint8_t(0));
        frozen_slots.clear();
        frozen_count = 0;
        pusher_slots.clear();
        gentle.clear();
        slots_used = 0;
        logf("reset: scene emptied");
    }

    void release_body(uint32_t slot) {
        Body& b = bodies[slot];
        if (!b.actor) return;
        freeze_pending[slot] = thaw_pending[slot] = 0;
        for (size_t at = 0; at < deferred_cmds.size();) {  // what waited for this body goes with it; the slot may be taken at once
            auto* h = reinterpret_cast<ipc::CmdHeader*>(deferred_cmds.data() + at);
            if (ipc::Cmd(h->type) == ipc::Cmd::RemoveParts && reinterpret_cast<ipc::RemoveParts*>(h)->slot == slot)
                reinterpret_cast<ipc::RemoveParts*>(h)->slot = max_bodies;
            at += h->size;
        }
        if (b.actor->getScene() && change_every > 1) {
            b.actor->userData = reinterpret_cast<void*>(uintptr_t(ipc::kEventNone));  // no slot of ours any more
            ghosts.push_back(b.actor);
        } else if (b.actor->getScene()) {
            changes |= 1u << kChangeGone;
            b.actor->release();
        } else {  // still waiting in pending_add
            pending_add.erase(std::remove(pending_add.begin(), pending_add.end(), static_cast<PxActor*>(b.actor)),
                              pending_add.end());
            b.actor->release();
        }
        b.actor = nullptr;
        poses[slot].flags = 0;
        if (frozen[slot]) {
            frozen[slot] = 0;
            --frozen_count;
        }
        calm[slot] = 0;
        user_kinematic[slot] = 0;
        pusher[slot] = 0;
        chunk[slot] = 0;
        chunk_parts.erase(slot);
        chunk_break_force.erase(slot);
        --live;
    }

    void add_shape(uint32_t id, PxShape* shape, bool start_over = true) {
        if (!shape) return;
        std::vector<PxShape*>& parts = shapes[id];
        if (start_over) {
            for (PxShape* old : parts) old->release();
            parts.clear();
        }
        parts.push_back(shape);
    }

    // Cooks the convex hull of the points around their centre (returned; the shape's local pose puts it back), grown
    // by Havok's convex radius. Havok rounds every shape by it (the game sets 1 cm): pieces lie that far apart there,
    // and their meshes are drawn about that much larger than their points. Without it a piece sank by it into what it
    // lay on (thin pieces on glass showed through underneath), and panels and sheets, flat in their points, came over
    // without any thickness: tilted, they cooked to slivers without a usable mass and hung in the air where they were
    // taken over (Vista's sphere of panels). Growing thin hulls by a share of their size instead (for PhysX's GPU code)
    // made big panels thicker than in Havok and small ones thinner: piles bubbled where pieces came over overlapping
    // their neighbours, and thin pieces jittered for a long time.
    // The points are copied out along their principal axes and the diagonals between them; along each principal axis
    // the hull is at least kMinThickness thick. PhysX's QuickHull looped forever on a building piece of the open world,
    // sent in world coordinates thousands of metres from the origin (Wrecking Crew maps sit near it): the points are
    // centred and welded within 1 mm, and what is left without a hull (a point, NaN) is skipped.
    static constexpr float kMinThickness = 0.004f;
    static constexpr uint32_t kHullPoints = 128;  // welded points used
    uint32_t stat_hull_skipped = 0, stat_hull_thickened = 0, stat_hull_boxes = 0;
    PxConvexMesh* cook_hull(const ipc::Vec3* v, uint32_t count, float radius, PxVec3& centre) {
        PxBounds3 b = PxBounds3::empty();
        for (uint32_t i = 0; i < count; ++i) b.include(PxVec3(v[i].x, v[i].y, v[i].z));
        centre = b.getCenter();
        const PxVec3 extent = b.getDimensions();
        if (!(extent.maxElement() > 0.002f) || !(extent.maxElement() < 1000.0f)) {  // a point, or NaN / absurd
            ++stat_hull_skipped;
            return nullptr;
        }
        PxVec3 pts[kHullPoints];
        uint32_t n = 0;
        for (uint32_t i = 0; i < count && n < kHullPoints; ++i) {
            const PxVec3 p = PxVec3(v[i].x, v[i].y, v[i].z) - centre;
            bool dup = false;
            for (uint32_t k = 0; k < n && !dup; ++k) dup = (pts[k] - p).magnitudeSquared() < 1e-6f;
            if (!dup) pts[n++] = p;
        }
        if (n < 2) {
            ++stat_hull_skipped;
            return nullptr;
        }
        PxVec3 mean(0.0f);
        for (uint32_t i = 0; i < n; ++i) mean += pts[i];
        mean *= 1.0f / float(n);
        PxMat33 cov(PxZero);
        for (uint32_t i = 0; i < n; ++i) {
            const PxVec3 d = pts[i] - mean;
            cov.column0 += d * d.x;
            cov.column1 += d * d.y;
            cov.column2 += d * d.z;
        }
        PxQuat q;
        PxDiagonalize(cov, q);
        const PxMat33 axes(q);  // columns: the principal axes
        float size[3];
        for (uint32_t k = 0; k < 3; ++k) {
            float lo = PX_MAX_F32, hi = -PX_MAX_F32;
            for (uint32_t i = 0; i < n; ++i) {
                const float d = pts[i].dot(axes[k]);
                lo = std::min(lo, d), hi = std::max(hi, d);
            }
            size[k] = hi - lo;
        }
        if (!(radius > 0.0f && radius < 0.5f)) radius = 0.0f;  // none, NaN or absurd
        float grow[3];
        bool thickened = false;
        for (uint32_t k = 0; k < 3; ++k) {
            grow[k] = std::max(radius, 0.5f * (kMinThickness - size[k]));
            thickened |= grow[k] > radius;
        }
        stat_hull_thickened += thickened ? 1 : 0;
        PxVec3 dirs[14];  // along the principal axes; with a radius also along the diagonals between them
        uint32_t nd = 0;
        for (uint32_t k = 0; k < 3; ++k)
            if (grow[k] > 0.0f) {
                dirs[nd++] = axes[k] * grow[k];
                dirs[nd++] = axes[k] * -grow[k];
            }
        if (radius > 0.0f) {
            const float d = radius * 0.57735027f;  // 1/sqrt(3)
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sy = -1; sy <= 1; sy += 2)
                    for (int sz = -1; sz <= 1; sz += 2)
                        dirs[nd++] = (axes[0] * float(sx) + axes[1] * float(sy) + axes[2] * float(sz)) * d;
        }
        PxVec3 grown[kHullPoints * 14];
        const PxVec3* hull = pts;
        uint32_t hull_n = n;
        if (nd) {
            hull_n = 0;
            for (uint32_t i = 0; i < n; ++i)
                for (uint32_t j = 0; j < nd; ++j) grown[hull_n++] = pts[i] + dirs[j];
            hull = grown;
        }
        if (hull_n < 4) {
            ++stat_hull_skipped;
            return nullptr;
        }
        PxCookingParams params(physics->getTolerancesScale());
        params.buildGPUData = gpu;
        PxConvexMeshDesc desc;
        desc.points.count = hull_n;
        desc.points.stride = sizeof(PxVec3);
        desc.points.data = hull;
        desc.flags = PxConvexFlag::eCOMPUTE_CONVEX | PxConvexFlag::eSHIFT_VERTICES;
        desc.vertexLimit = 64;
        return PxCreateConvexMesh(params, desc, physics->getPhysicsInsertionCallback());
    }

    PxShape* make_convex(const ipc::Vec3* v, uint32_t count, float radius) {
        PxVec3 centre;
        PxConvexMesh* mesh = cook_hull(v, count, radius, centre);
        PxShape* shape = nullptr;
        if (mesh) {
            shape = physics->createShape(PxConvexMeshGeometry(mesh), *material, false, PxShapeFlag::eSIMULATION_SHAPE);
            mesh->release();  // the shape keeps its reference
        } else {
            // A piece without a shape gets no body and hangs where it was taken over: a box around its points.
            PxBounds3 b = PxBounds3::empty();
            for (uint32_t i = 0; i < count; ++i) b.include(PxVec3(v[i].x, v[i].y, v[i].z));
            if (b.isEmpty() || !b.isFinite() || b.getDimensions().maxElement() >= 1000.0f) return nullptr;
            centre = b.getCenter();
            const float r = radius > 0.0f && radius < 0.5f ? radius : 0.0f;
            const PxVec3 e = b.getExtents() + PxVec3(r);
            const float least = 0.5f * kMinThickness;
            const PxVec3 half(std::max(e.x, least), std::max(e.y, least), std::max(e.z, least));
            shape = physics->createShape(PxBoxGeometry(half), *material, false, PxShapeFlag::eSIMULATION_SHAPE);
            ++stat_hull_boxes;
        }
        if (shape) shape->setLocalPose(PxTransform(centre));
        return shape;
    }

    uint32_t stat_no_shape = 0, stat_inertia_fixed = 0, stat_born_frozen = 0;
    void create_body(const ipc::CreateBody& c) {
        if (c.slot >= max_bodies) return;
        auto it = shapes.find(c.shape);
        if (it == shapes.end() || it->second.empty()) {
            ++stat_no_shape;
            return;
        }
        release_body(c.slot);
        const PxTransform pose(PxVec3(c.pos.x, c.pos.y, c.pos.z), PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized());
        PxRigidDynamic* a = physics->createRigidDynamic(pose);
        for (PxShape* shape : it->second) a->attachShape(*shape);
        if ((c.flags & ipc::kBodyMass) && c.density > 0)
            PxRigidBodyExt::setMassAndUpdateInertia(*a, c.density);
        else
            PxRigidBodyExt::updateMassAndInertia(*a, c.density > 0 ? c.density : 2000.0f);
        // A hull without a volume gives no usable inertia (the piece does not move): a box's of its bounds instead.
        const PxVec3 inertia = a->getMassSpaceInertiaTensor();
        const float mass = a->getMass();
        if (!(mass > 0.0f) || !inertia.isFinite() || !(inertia.minElement() > 1e-7f * mass) ||
            !a->getCMassLocalPose().isFinite()) {
            const float m = mass > 0.0f && std::isfinite(mass) ? mass : (c.density > 0 ? c.density : 10.0f);
            PxBounds3 wb = PxBounds3::empty();
            for (PxShape* shape : it->second) wb.include(PxShapeExt::getWorldBounds(*shape, *a, 1.0f));
            const PxVec3 e = wb.getDimensions().maximum(PxVec3(0.01f));
            a->setMass(m);
            a->setCMassLocalPose(PxTransform(PxIdentity));
            a->setMassSpaceInertiaTensor(PxVec3(e.y * e.y + e.z * e.z, e.x * e.x + e.z * e.z, e.x * e.x + e.y * e.y) *
                                         (m / 12.0f));
            ++stat_inertia_fixed;
        }
        a->setSolverIterationCounts(4, 1);
        if (!(c.flags & ipc::kBodyKinematic)) a->setMaxLinearVelocity((c.flags & ipc::kBodyChunk) ? kChunkMaxSpeed : kPieceMaxSpeed);
        const bool gentle_start = (c.flags & ipc::kBodyGentle) != 0;
        a->setMaxDepenetrationVelocity(gentle_start ? kGentleDepenetration : kMaxDepenetration);
        if (gentle_start) gentle.emplace_back(c.slot, step_index + kGentleSteps);
        a->setLinearVelocity(PxVec3(c.lin_vel.x, c.lin_vel.y, c.lin_vel.z));
        a->setAngularVelocity(PxVec3(c.ang_vel.x, c.ang_vel.y, c.ang_vel.z));
        a->userData = reinterpret_cast<void*>(uintptr_t(c.slot));
        // No speculative CCD: it grows every moving piece's bounds by how far it moves in a step, and pieces thrown at
        // 20-50 m/s through a crowd got so many more pairs that steps took 10-25 times as long (B5b: 300-600 ms with
        // 20,000-30,000 awake, then the GPU ran out of pinned memory). The pieces it was for had found no terrain at all
        // (the game's collision model table was full), not tunnelled through it (research/docs/07, B5b).
        if (c.flags & ipc::kBodyKinematic) a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        user_kinematic[c.slot] = (c.flags & ipc::kBodyKinematic) ? 1 : 0;
        pusher[c.slot] = (c.flags & ipc::kBodyKinematic) && (c.flags & ipc::kBodyPusher) ? 1 : 0;
        // A pusher's contacts give at most kPusherMaxImpulse each (between a kinematic and a dynamic body PhysX takes
        // the smaller of their limits): a loose chunk coming down on pieces lying on the ground crushed them between
        // itself and the static ground without a limit, and the solver shot them out underneath (research/docs/07,
        // B5a: through the avenue map's 4 m slab, at up to 300 m/s).
        if (pusher[c.slot]) a->setMaxContactImpulse(kPusherMaxImpulse);
        // A loose chunk here is a dynamic body with its own mass (research/docs/08): it lands on the pieces, with no
        // cap on its contacts, and wakes the frozen ones it is about to reach as a pusher does (a frozen piece is
        // kinematic and would hold it up like the ground).
        chunk[c.slot] = (c.flags & ipc::kBodyChunk) && !(c.flags & ipc::kBodyKinematic) ? 1 : 0;
        if (chunk[c.slot]) {
            // The report threshold is on the pair of actors: past it PhysX reports all their shape pairs, each step
            // while it lasts, and break_parts looks at each part. Without a break force, as good as never.
            a->setContactReportThreshold(c.break_force_kn > 0.0f ? c.break_force_kn * 1000.0f : 3.0f * 9.81f * a->getMass());
            if (c.break_force_kn > 0.0f) chunk_break_force[c.slot] = c.break_force_kn * 1000.0f;
            const auto parts = parts_of_shape.find(c.shape);
            if (parts != parts_of_shape.end()) chunk_parts[c.slot] = parts->second;
        }
        if ((pusher[c.slot] || chunk[c.slot]) &&
            std::find(pusher_slots.begin(), pusher_slots.end(), c.slot) == pusher_slots.end()) {
            pusher_slots.push_back(c.slot);
            pusher_wakes[c.slot] = pusher_woken[c.slot] = 0;  // a pusher before it went out of the list unlogged
        }
        // Made where it stands: it wakes nothing until it moves (frozen pieces it overlaps are kinematic too, so they
        // do not touch it until then).
        pusher_woke_at[c.slot].pos = c.pos;
        pusher_woke_at[c.slot].rot = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
        heavy[c.slot] = (it->second[0]->getSimulationFilterData().word0 & ipc::kShapeHeavy) ? 1 : 0;
        float radius = 0;
        PxBounds3 all_bounds = PxBounds3::empty();
        for (PxShape* shape : it->second) {
            const PxBounds3 b = PxShapeExt::getWorldBounds(*shape, *a, 1.0f);
            radius = std::max(radius, (b.getCenter() - pose.p).magnitude() + b.getExtents().magnitude());
            all_bounds.include(b);
        }
        body_radius[c.slot] = radius;
        if (pusher[c.slot] || chunk[c.slot]) {
            PxBounds3 local = PxBounds3::empty();
            for (PxShape* shape : it->second) {
                PxBounds3 b;
                PxGeometryQuery::computeGeomBounds(b, shape->getGeometry(), shape->getLocalPose(), 0.0f, 1.0f);
                local.include(b);
            }
            pusher_mid[c.slot] = local.getCenter();
            pusher_half[c.slot] = local.getExtents().magnitude();
            if (chunk[c.slot]) a->setMaxAngularVelocity(kChunkMaxSpeed / std::max(pusher_half[c.slot], 1.0f));
        }
        calm[c.slot] = 0;
        awake_since[c.slot] = step_index;
        still_from[c.slot].pos = c.pos;
        still_from[c.slot].rot = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
        still_step[c.slot] = step_index;
        born_step[c.slot] = step_index;
        born_excused[c.slot] = c.lin_vel.y > kBubbleSpeed ? 1 : 0;
        bodies[c.slot].actor = a;
        pending_add.push_back(a);
        if (chunk[c.slot]) pending_chunks.push_back({c.slot, a, c.made_qpc});
        ipc::BodyPose& p = poses[c.slot];
        p.pos = c.pos;
        p.prev_pos = c.pos;
        p.rot = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
        p.prev_rot = p.rot;
        p.flags = ipc::kPoseAlive;
        ++live;
        slots_used = std::max(slots_used, c.slot + 1);
        pose_block_used[c.slot >> kPoseBlockShift] = 1;
        moved_late[c.slot] = 0;
        // A piece that lay still in Havok starts frozen. A whole pile that had settled there came over awake and in
        // contact at once, and the solver could not quite settle it again: hundreds of pieces rocked for good.
        if ((c.flags & ipc::kBodyAtRest) && !user_kinematic[c.slot]) {
            a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            frozen[c.slot] = 1;
            frozen_bounds[c.slot] = all_bounds;
            p.flags = ipc::kPoseAlive | ipc::kPoseFrozen;
            frozen_slots.push_back(c.slot);
            grid_add(c.slot);
            ++frozen_count;
            ++stat_born_frozen;
        }
        // (A loose chunk is not measured: thousands of parts, each against what is near it.)
        if (!user_kinematic[c.slot] && !chunk[c.slot] && ++overlap_count % kOverlapEvery == 0) measure_overlap(a, c.slot);
    }

    // Diagnostic: how deep a piece overlaps what is already here when it comes over (the pieces near it, the static
    // world), which PhysX then pushes out: the bubbling of piles. One piece in kOverlapEvery is measured, with
    // PxGeometryQuery::computePenetration, into the next log line.
    static constexpr uint32_t kOverlapEvery = 32;
    uint32_t overlap_count = 0;
    uint32_t stat_overlap[5] = {};  // measured pieces by their deepest overlap: none, under 1 cm, 1-3, 3-10, deeper
    // Over 3 cm deep into: the static world, a frozen piece, an awake one, a pusher, an awake one that came over in the
    // last kOverlapYoungSteps (the pieces of the same shatter, or of the same collapse).
    uint32_t stat_overlap_deep[5] = {};
    static constexpr uint32_t kOverlapYoungSteps = 30;
    void measure_overlap(PxRigidDynamic* a, uint32_t slot) {
        PxShape* own[16];
        const PxU32 n_own = a->getShapes(own, 16);
        const PxTransform pose = a->getGlobalPose();
        PxBounds3 b = PxBounds3::empty();
        for (PxU32 i = 0; i < n_own; ++i) b.include(PxShapeExt::getWorldBounds(*own[i], *a, 1.0f));
        float deepest = 0.0f, deepest_in[5] = {};
        uint32_t kind = 0;  // what `against` measures: 0 statics, 1 frozen pieces, 2 awake pieces, 3 pushers, 4 young ones
        auto against = [&](const PxGeometry& geom, const PxTransform& at) {
            for (PxU32 i = 0; i < n_own; ++i) {
                PxVec3 dir;
                PxF32 depth = 0.0f;
                if (PxGeometryQuery::computePenetration(dir, depth, own[i]->getGeometry(), pose * own[i]->getLocalPose(),
                                                        geom, at)) {
                    deepest = std::max(deepest, depth);
                    deepest_in[kind] = std::max(deepest_in[kind], depth);
                }
            }
        };
        // The static world, through the scene's queries.
        PxOverlapHit hits[32];
        for (PxU32 i = 0; i < n_own; ++i) {
            PxOverlapBuffer buf(hits, 32);
            scene->overlap(own[i]->getGeometry(), pose * own[i]->getLocalPose(), buf,
                           PxQueryFilterData(PxQueryFlag::eSTATIC | PxQueryFlag::eNO_BLOCK));
            for (PxU32 h = 0; h < buf.getNbTouches(); ++h) {
                const PxOverlapHit& t = buf.getTouch(h);
                against(t.shape->getGeometry(), PxShapeExt::getGlobalPose(*t.shape, *t.actor));
            }
        }
        // The pieces near it (not in the scene's queries): frozen ones through the grid, awake ones by their spheres.
        auto piece = [&](uint32_t f) {
            if (f == slot || !bodies[f].actor) return;
            kind = pusher[f] ? 3 : frozen[f] ? 1 : step_index - born_step[f] < kOverlapYoungSteps ? 4 : 2;
            PxRigidDynamic* o = bodies[f].actor;
            PxShape* other[256];
            const PxU32 n_o = o->getShapes(other, 256);
            const PxTransform op = o->getGlobalPose();
            for (PxU32 j = 0; j < n_o; ++j)
                if (PxShapeExt::getWorldBounds(*other[j], *o, 1.0f).intersects(b))
                    against(other[j]->getGeometry(), op * other[j]->getLocalPose());
        };
        const int x0 = cell_of(b.minimum.x - kCell), x1 = cell_of(b.maximum.x + kCell),
                  y0 = cell_of(b.minimum.y - kCell), y1 = cell_of(b.maximum.y + kCell),
                  z0 = cell_of(b.minimum.z - kCell), z1 = cell_of(b.maximum.z + kCell);
        if (x1 - x0 < 16 && y1 - y0 < 16 && z1 - z0 < 16)
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    for (int z = z0; z <= z1; ++z) {
                        auto it = frozen_grid.find(cell_key(x, y, z));
                        if (it == frozen_grid.end()) continue;
                        for (uint32_t f : it->second)
                            if (frozen[f]) piece(f);
                    }
        const float r = body_radius[slot];
        for (uint32_t f : awake_slots) {
            const ipc::Vec3& q = poses[f].pos;
            const PxVec3 d(q.x - pose.p.x, q.y - pose.p.y, q.z - pose.p.z);
            if (!frozen[f] && d.magnitude() < r + body_radius[f]) piece(f);
        }
        for (uint32_t f : pusher_slots)  // kinematic copies of vehicles and loose chunks: not in the grid
            if (pusher[f] && bodies[f].actor && bodies[f].actor->getScene() &&
                bodies[f].actor->getWorldBounds().intersects(b))
                piece(f);
        ++stat_overlap[deepest <= 0.0f ? 0 : deepest < 0.01f ? 1 : deepest < 0.03f ? 2 : deepest < 0.1f ? 3 : 4];
        for (uint32_t k = 0; k < 5; ++k) stat_overlap_deep[k] += deepest_in[k] > 0.03f ? 1 : 0;
    }

    // addActors adds all or none: it backs out at the first actor it refuses, and once the GPU has failed (its scene
    // corrupt) it refuses all. A body left out of the scene crashed the next setKinematicTarget on it (null scene +
    // 0x19EC, B5b), so those go one by one, and what is still out is given up (its slot no longer alive).
    uint32_t stat_add_refused = 0;
    float stat_fastest2 = 0;  // the fastest moving piece's speed squared, since the last stats
    float stat_fastest_up = 0;  // the fastest upward (a falling piece says nothing), m/s
    // A chunk comes in at once, not with the next change: the game's copy is flying on until its first pose. From where it
    // is by the time that pose is drawn: the commands' wait, and the step that makes it.
    struct PendingChunk {
        uint32_t slot;
        PxRigidDynamic* actor;
        int64_t made_qpc;
    };
    std::vector<PendingChunk> pending_chunks;
    void start_chunk_late(const PendingChunk& c) {
        PxRigidDynamic* a = c.actor;
        const PxVec3 lin = a->getLinearVelocity();
        if (lin.magnitudeSquared() < 0.25f) return;  // as slow as this the game's copy waits for it where it is
        const float t = std::clamp(float(double(qpc() - c.made_qpc) / double(g_freq.QuadPart)) + dt, 0.0f, 0.15f);
        const PxVec3 ang = a->getAngularVelocity(), gravity(0.0f, -9.81f, 0.0f);
        const PxTransform pose = a->getGlobalPose(), cm = a->getCMassLocalPose();
        const float w = ang.magnitude();
        const PxQuat q = w * t > 1e-5f ? (PxQuat(w * t, ang / w) * pose.q).getNormalized() : pose.q;
        const PxVec3 com = pose.transform(cm.p) + lin * t + gravity * (0.5f * t * t);
        const PxTransform moved(com - q.rotate(cm.p), q);
        a->setGlobalPose(moved);
        a->setLinearVelocity(lin + gravity * t);
        ipc::BodyPose& p = poses[c.slot];
        p.pos = p.prev_pos = {moved.p.x, moved.p.y, moved.p.z};
        p.rot = p.prev_rot = {moved.q.x, moved.q.y, moved.q.z, moved.q.w};
    }

    void flush_pending() {
        if (pending_add.empty()) return;
        changes |= 1u << kChangeNew;
        for (const PendingChunk& c : pending_chunks)
            if (bodies[c.slot].actor == c.actor) start_chunk_late(c);
        pending_chunks.clear();
        HOST_ZONE("host.drain.flush_pending");
        if (!scene->addActors(pending_add.data(), PxU32(pending_add.size())))
            for (PxActor* a : pending_add)
                if (!a->getScene()) scene->addActor(*a);
        std::vector<uint32_t> refused;
        for (PxActor* a : pending_add)
            if (!a->getScene()) refused.push_back(uint32_t(reinterpret_cast<uintptr_t>(a->userData)));
        pending_add.clear();
        for (uint32_t s : refused) release_body(s);
        stat_add_refused += uint32_t(refused.size());
        if (!impulses_waiting.empty()) {
            std::vector<ipc::Impulse> now;
            now.swap(impulses_waiting);
            for (const ipc::Impulse& c : now) impulse(c);
        }
    }

    // ---- the static world mirror ----

    void count_statics(int delta) {
        static_count = uint32_t(int(static_count) + delta);
        sh.control->statics.store(static_count, std::memory_order_relaxed);
    }

    void release_group(Group& g) {
        for (PxRigidStatic* st : g.pieces) st->release();
        for (PxRigidStatic* st : g.actors) st->release();
        count_statics(-int(g.shape_count));
        g = Group{};
    }

    void add_static_actor(uint32_t group, PxRigidStatic* st) {
        st->userData = reinterpret_cast<void*>(uintptr_t(ipc::kEventStatic | group));
        shape_keys_unknown(st);
        scene->addActor(*st);
        Group& g = groups[group];
        g.actors.push_back(st);
        ++g.shape_count;
        count_statics(1);
    }

    void static_mesh(const ipc::StaticMesh& c, uint32_t size) {
        const size_t need = sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) + size_t(c.triangle_count) * 12;
        if (c.vertex_count < 3 || c.triangle_count == 0 || need > size) return;
        const auto* v = reinterpret_cast<const ipc::Vec3*>(&c + 1);
        const auto* idx = reinterpret_cast<const uint32_t*>(v + c.vertex_count);
        const double t0 = now_ms();
        PxCookingParams params(physics->getTolerancesScale());
        params.buildGPUData = gpu;
        PxTriangleMeshDesc desc;
        desc.points.count = c.vertex_count;
        desc.points.stride = sizeof(ipc::Vec3);
        desc.points.data = v;
        desc.triangles.count = c.triangle_count;
        desc.triangles.stride = 12;
        desc.triangles.data = idx;
        PxTriangleMesh* mesh = PxCreateTriangleMesh(params, desc, physics->getPhysicsInsertionCallback());
        stat_cook_ms += now_ms() - t0;
        if (!mesh) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        PxRigidStatic* st = physics->createRigidStatic(PxTransform(PxIdentity));
        PxRigidActorExt::createExclusiveShape(*st, PxTriangleMeshGeometry(mesh), *material);
        mesh->release();
        add_static_actor(c.group, st);
    }

    void static_convex(const ipc::StaticConvex& c, uint32_t size) {
        if (c.vertex_count < 3 || sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) > size) return;
        const double t0 = now_ms();
        PxVec3 centre;
        PxConvexMesh* mesh = cook_hull(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count, c.radius, centre);
        stat_cook_ms += now_ms() - t0;
        if (!mesh) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        add_static_piece(c.group, c.key, mesh, PxTransform(centre));
        mesh->release();  // the shape keeps its reference
    }

    void add_static_piece(uint32_t group, uint32_t key, PxConvexMesh* mesh, const PxTransform& pose) {
        Group& g = groups[group];
        if (g.pieces.empty() || g.pieces.back()->getNbShapes() >= kPiecesPerActor) {
            PxRigidStatic* st = physics->createRigidStatic(PxTransform(PxIdentity));
            st->userData = reinterpret_cast<void*>(uintptr_t(ipc::kEventStatic | group));  // a chunk's impacts
            scene->addActor(*st);
            g.pieces.push_back(st);
        }
        changes |= 1u << kChangeStatics;
        PxShape* shape = PxRigidActorExt::createExclusiveShape(*g.pieces.back(), PxConvexMeshGeometry(mesh), *material);
        if (!shape) return;
        shape->setLocalPose(pose);
        shape->userData = reinterpret_cast<void*>(uintptr_t(key));
        g.by_key[key].push_back(shape);
        ++g.shape_count;
        count_statics(1);
    }

    // Static hulls alike are cooked once and shared (research/docs/07, B5b): a building's modules repeat on every
    // storey and each avenue type stands four times, and the avenue's 390,000 hulls took 33-43 s to cook one by one,
    // the debris waiting in Havok all that time. In a piece's own frame (StaticConvexPlaced) the points of pieces alike
    // are the same to the bit. The cache holds a reference of each mesh: the ones no shape uses any more go when it has
    // doubled since it was last swept, and all of them on a reset.
    struct CachedHull {
        std::vector<ipc::Vec3> points;
        float radius;
        PxConvexMesh* mesh;  // null: it did not cook
        PxVec3 centre;
    };
    static constexpr size_t kHullCacheSweep = 65536;
    std::unordered_map<uint64_t, std::vector<CachedHull>> hull_cache;  // by a hash of the points and the radius
    size_t hull_cache_count = 0, hull_cache_sweep_at = kHullCacheSweep;
    uint32_t stat_hulls_alike = 0;

    // The cache's mesh (a shape takes a reference of its own) and the centre it was cooked around.
    PxConvexMesh* cached_hull(const ipc::Vec3* v, uint32_t count, float radius, PxVec3& centre) {
        uint64_t h = 1469598103934665603ull ^ count;
        uint32_t word;
        for (size_t i = 0; i < size_t(count) * 3; ++i) {
            memcpy(&word, reinterpret_cast<const float*>(v) + i, 4);
            h = (h ^ word) * 1099511628211ull;
        }
        memcpy(&word, &radius, 4);
        h = (h ^ word) * 1099511628211ull;
        const size_t bytes = size_t(count) * sizeof(ipc::Vec3);
        if (auto it = hull_cache.find(h); it != hull_cache.end())
            for (const CachedHull& e : it->second)
                if (e.points.size() == count && !memcmp(&e.radius, &radius, 4) && !memcmp(e.points.data(), v, bytes)) {
                    ++stat_hulls_alike;
                    centre = e.centre;
                    return e.mesh;
                }
        if (hull_cache_count >= hull_cache_sweep_at) sweep_hull_cache();
        PxConvexMesh* mesh = cook_hull(v, count, radius, centre);
        hull_cache[h].push_back({std::vector<ipc::Vec3>(v, v + count), radius, mesh, centre});
        ++hull_cache_count;
        return mesh;
    }

    void sweep_hull_cache() {
        hull_cache_count = 0;
        for (auto it = hull_cache.begin(); it != hull_cache.end();) {
            std::vector<CachedHull>& bucket = it->second;
            for (size_t i = 0; i < bucket.size();) {
                CachedHull& e = bucket[i];
                if (e.mesh && e.mesh->getReferenceCount() > 1) {
                    ++i;
                    continue;
                }
                if (e.mesh) e.mesh->release();
                e = std::move(bucket.back());
                bucket.pop_back();
            }
            hull_cache_count += bucket.size();
            if (bucket.empty()) it = hull_cache.erase(it);
            else ++it;
        }
        hull_cache_sweep_at = std::max(kHullCacheSweep, 2 * hull_cache_count);
    }

    void clear_hull_cache() {
        for (auto& [h, bucket] : hull_cache)
            for (CachedHull& e : bucket)
                if (e.mesh) e.mesh->release();
        hull_cache.clear();
        hull_cache_count = 0;
        hull_cache_sweep_at = kHullCacheSweep;
    }

    // A body shape's part in its own frame (a loose chunk's: research/docs/08), its hull from the cache (the building
    // pieces it is made of were cooked for the static world), placed in the body's frame. A part that does not cook is
    // left out.
    PxShape* make_convex_placed(const ipc::ShapePartPlaced& c) {
        PxVec3 centre;
        PxConvexMesh* mesh = cached_hull(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count, c.radius, centre);
        const PxQuat rot = PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized();
        const PxVec3 pos(c.pos.x, c.pos.y, c.pos.z);
        if (!mesh || !rot.isFinite() || !pos.isFinite()) return nullptr;
        PxShape* shape = physics->createShape(PxConvexMeshGeometry(mesh), *material, false, PxShapeFlag::eSIMULATION_SHAPE);
        if (shape) {
            shape->setLocalPose(PxTransform(pos + rot.rotate(centre), rot));
            shape->userData = reinterpret_cast<void*>(uintptr_t(c.part));  // the part an impact hit (ContactEvent)
        }
        return shape;
    }

    // A static actor's shapes other than placed pieces (a height field, a mesh): no key (ContactEvent::other_key).
    void shape_keys_unknown(PxRigidStatic* st) {
        PxShape* found[8];
        const PxU32 n = st->getShapes(found, 8);
        for (PxU32 i = 0; i < n; ++i) found[i]->userData = reinterpret_cast<void*>(uintptr_t(ipc::kEventNone));
    }

    // The loose chunks break under their load (research/docs/08 §12). A chunk's contacts are hard, so nothing goes through
    // anything. The contact reports (filter_shader) say what each part carries in a step: new touches, and pairs of
    // actors over the chunk's report threshold, its break force. A part carrying more than that breaks off here and now:
    // its shape leaves the body and the game hears of it (the event ring) and breaks the subpiece. The chunk then goes on
    // as if that part had not been there: what stopped it in this step is undone, for the share of the step's impulse the
    // broken parts carried, less the energy they took (ShapePartPlaced::break_energy). So a block coming down eats its way
    // through its lower floors until its energy is spent, and then lies on what is left if that carries it; if it does
    // not, the most loaded part gives way, one a step, whatever the energy: it settles under its own weight, half broken.
    // A small chunk's load hardly reaches the break force. A capped contact (setMaxContactImpulse) gave the same crushing
    // and went through the ground: past the ground's middle PhysX pushes a part down, and a block whose contacts could
    // not stop it fell out under the map. Not in a chunk's first kEventQuietSteps: it is made where Havok had it, often
    // in what it fell into meanwhile.
    static constexpr uint32_t kEventQuietSteps = 10;
    static constexpr uint32_t kBreaksPerStep = 64;  // detaching a shape is not free, nor is the game's breaking
    struct ChunkPart {
        PxShape* shape = nullptr;
        float break_energy = 0;
        float break_strength = 1.0f;
    };
    std::unordered_map<uint32_t, std::vector<ChunkPart>> parts_of_shape;  // by shape id: until a body takes them
    std::unordered_map<uint32_t, std::vector<ChunkPart>> chunk_parts;     // by slot
    std::unordered_map<uint32_t, float> chunk_break_force;                // by slot: N on a part that breaks it
    struct ChunkBefore {
        PxVec3 lin{0.0f}, ang{0.0f};  // before this step
        double kinetic = 0;           // J
        float impulse = 0;            // N*s: all its reported contacts in this step
    };
    std::unordered_map<uint32_t, ChunkBefore> chunk_before;  // by slot
    struct PartLoad {
        ipc::ContactEvent e;  // the point, normal and what pressed: of its strongest pair
        float impulse;        // N*s: its pairs together
        float best;
    };
    std::vector<PartLoad> part_loads;
    std::unordered_map<uint64_t, uint32_t> part_load_index;  // (slot, part) -> part_loads, while reports arrive
    std::vector<ipc::ContactEvent> parts_broken;
    uint64_t stat_events = 0, stat_events_dropped = 0, stat_event_pairs = 0, stat_parts_broken = 0;
    double stat_break_energy = 0;

    void measure_chunks_before() {
        HOST_ZONE("host.measure_chunks");
        for (uint32_t s : pusher_slots) {
            if (!chunk[s] || !bodies[s].actor || frozen[s]) continue;
            PxRigidDynamic* a = bodies[s].actor;
            ChunkBefore& b = chunk_before[s];
            b.lin = a->getLinearVelocity();
            b.ang = a->getAngularVelocity();
            const PxVec3 I = a->getMassSpaceInertiaTensor();
            const PxVec3 w = (a->getGlobalPose() * a->getCMassLocalPose()).q.rotateInv(b.ang);
            b.kinetic = 0.5 * double(a->getMass()) * b.lin.magnitudeSquared() +
                        0.5 * (double(I.x) * w.x * w.x + double(I.y) * w.y * w.y + double(I.z) * w.z * w.z);
            b.impulse = 0;
        }
    }

    // After the step and its reports.
    // Where each chunk bears on what holds it, now and then: what it may snap under is the game's side's to work out (research/docs/08 §17).
    static constexpr uint32_t kSupportEverySteps = 30;
    static constexpr size_t kSupportMinParts = 100;  // the game's side does not snap smaller ones (kCrackMinParts)
    struct Support {
        PxBounds3 box = PxBounds3::empty();  // in the chunk's frame
        float impulse = 0;
    };
    std::unordered_map<uint32_t, Support> chunk_support;
    void note_support() {
        for (const PartLoad& l : part_loads) {
            if (l.e.normal.y < 0.3f) continue;  // from below: what holds it, not what lies on it
            const auto parts = chunk_parts.find(l.e.slot);
            if (parts == chunk_parts.end() || parts->second.size() < kSupportMinParts || !bodies[l.e.slot].actor) continue;
            Support& support = chunk_support[l.e.slot];
            support.box.include(bodies[l.e.slot].actor->getGlobalPose().transformInv(PxVec3(l.e.point.x, l.e.point.y, l.e.point.z)));
            support.impulse += l.impulse;
        }
        if (step_index % kSupportEverySteps) return;
        for (const auto& [slot, support] : chunk_support) {
            if (!chunk[slot] || !bodies[slot].actor || frozen[slot] || !(support.impulse > 0.0f)) continue;
            ipc::ContactEvent e{};
            e.slot = slot;
            e.part = e.other = e.other_key = ipc::kEventNone;
            e.normal = {support.box.minimum.x, support.box.minimum.y, support.box.minimum.z};
            e.velocity = {support.box.maximum.x, support.box.maximum.y, support.box.maximum.z};
            e.total = support.impulse / (float(kSupportEverySteps) * dt);
            e.step = step_index;
            parts_broken.push_back(e);
        }
        chunk_support.clear();
    }

    // A part that broke against rubble or another chunk pressed on what is under them: a piece of a building straight under the
    // contact takes the load through them (research/docs/08 §17). Rubble lies between a falling block and its stump from the first
    // moment, so nearly nothing touches the stump itself. Straight under: rubble on the ground beside a wall leaves the wall alone.
    // As far as a floor under a room: most of a stump is air.
    static constexpr float kThroughRubble = 4.0f;  // m
    void building_under(ipc::ContactEvent& e) {
        e.under = e.under_key = ipc::kEventNone;
        if (e.other != ipc::kEventNone && (e.other & ipc::kEventStatic)) return;  // it touched one
        if (!(e.normal.y > 0.3f)) return;  // from the side or from above: nothing presses down on what is under it
        PxRaycastBuffer hit;
        if (!scene->raycast(PxVec3(e.point.x, e.point.y, e.point.z), PxVec3(0.0f, -1.0f, 0.0f), kThroughRubble, hit, PxHitFlag::eDEFAULT,
                            PxQueryFilterData(PxQueryFlag::eSTATIC)) ||
            !hit.hasBlock)
            return;
        const uintptr_t group = reinterpret_cast<uintptr_t>(hit.block.actor->userData);
        const uint32_t key = uint32_t(reinterpret_cast<uintptr_t>(hit.block.shape->userData));
        if (!(group & ipc::kEventStatic) || key == ipc::kEventNone) return;
        e.under = uint32_t(group);
        e.under_key = key;
        ++stat_through_rubble;
    }
    uint64_t stat_through_rubble = 0;

    void break_parts() {
        note_support();
        if (part_loads.empty()) return;
        std::sort(part_loads.begin(), part_loads.end(),
                  [](const PartLoad& x, const PartLoad& y) { return x.impulse > y.impulse; });
        struct Broke {
            double kinetic_left;
            size_t first_event = 0;  // in parts_broken
            double owed = 0;
            float impulse = 0;
            PxVec3 at{0.0f}, normal{0.0f};  // by what each part took
            PxBounds3 freed = PxBounds3::empty();
        };
        std::unordered_map<uint32_t, Broke> broke;
        uint32_t room = kBreaksPerStep;
        for (PartLoad& l : part_loads) {
            if (!room) break;
            const uint32_t s = l.e.slot;
            if (!chunk[s] || !bodies[s].actor || frozen[s]) continue;
            const auto limit = chunk_break_force.find(s);
            if (limit == chunk_break_force.end()) continue;
            const auto parts = chunk_parts.find(s);
            if (parts == chunk_parts.end() || l.e.part >= parts->second.size()) continue;
            ChunkPart& part = parts->second[l.e.part];
            const float part_limit = limit->second * part.break_strength * dt;
            if (!part.shape || !(part.break_energy > 0.0f) || l.impulse < part_limit) continue;
            const ChunkBefore& before = chunk_before[s];
            PxRigidDynamic* a = bodies[s].actor;
            const PxVec3 hit_at(l.e.point.x, l.e.point.y, l.e.point.z), hit_normal(l.e.normal.x, l.e.normal.y, l.e.normal.z);
            // Lying still it takes more than when hit. Over half of what broke went at under 1 m/s, a chunk melting into the
            // pile under its own weight, one part a step, straight down, and waking all around it as it went (research/docs/08 §18).
            if (rest_strength > 1.0f && l.impulse < part_limit * rest_strength) {
                const PxVec3 r = hit_at - (a->getGlobalPose() * a->getCMassLocalPose()).p;
                const float into = -(before.lin + before.ang.cross(r)).dot(hit_normal);
                const float lying = PxClamp((kHitSpeed - into) / (kHitSpeed - kLieSpeed), 0.0f, 1.0f);
                if (l.impulse < part_limit * (1.0f + (rest_strength - 1.0f) * lying)) continue;
            }
            const auto owing = crush_owed.find(s);
            const auto [it, first] = broke.try_emplace(s, Broke{std::max(0.0, before.kinetic - (owing != crush_owed.end() ? owing->second.joules : 0.0))});
            Broke& b = it->second;
            if (first) b.first_event = parts_broken.size();
            if (!first && b.kinetic_left < part.break_energy) continue;  // spent: this one holds, if the others hold it
            if (a->getScene()) b.freed.include(PxShapeExt::getWorldBounds(*part.shape, *a, 1.0f));
            // It touches nothing from the next step on, and leaves the broad phase with the next change: while a block
            // crushes, a part breaks nearly every step (research/docs/08 §14).
            PxFilterData filter = part.shape->getSimulationFilterData();
            filter.word0 |= ipc::kShapeGone;
            part.shape->setSimulationFilterData(filter);
            parts_to_detach.push_back({a, part.shape});
            part.shape = nullptr;
            // At most half of what it has left at a time: a part takes far more than a slow or small chunk has, and each
            // one stopped it dead (research/docs/08 §15).
            const double cost = std::min(double(part.break_energy), 0.5 * b.kinetic_left);
            b.kinetic_left -= cost;
            b.owed += cost;
            b.impulse += l.impulse;
            b.at += hit_at * float(cost);
            b.normal += hit_normal * float(cost);
            l.e.energy = part.break_energy;
            l.e.total = first ? float(before.kinetic) : 0.0f;
            l.e.velocity = {before.lin.x, before.lin.y, before.lin.z};
            l.e.step = step_index;
            building_under(l.e);
            parts_broken.push_back(l.e);
            ++trace_broke[s];
            --room;
            ++stat_parts_broken;
            stat_break_energy += double(part.break_energy);
        }
        for (const auto& [s, b] : broke) {
            const ChunkBefore& before = chunk_before[s];
            PxRigidDynamic* a = bodies[s].actor;
            const float share = before.impulse > 0.0f ? std::min(1.0f, b.impulse / before.impulse) : 0.0f;
            const PxVec3 lin = a->getLinearVelocity(), ang = a->getAngularVelocity();
            // Its speed comes back for the share the broken parts carried; what they took it pays over the next steps.
            PxVec3 now_lin = lin + (before.lin - lin) * share;
            // Too heavy for what is under it, it keeps sinking through it, instead of stopping on each layer until that is
            // eaten and then dropping to the next. As far as what broke carried its weight: the other parts still hold it.
            const auto limit = chunk_break_force.find(s);
            if (limit != chunk_break_force.end() && a->getMass() * 9.81f > limit->second && now_lin.y > -kCrushSink * share)
                now_lin.y = -kCrushSink * share;
            const PxVec3 now_ang = ang + (before.ang - ang) * share;
            a->setLinearVelocity(now_lin);
            a->setAngularVelocity(now_ang);
            if (b.owed > 0.0) {
                CrushDebt& debt = crush_owed[s];
                const float mine = float(b.owed / (debt.joules + b.owed));
                const PxVec3 at = a->getGlobalPose().transformInv(b.at / float(b.owed));
                debt.at = debt.at + (at - debt.at) * mine;
                const PxVec3 normal = b.normal / float(b.owed);
                debt.normal = (debt.normal + (normal - debt.normal) * mine).getNormalized();
                debt.joules += b.owed;
            }
            // What it lost in this step: what its contacts kept from it, and what the broken parts took, which it pays later.
            const PxVec3 I = a->getMassSpaceInertiaTensor();
            const PxVec3 w = (a->getGlobalPose() * a->getCMassLocalPose()).q.rotateInv(now_ang);
            const double now_kinetic = 0.5 * double(a->getMass()) * now_lin.magnitudeSquared() +
                                       0.5 * (double(I.x) * w.x * w.x + double(I.y) * w.y * w.y + double(I.z) * w.z * w.z);
            if (b.first_event < parts_broken.size()) parts_broken[b.first_event].lost = float(std::max(0.0, before.kinetic - now_kinetic) + b.owed);
            if (!b.freed.isEmpty()) wake_unsupported(b.freed);
        }
        part_loads.clear();
        part_load_index.clear();
    }

    // What the broken parts took comes off a chunk at no more than kCrushDecel, a little each step, where they broke: an impulse
    // at that point along the contact's normal, so a block coming down on one corner turns about it and one landing on a slope is
    // turned aside. Taken off its speed and spin alike, as it was, a crushing chunk could neither tip nor be deflected and sank
    // straight down where it was (research/docs/08 §18). The impulse only ever takes energy: it stops at that point's stopping.
    static constexpr float kCrushDecel = 30.0f;  // m/s^2; gravity takes 10 of it back
    static constexpr float kCrushSink = 0.25f;   // m/s
    static constexpr float kLieSpeed = 0.5f, kHitSpeed = 2.0f;  // m/s into the contact: up to here it lies, from here it is hit
    float rest_strength = 1.0f;       // Control::chunk_rest_strength
    bool crush_at_contact = true;     // Control::chunk_crush_at_contact
    struct CrushDebt {
        double joules = 0;
        PxVec3 at{0.0f};                  // in its frame
        PxVec3 normal{0.0f, 1.0f, 0.0f};  // world, toward the chunk
    };
    std::unordered_map<uint32_t, CrushDebt> crush_owed;  // by slot

    // What the GPU has in use, all programs together, from the driver's NVML: PhysX's own heap leaves out the game and the rest.
    struct NvmlMemory {
        unsigned long long total, free, used;
    };
    int (*nvml_memory)(void*, NvmlMemory*) = nullptr;
    void* nvml_device = nullptr;
    bool nvml_tried = false;
    unsigned long long vram_used = 0, vram_peak = 0, vram_total = 0;
    void vram_read() {
        if (!nvml_tried) {
            nvml_tried = true;
            if (HMODULE m = LoadLibraryW(L"nvml.dll")) {
                const auto init = reinterpret_cast<int (*)()>(GetProcAddress(m, "nvmlInit_v2"));
                const auto device = reinterpret_cast<int (*)(unsigned, void**)>(GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2"));
                const auto memory = reinterpret_cast<int (*)(void*, NvmlMemory*)>(GetProcAddress(m, "nvmlDeviceGetMemoryInfo"));
                if (init && device && memory && init() == 0 && device(0, &nvml_device) == 0) nvml_memory = memory;
            }
        }
        NvmlMemory now{};
        if (!nvml_memory || nvml_memory(nvml_device, &now) != 0) return;
        vram_used = now.used;
        vram_total = now.total;
        vram_peak = std::max(vram_peak, now.used);
    }
    void pay_crush() {
        for (auto it = crush_owed.begin(); it != crush_owed.end();) {
            PxRigidDynamic* a = chunk[it->first] && !frozen[it->first] ? bodies[it->first].actor : nullptr;
            const PxVec3 lin = a ? a->getLinearVelocity() : PxVec3(0.0f);
            const double speed2 = double(lin.magnitudeSquared());
            if (!a || (speed2 < 1e-6 && a->getAngularVelocity().magnitudeSquared() < 1e-6f)) {
                it = crush_owed.erase(it);
                continue;
            }
            CrushDebt& debt = it->second;
            const double mass = double(a->getMass());
            if (!crush_at_contact) {  // off its speed and spin alike
                const double speed = std::sqrt(speed2);
                double slower = std::max(0.0, speed - double(kCrushDecel * dt));
                double paid = 0.5 * mass * (speed2 - slower * slower);
                if (paid > debt.joules) {
                    paid = debt.joules;
                    slower = std::sqrt(std::max(0.0, speed2 - 2.0 * paid / mass));
                }
                const float keep = speed > 0.0 ? float(slower / speed) : 0.0f;
                a->setLinearVelocity(lin * keep);
                a->setAngularVelocity(a->getAngularVelocity() * keep);
                debt.joules -= paid;
                it = debt.joules <= 1.0 || slower <= 0.0 ? crush_owed.erase(it) : std::next(it);
                continue;
            }
            const PxTransform pose = a->getGlobalPose();
            const PxTransform com = pose * a->getCMassLocalPose();
            const PxVec3 r = pose.transform(debt.at) - com.p;
            const PxVec3 ang = a->getAngularVelocity();
            const PxVec3 n = debt.normal;
            const double into = -double((lin + ang.cross(r)).dot(n));
            if (into <= 0.0) {  // that point is leaving: nothing to pay with
                it = crush_owed.erase(it);
                continue;
            }
            // What a unit of impulse there does to its spin, and how heavy it is to that point along the normal.
            const PxVec3 I = a->getMassSpaceInertiaTensor();
            const PxVec3 torque = com.q.rotateInv(r.cross(n));
            const PxVec3 spin_per_impulse = com.q.rotate(PxVec3(torque.x / I.x, torque.y / I.y, torque.z / I.z));
            const double heavy = 1.0 / (1.0 / mass + double(n.dot(spin_per_impulse.cross(r))));
            const bool stops = debt.joules >= 0.5 * heavy * into * into;
            double impulse = stops ? heavy * into : heavy * (into - std::sqrt(into * into - 2.0 * debt.joules / heavy));
            const double most = mass * double(kCrushDecel * dt);
            const bool capped = impulse > most;
            if (capped) impulse = most;
            a->setLinearVelocity(lin + n * float(impulse / mass));
            a->setAngularVelocity(ang + spin_per_impulse * float(impulse));
            debt.joules -= impulse * into - 0.5 * impulse * impulse / heavy;
            it = debt.joules <= 1.0 || (stops && !capped) ? crush_owed.erase(it) : std::next(it);
        }
    }

    struct ContactCallback : PxSimulationEventCallback {
        Host* host = nullptr;
        void onContact(const PxContactPairHeader& h, const PxContactPair* pairs, PxU32 n) override {
            host->on_contact(h, pairs, n);
        }
        void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
        void onWake(PxActor**, PxU32) override {}
        void onSleep(PxActor**, PxU32) override {}
        void onTrigger(PxTriggerPair*, PxU32) override {}
        void onAdvance(const PxRigidBody* const*, const PxTransform*, const PxU32) override {}
    } contact_cb;
    void on_contact(const PxContactPairHeader& h, const PxContactPair* pairs, PxU32 n) {
        if (h.flags & (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1)) return;
        for (int side = 0; side < 2; ++side) {
            const PxActor* a = h.actors[side];
            const PxActor* b = h.actors[1 - side];
            if (!a || a->getType() != PxActorType::eRIGID_DYNAMIC) continue;
            const uint32_t slot = uint32_t(reinterpret_cast<uintptr_t>(a->userData));
            if (slot >= max_bodies || !chunk[slot] || step_index - born_step[slot] < kEventQuietSteps) continue;
            uint32_t other = ipc::kEventNone;
            bool other_chunk = false;
            if (b) {
                const uintptr_t bu = reinterpret_cast<uintptr_t>(b->userData);
                if (b->getType() == PxActorType::eRIGID_DYNAMIC && bu < max_bodies) {
                    other = uint32_t(bu);
                    other_chunk = chunk[other] != 0;
                } else if (b->getType() == PxActorType::eRIGID_STATIC && (bu & ipc::kEventStatic)) {
                    other = uint32_t(bu);
                }
            }
            for (PxU32 i = 0; i < n; ++i) {
                const PxContactPair& p = pairs[i];
                if (p.flags & (PxContactPairFlag::eREMOVED_SHAPE_0 | PxContactPairFlag::eREMOVED_SHAPE_1)) continue;
                if (!(p.events & (PxPairFlag::eNOTIFY_TOUCH_FOUND | PxPairFlag::eNOTIFY_THRESHOLD_FORCE_FOUND |
                                  PxPairFlag::eNOTIFY_THRESHOLD_FORCE_PERSISTS)))
                    continue;
                PxContactPairPoint pts[16];
                const PxU32 m = p.extractContacts(pts, 16);
                float total = 0, best = -1.0f;
                PxU32 bi = 0;
                for (PxU32 k = 0; k < m; ++k) {
                    const float j = pts[k].impulse.magnitude();
                    total += j;
                    if (j > best) best = j, bi = k;
                }
                if (!m || !(total > 0.0f) || !std::isfinite(total)) continue;
                ++stat_event_pairs;
                // PhysX's normal points from the pair's second shape to its first: toward the chunk when it is first.
                const PxVec3 nrm = side == 0 ? pts[bi].normal : -pts[bi].normal;
                const PxShape* own = p.shapes[side];
                const PxShape* theirs = p.shapes[1 - side];
                const uint32_t part = own ? uint32_t(reinterpret_cast<uintptr_t>(own->userData)) : 0;
                const bool keyed = (other != ipc::kEventNone && (other & ipc::kEventStatic)) || other_chunk;
                const uint32_t key = theirs && keyed ? uint32_t(reinterpret_cast<uintptr_t>(theirs->userData))
                                                     : ipc::kEventNone;
                const auto [at, added] =
                    part_load_index.try_emplace(uint64_t(slot) << 32 | part, uint32_t(part_loads.size()));
                if (added) part_loads.push_back(PartLoad{});
                PartLoad& l = part_loads[at->second];
                if (added) {
                    l.e.slot = slot;
                    l.e.part = part;
                    l.impulse = 0;
                    l.best = -1.0f;
                }
                l.impulse += total;
                chunk_before[slot].impulse += total;
                if (total > l.best) {
                    l.best = total;
                    l.e.other = other;
                    l.e.other_key = key;
                    l.e.point = {pts[bi].position.x, pts[bi].position.y, pts[bi].position.z};
                    l.e.normal = {nrm.x, nrm.y, nrm.z};
                }
            }
        }
    }

    // ipc::kTestChunkTrace: each loose chunk, each step. Speeds before the step and after it and its breaking, what it lost
    // and carried: research/docs/08 §15.
    FILE* chunk_steps = nullptr;
    std::unordered_map<uint32_t, uint32_t> trace_broke;  // by slot: parts broken this step
    void trace_chunks() {
        if (!(sh.control->test_flags.load(std::memory_order_relaxed) & ipc::kTestChunkTrace)) return;
        if (!chunk_steps) {
            chunk_steps = _wfsopen((g_log_dir + L"\\chunk_steps.csv").c_str(), L"w", _SH_DENYNO);
            if (!chunk_steps) return;
            fprintf(chunk_steps, "step,ms,slot,shapes,frozen,in_scene,bx,by,bz,vx,vy,vz,kinetic,impulse,broke\n");
        }
        for (uint32_t s : pusher_slots) {
            if (!chunk[s] || !bodies[s].actor) continue;
            PxRigidDynamic* a = bodies[s].actor;
            const ChunkBefore& b = chunk_before[s];
            const PxVec3 v = a->getLinearVelocity();
            const auto n = trace_broke.find(s);
            fprintf(chunk_steps, "%u,%.2f,%u,%u,%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.0f,%.0f,%u\n", step_index, now_ms(), s, a->getNbShapes(),
                    uint32_t(frozen[s]), a->getScene() ? 1u : 0u, double(b.lin.x), double(b.lin.y), double(b.lin.z), double(v.x), double(v.y),
                    double(v.z), b.kinetic, double(b.impulse), n == trace_broke.end() ? 0u : n->second);
        }
        trace_broke.clear();
    }

    void publish_events() {
        if (parts_broken.empty()) return;
        ipc::Control* ctl = sh.control;
        if (!ctl->event_count) {  // a game side without the ring
            parts_broken.clear();
            return;
        }
        auto* ring = reinterpret_cast<ipc::ContactEvent*>(sh.base + ctl->event_offset);
        uint32_t head = ctl->event_head.load(std::memory_order_relaxed);
        const uint32_t tail = ctl->event_tail.load(std::memory_order_acquire);
        for (const ipc::ContactEvent& e : parts_broken) {
            if (head - tail >= ctl->event_count) {
                ++stat_events_dropped;
                continue;
            }
            ring[head & (ctl->event_count - 1)] = e;
            ++head;
            ++stat_events;
        }
        ctl->event_head.store(head, std::memory_order_release);
        parts_broken.clear();
    }

    // A loose chunk's body loses parts where it stands (RemoveParts, research/docs/08 C2c): the game broke them off.
    // Its mass, centre of mass and inertia follow what is left; it wakes, and so does what rested on what went.
    uint32_t stat_parts_removed = 0;
    void remove_parts(const ipc::RemoveParts& c, uint32_t size) {
        if (c.slot >= max_bodies || !chunk[c.slot] || !bodies[c.slot].actor || !c.count ||
            sizeof(c) + size_t(c.count) * sizeof(uint32_t) > size)
            return;
        changes |= 1u << kChangeParts;
        PxRigidDynamic* a = bodies[c.slot].actor;
        const uint32_t* parts = reinterpret_cast<const uint32_t*>(&c + 1);
        const std::unordered_set<uint32_t> gone(parts, parts + c.count);
        std::vector<PxShape*> all(a->getNbShapes());
        a->getShapes(all.data(), uint32_t(all.size()));
        PxBounds3 freed = PxBounds3::empty();
        uint32_t removed = 0;
        for (PxShape* shape : all) {
            if (!gone.count(uint32_t(reinterpret_cast<uintptr_t>(shape->userData)))) continue;
            if (a->getScene()) freed.include(PxShapeExt::getWorldBounds(*shape, *a, 1.0f));
            parts_to_detach.erase(std::remove(parts_to_detach.begin(), parts_to_detach.end(), std::make_pair(a, shape)),
                                  parts_to_detach.end());
            a->detachShape(*shape);
            ++removed;
            const auto parts = chunk_parts.find(c.slot);
            const uint32_t part = uint32_t(reinterpret_cast<uintptr_t>(shape->userData));
            if (parts != chunk_parts.end() && part < parts->second.size()) parts->second[part].shape = nullptr;
        }
        stat_parts_removed += removed;  // none if they broke here first: then this brings the mass that goes with it
        if (a->getNbShapes() && c.mass > 0.0f) {
            PxRigidBodyExt::setMassAndUpdateInertia(*a, c.mass);
            const PxVec3 inertia = a->getMassSpaceInertiaTensor();
            if (!inertia.isFinite() || !(inertia.minElement() > 1e-7f * c.mass) || !a->getCMassLocalPose().isFinite()) {
                PxBounds3 local = PxBounds3::empty();
                std::vector<PxShape*> left(a->getNbShapes());
                a->getShapes(left.data(), uint32_t(left.size()));
                for (PxShape* shape : left) {
                    PxBounds3 b;
                    PxGeometryQuery::computeGeomBounds(b, shape->getGeometry(), shape->getLocalPose(), 0.0f, 1.0f);
                    local.include(b);
                }
                const PxVec3 e = local.getDimensions().maximum(PxVec3(0.01f));
                a->setMass(c.mass);
                a->setCMassLocalPose(PxTransform(local.getCenter()));
                a->setMassSpaceInertiaTensor(
                    PxVec3(e.y * e.y + e.z * e.z, e.x * e.x + e.z * e.z, e.x * e.x + e.y * e.y) * (c.mass / 12.0f));
                ++stat_inertia_fixed;
            }
        }
        if (removed && a->getScene()) {
            if (!frozen[c.slot]) a->wakeUp();
            if (!freed.isEmpty()) wake_unsupported(freed);
        }
    }

    void static_convex_placed(const ipc::StaticConvexPlaced& c, uint32_t size) {
        if (c.vertex_count < 3 || sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) > size) return;
        const double t0 = now_ms();
        PxVec3 centre;
        PxConvexMesh* mesh = cached_hull(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count, c.radius, centre);
        stat_cook_ms += now_ms() - t0;
        if (!mesh) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        const PxQuat rot = PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized();
        const PxVec3 pos(c.pos.x, c.pos.y, c.pos.z);
        if (!rot.isFinite() || !pos.isFinite()) {
            ++stat_cook_failed;
            return;
        }
        add_static_piece(c.group, c.key, mesh, PxTransform(pos + rot.rotate(centre), rot));
    }

    void remove_static_keys(const ipc::RemoveStaticKeys& c, uint32_t size) {
        if (sizeof(c) + size_t(c.count) * 4 > size) return;
        auto it = groups.find(c.group);
        if (it == groups.end() || it->second.pieces.empty()) return;
        Group& g = it->second;
        changes |= 1u << kChangeStatics;
        const auto* keys = reinterpret_cast<const uint32_t*>(&c + 1);
        uint32_t removed = 0;
        PxBounds3 gone = PxBounds3::empty();
        for (uint32_t i = 0; i < c.count; ++i) {
            auto k = g.by_key.find(keys[i]);
            if (k == g.by_key.end()) continue;
            for (PxShape* shape : k->second) {
                PxRigidActor* actor = shape->getActor();
                gone.include(PxShapeExt::getWorldBounds(*shape, *actor, 1.0f));
                actor->detachShape(*shape);
                ++removed;
            }
            g.by_key.erase(k);
        }
        g.shape_count -= removed;
        count_statics(-int(removed));
        wake_unsupported(gone);
    }

    // Heights arrive as int16 samples; PhysX's own cell split runs from (row, column) to (row + 1, column + 1) when a
    // sample's tessellation flag is set.
    void static_height_field(const ipc::StaticHeightField& c, uint32_t size) {
        const size_t samples = size_t(c.rows) * c.columns;
        if (c.rows < 2 || c.columns < 2 || sizeof(c) + samples * 2 > size) return;
        const auto* heights = reinterpret_cast<const int16_t*>(&c + 1);
        const double t0 = now_ms();
        std::vector<PxHeightFieldSample> s(samples);
        for (size_t i = 0; i < samples; ++i) {
            s[i].height = heights[i];
            s[i].materialIndex0 = 0;
            s[i].materialIndex1 = 0;
            if (!c.flip) s[i].setTessFlag(); else s[i].clearTessFlag();
        }
        PxHeightFieldDesc desc;
        desc.nbRows = c.rows;
        desc.nbColumns = c.columns;
        desc.samples.data = s.data();
        desc.samples.stride = sizeof(PxHeightFieldSample);
        PxHeightField* hf = PxCreateHeightField(desc, physics->getPhysicsInsertionCallback());
        stat_cook_ms += now_ms() - t0;
        if (!hf) {
            ++stat_cook_failed;
            return;
        }
        ++stat_cooked;
        const PxTransform pose(PxVec3(c.origin.x, c.origin.y, c.origin.z), PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized());
        PxRigidStatic* st = physics->createRigidStatic(pose);
        PxRigidActorExt::createExclusiveShape(*st, PxHeightFieldGeometry(hf, PxMeshGeometryFlags(), c.height_scale, c.row_scale,
                                                                         c.column_scale), *material);
        hf->release();
        add_static_actor(c.group, st);
        logf("height field %u x %u (cells %.2f x %.2f m, height step %.4f m) for group %u", c.rows, c.columns,
             double(c.row_scale), double(c.column_scale), double(c.height_scale), c.group);
    }

    // A whole body leaves the mirror. Terrain (a height field) goes when its zone streams out, the pieces lying there
    // with it, and frozen pieces near it stay where they are (woken they would drop through the world). Anything
    // else is a building or structure that was destroyed or came loose as a whole (the sphere of panels on Vista):
    // the frozen pieces resting on it wake, or they would hang in the air for good. Sleeping ones PhysX wakes as it
    // loses their contacts. Buildings losing single pieces go through remove_static_keys, which wakes too.
    void remove_statics(uint32_t group) {
        auto it = groups.find(group);
        if (it == groups.end()) return;
        Group& g = it->second;
        PxBounds3 bounds = PxBounds3::empty();
        bool terrain = false;
        for (PxRigidStatic* st : g.pieces) bounds.include(st->getWorldBounds());
        for (PxRigidStatic* st : g.actors) {
            PxShape* shape = nullptr;
            if (st->getShapes(&shape, 1) == 1 && shape->getGeometry().getType() == PxGeometryType::eHEIGHTFIELD)
                terrain = true;
            bounds.include(st->getWorldBounds());
        }
        release_group(g);
        groups.erase(it);
        if (!terrain) {
            const uint32_t woken = wake_unsupported(bounds);
            stat_unsupported_bodies += woken ? 1 : 0;
        }
    }

    void raycast_batch(const ipc::RaycastBatch& c, uint32_t size) {
        const uint32_t n = std::min<uint32_t>(c.count, ipc::kMaxRays);
        if (sizeof(c) + size_t(n) * sizeof(ipc::Ray) > size) return;
        flush_pending();
        // Queries see what the last step left in the scene query structure; statics added since are included by
        // forcing the update.
        scene->flushQueryUpdates();
        const auto* rays = reinterpret_cast<const ipc::Ray*>(&c + 1);
        float* out = reinterpret_cast<float*>(sh.base + sh.control->query_offset);
        uint32_t* kinds = reinterpret_cast<uint32_t*>(out + ipc::kMaxRays);
        const PxQueryFilterData statics_only(PxQueryFlag::eSTATIC);
        for (uint32_t i = 0; i < n; ++i) {
            const ipc::Ray& r = rays[i];
            PxRaycastBuffer hit;
            const bool any = scene->raycast(PxVec3(r.origin.x, r.origin.y, r.origin.z),
                                            PxVec3(r.direction.x, r.direction.y, r.direction.z), r.max_distance, hit,
                                            PxHitFlag::eDEFAULT, statics_only);
            out[i] = any && hit.hasBlock ? hit.block.distance : -1.0f;
            kinds[i] = any && hit.hasBlock && hit.block.shape ? uint32_t(hit.block.shape->getGeometry().getType()) + 1 : 0;
        }
        sh.control->query_done.store(c.request, std::memory_order_release);
        logf("raycast batch %u: %u rays answered", c.request, n);
    }

    void thaw(uint32_t s) {
        if (!frozen[s] || !bodies[s].actor) return;
        HOST_ZONE("host.thaw");
        changes |= 1u << kChangeThaw;
        PxRigidDynamic* a = bodies[s].actor;
        a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, false);
        if (a->getScene()) a->wakeUp();  // not one made at rest, still waiting to come into the scene
        // Frozen in an overlap, at the usual speed it would jump out and tell its neighbours it had moved.
        a->setMaxDepenetrationVelocity(kGentleDepenetration);
        frozen[s] = 0;
        calm[s] = 0;
        still_from[s] = poses[s];
        still_step[s] = step_index;
        poses[s].flags &= ~ipc::kPoseFrozen;
        --frozen_count;
        ++stat_thawed;
    }

    static int cell_of(float v) { return int(std::floor(v / kCell)); }
    static uint64_t cell_key(int x, int y, int z) {
        return (uint64_t(uint32_t(x) & 0x1FFFFF) << 42) | (uint64_t(uint32_t(y) & 0x1FFFFF) << 21) |
               uint64_t(uint32_t(z) & 0x1FFFFF);
    }
    void grid_add(uint32_t s) {
        const ipc::Vec3& p = poses[s].pos;
        frozen_grid[cell_key(cell_of(p.x), cell_of(p.y), cell_of(p.z))].push_back(s);
    }
    void grid_rebuild() {
        frozen_grid.clear();
        for (uint32_t s : frozen_slots)
            if (frozen[s]) grid_add(s);
    }

    // Thaws a frozen piece; if it then moves it passes the news on to what rests on it.
    void wake(uint32_t s) {
        if (!frozen[s]) return;
        thaw(s);
        moved_late[s] = 0;
        wake_pos[s] = poses[s].pos;
        wake_due[s] = step_index + std::max(1u, uint32_t(kCascadeDelaySeconds / dt + 0.5f));
        if (!cascade_mark[s]) {
            cascade_mark[s] = 1;
            cascade_next.push_back(s);
        }
    }

    // At the next change, or now without batches. False: it was on its way already.
    bool wake_soon(uint32_t s) {
        if (change_every == 1) {
            wake(s);
            return true;
        }
        if (thaw_pending[s]) return false;
        thaw_pending[s] = 1;
        thaw_list.push_back(s);
        return true;
    }

    void apply_deferred() {
        if (deferred_cmds.empty()) return;
        std::vector<uint8_t> cmds;
        cmds.swap(deferred_cmds);
        applying_deferred = true;
        for (size_t at = 0; at < cmds.size(); at += reinterpret_cast<const ipc::CmdHeader*>(cmds.data() + at)->size)
            apply(*reinterpret_cast<const ipc::CmdHeader*>(cmds.data() + at));
        applying_deferred = false;
    }

    // A command that can wait: the next change takes it, or this one if it is about to happen.
    bool defer(const ipc::CmdHeader& h) {
        if (applying_deferred || changes_now()) return false;
        const auto* bytes = reinterpret_cast<const uint8_t*>(&h);
        deferred_cmds.insert(deferred_cmds.end(), bytes, bytes + h.size);
        return true;
    }

    void apply_changes() {
        HOST_ZONE("host.apply_changes");
        if (!parts_to_detach.empty()) changes |= 1u << kChangeBroke;
        for (const auto& [a, shape] : parts_to_detach) a->detachShape(*shape);
        parts_to_detach.clear();
        apply_deferred();
        flush_pending();  // first: thawing a body made at rest needs it in the scene
        for (uint32_t s : thaw_list) {
            if (!thaw_pending[s]) continue;  // released meanwhile
            thaw_pending[s] = 0;
            if (frozen[s]) wake(s);
        }
        thaw_list.clear();
        if (!ghosts.empty()) changes |= 1u << kChangeGone;
        for (PxRigidDynamic* a : ghosts) a->release();
        ghosts.clear();
    }

    // Once per step: woken pieces whose check is due, and the frozen pieces resting on those that moved. Only the telling
    // waits for wakes, each check is on time: late, it saw all the creeping since the wake, 40% had "moved" where 2-3% have,
    // and that fed itself for as long as there was a pile (research/docs/08 §15).
    void run_cascade() {
        stat_wakes_waited += wakes_wait();
        while (!unsupported_later.empty() && !wakes_wait()) {
            const PxBounds3 b = unsupported_later.back();
            unsupported_later.pop_back();
            wake_unsupported(b);
        }
        if (cascade_next.empty() && resting_on_later.empty()) return;
        HOST_ZONE("host.cascade");
        size_t keep = 0;
        for (size_t i = 0; i < cascade_next.size(); ++i) {
            const uint32_t t = cascade_next[i];
            if (step_index < wake_due[t]) {
                cascade_next[keep++] = t;  // later
                continue;
            }
            cascade_mark[t] = 0;
            if (!bodies[t].actor) continue;
            ++stat_cascade_looked;
            if (moved_since_woken(t) < kMovedSq) {  // still where it was: it holds what rests on it
                moved_late[t] = 1;
                continue;
            }
            ++stat_cascade_moved;
            tell_what_rested_on(t);
        }
        cascade_next.resize(keep);
        for (uint32_t i = 0; i < kPassOnPerStep && !resting_on_later.empty() && !wakes_wait(); ++i) {
            const auto [lay, t] = resting_on_later.front();
            resting_on_later.pop_front();
            wake_resting_in(lay, t);
        }
    }

    // Diagnostic: frozen pieces with nothing at all under them, what the wakes above could leave behind. Bounds are generous,
    // so this is the least there are. A slab under a piece's bounds, against the static world, frozen pieces and awake ones.
    static constexpr uint32_t kHangingLooks = 64;
    static constexpr float kHangingGap = 0.15f;  // m
    uint32_t hanging_cursor = 0, stat_hanging = 0, stat_hanging_looked = 0;
    bool something_under(uint32_t s) {
        const PxBounds3& fb = frozen_bounds[s];
        const PxBounds3 under(PxVec3(fb.minimum.x, fb.minimum.y - kHangingGap, fb.minimum.z),
                              PxVec3(fb.maximum.x, fb.minimum.y + 0.05f, fb.maximum.z));
        PxOverlapBuffer hit;
        PxQueryFilterData statics_only(PxQueryFlag::eSTATIC | PxQueryFlag::eANY_HIT);
        if (scene->overlap(PxBoxGeometry(under.getExtents()), PxTransform(under.getCenter()), hit, statics_only)) return true;
        const int x0 = cell_of(under.minimum.x - kCell), x1 = cell_of(under.maximum.x + kCell), y0 = cell_of(under.minimum.y - kCell),
                  y1 = cell_of(under.maximum.y + kCell), z0 = cell_of(under.minimum.z - kCell), z1 = cell_of(under.maximum.z + kCell);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) {
                    auto it = frozen_grid.find(cell_key(x, y, z));
                    if (it == frozen_grid.end()) continue;
                    for (uint32_t f : it->second)
                        if (f != s && frozen[f] && under.intersects(frozen_bounds[f])) return true;
                }
        for (uint32_t a : awake_slots) {
            const PxVec3 at(poses[a].pos.x, poses[a].pos.y, poses[a].pos.z);
            if ((at - under.getCenter()).magnitude() < body_radius[a] + under.getExtents().magnitude()) return true;
        }
        for (uint32_t p : pusher_slots)  // chunks, frozen or not: in the grid by their origin, far from their parts
            if (bodies[p].actor && bodies[p].actor->getWorldBounds(1.0f).intersects(under)) return true;
        return false;
    }
    void look_for_hanging() {
        if (frozen_slots.empty()) return;
        for (uint32_t i = 0; i < kHangingLooks; ++i) {
            hanging_cursor += 7919;
            const uint32_t s = frozen_slots[hanging_cursor % frozen_slots.size()];
            if (!frozen[s] || chunk[s]) continue;
            ++stat_hanging_looked;
            stat_hanging += !something_under(s);
        }
    }

    float moved_since_woken(uint32_t s) const {
        const ipc::Vec3& c = poses[s].pos;
        const float mx = c.x - wake_pos[s].x, my = c.y - wake_pos[s].y, mz = c.z - wake_pos[s].z;
        return mx * mx + my * my + mz * mz;
    }

    // What rested on t is where t lay when it froze, however far it has gone since. By its bounds now, fattened by how far it
    // went, a piece that had come 15 m woke everything above it within 15 m: 100-300 pieces each in a real session, and most
    // of them went nowhere. A chunk tells what it reaches by its parts as it goes (run_pushers): by its bounds, 36 m across,
    // one check woke 50,000 pieces.
    void tell_what_rested_on(uint32_t t) {
        if (!chunk[t] && !pusher[t]) resting_on_later.emplace_back(frozen_bounds[t], t);
    }

    // The frozen pieces resting on t, which lay in these bounds.
    void wake_resting_in(PxBounds3 b, uint32_t t) {
        const float bottom = b.minimum.y - 0.1f;
        b.fattenFast(kTouch);
        const int x0 = cell_of(b.minimum.x - kNeighbourReach), x1 = cell_of(b.maximum.x + kNeighbourReach), y0 = cell_of(bottom),
                  y1 = cell_of(b.maximum.y + kNeighbourReach), z0 = cell_of(b.minimum.z - kNeighbourReach),
                  z1 = cell_of(b.maximum.z + kNeighbourReach);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) {
                    auto it = frozen_grid.find(cell_key(x, y, z));
                    if (it == frozen_grid.end()) continue;
                    for (uint32_t f : it->second) {
                        if (!frozen[f] || f == t) continue;
                        const PxBounds3& fb = frozen_bounds[f];
                        if (fb.minimum.y < bottom || !b.intersects(fb)) continue;
                        stat_cascade += wake_soon(f);
                    }
                }
    }

    // Frozen pieces whose bounds reach into the box `b`, found in the grid of frozen pieces, move again.
    uint32_t wake_in_box(const PxBounds3& b) {
        uint32_t n = 0;
        const float reach = kCell;  // a piece in a cell outside the box can still reach into it
        const int x0 = cell_of(b.minimum.x - reach), x1 = cell_of(b.maximum.x + reach), y0 = cell_of(b.minimum.y - reach),
                  y1 = cell_of(b.maximum.y + reach), z0 = cell_of(b.minimum.z - reach), z1 = cell_of(b.maximum.z + reach);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) {
                    auto it = frozen_grid.find(cell_key(x, y, z));
                    if (it == frozen_grid.end()) continue;
                    for (uint32_t f : it->second) {
                        if (!frozen[f]) continue;  // stale
                        if (b.intersects(frozen_bounds[f])) n += wake_soon(f);
                    }
                }
        return n;
    }

    // Frozen pieces near a pusher's parts (each part's bounds, `reach` around them) move again. Its parts, not a sphere
    // around its origin: a loose chunk of a building reaches 30 m and more from its origin, and woke everything that
    // far whenever it moved.
    // Parts close together are looked at as one box (blocks of kPartBlock): a chunk has thousands of them.
    static constexpr float kPartBlock = 4.0f;
    std::unordered_map<uint64_t, PxBounds3> part_blocks;  // scratch
    // A chunk (ahead): only what it is about to reach, each block by how it moves, and what lies on it. The rubble beside a chunk
    // going down cannot be touched by it, and nearly all wakes went nowhere (research/docs/08 §18).
    bool chunk_wake_ahead = true;  // Control::chunk_wake_ahead
    uint32_t wake_near_parts(PxRigidDynamic* a, float reach, bool ahead = false) {
        part_blocks.clear();
        PxShape* parts[256];
        const PxU32 total = a->getNbShapes();
        for (PxU32 start = 0; start < total; start += 256) {
            const PxU32 n = a->getShapes(parts, 256, start);
            for (PxU32 i = 0; i < n; ++i) {
                const PxBounds3 b = PxShapeExt::getWorldBounds(*parts[i], *a, 1.0f);
                const PxVec3 c = b.getCenter() / kPartBlock;
                const auto [it, first] = part_blocks.try_emplace(cell_key(int(std::floor(c.x)), int(std::floor(c.y)), int(std::floor(c.z))), b);
                if (!first) it->second.include(b);
            }
        }
        uint32_t woken = 0;
        const PxVec3 com = (a->getGlobalPose() * a->getCMassLocalPose()).p;
        const PxVec3 lin = a->getLinearVelocity(), ang = a->getAngularVelocity();
        const float look = (kPushLookahead + float(change_every - 1)) * dt;
        for (auto& [key, b] : part_blocks) {
            const PxVec3 v = lin + ang.cross(b.getCenter() - com);
            const float speed = v.magnitude();
            if (!ahead || speed < kWakeAheadSpeed) {
                b.fattenFast(reach);
                woken += wake_in_box(b);
                continue;
            }
            // Past where it goes by the next wake, along how it moves; what touches it now all round; what lies on it.
            const PxVec3 go = v * (look + kPushReach / speed);
            b.fattenFast(kWakeTouching);
            for (int k = 0; k < 3; ++k) (go[k] > 0.0f ? b.maximum[k] : b.minimum[k]) += go[k];
            b.maximum.y = std::max(b.maximum.y, b.maximum.y - kWakeTouching + kPushReach);
            woken += wake_in_box(b);
        }
        return woken;
    }

    // How far any point of pusher s went from pose (pa, qa) to (pb, qb), at most: its shapes' middle, and its turn
    // times how far they reach from there.
    float pusher_moved(uint32_t s, const ipc::Vec3& pa, const ipc::Quat& qa, const ipc::Vec3& pb, const ipc::Quat& qb) const {
        const PxQuat a(qa.x, qa.y, qa.z, qa.w), b(qb.x, qb.y, qb.z, qb.w);
        const PxVec3 ma = PxVec3(pa.x, pa.y, pa.z) + a.rotate(pusher_mid[s]);
        const PxVec3 mb = PxVec3(pb.x, pb.y, pb.z) + b.rotate(pusher_mid[s]);
        const float half_turn_cos = std::min(1.0f, std::fabs(a.dot(b)));
        return (mb - ma).magnitude() + 2.0f * std::sqrt(1.0f - half_turn_cos * half_turn_cos) * pusher_half[s];
    }

    // Once per step: each pusher wakes the frozen pieces ahead of it.
    void run_pushers() {
        if (pusher_slots.empty() || !frozen_count) return;
        HOST_ZONE("host.pushers");
        size_t keep = 0;
        for (size_t i = 0; i < pusher_slots.size(); ++i) {
            const uint32_t s = pusher_slots[i];
            if ((!pusher[s] && !chunk[s]) || !bodies[s].actor) continue;  // gone
            pusher_slots[keep++] = s;
            if (frozen[s] || wakes_wait()) continue;  // it asks again when it can
            const ipc::BodyPose& p = poses[s];
            ipc::BodyPose& at = pusher_woke_at[s];
            // Still about where it last woke what it reaches.
            if (pusher_moved(s, at.pos, at.rot, p.pos, p.rot) < kPushStep) continue;
            at = p;
            const float speed_per_step = pusher_moved(s, p.prev_pos, p.prev_rot, p.pos, p.rot);
            const uint32_t woken = wake_near_parts(bodies[s].actor, kPushReach + (kPushLookahead + float(change_every - 1)) * std::min(speed_per_step, 2.0f),
                                                   chunk[s] && chunk_wake_ahead);
            stat_pushed_awake += woken;
            ++pusher_wakes[s];
            pusher_woken[s] += woken;
        }
        pusher_slots.resize(keep);
    }

    // A push on one piece at a point (a hammer blow, a bullet): it moves again if it was frozen.
    void impulse(const ipc::Impulse& c) {
        if (c.slot >= max_bodies || !bodies[c.slot].actor || user_kinematic[c.slot]) return;
        if (!bodies[c.slot].actor->getScene()) {
            impulses_waiting.push_back(c);
            return;
        }
        if (frozen[c.slot]) wake(c.slot);
        born_excused[c.slot] = 1;
        PxRigidDynamic* a = bodies[c.slot].actor;
        const PxVec3 j(c.impulse.x, c.impulse.y, c.impulse.z), at(c.point.x, c.point.y, c.point.z);
        if (!j.isFinite()) return;
        if (c.mode == ipc::kImpulseAtCentre) {
            a->addForce(j, PxForceMode::eIMPULSE);
        } else {
            if (!at.isFinite()) return;
            PxRigidBodyExt::addForceAtPos(*a, c.mode == ipc::kExplosionVelocityChange ? j * a->getMass() : j, at,
                                          PxForceMode::eIMPULSE);
        }
        ++stat_impulses;
    }

    // A melee blow through a swept sphere (ipc::MeleeSweep): the game's sphere cast does not see the pieces here, so
    // each piece within the sphere's reach of the swept segment gets the push the game gives every object the blow
    // hits, once per blow (blow_of). At the side of it facing the segment, so it tumbles as well.
    std::vector<uint32_t> blow_of;  // per slot: the last blow that pushed it
    uint32_t stat_sweeps = 0, stat_swept = 0, sweeps_logged = 0;
    void melee_sweep(const ipc::MeleeSweep& c) {
        const PxVec3 a(c.start.x, c.start.y, c.start.z), b(c.end.x, c.end.y, c.end.z);
        const PxVec3 j(c.impulse.x, c.impulse.y, c.impulse.z);
        if (!a.isFinite() || !b.isFinite() || !j.isFinite() || !(c.radius > 0.0f) || !(c.max_speed > 0.0f)) return;
        flush_pending();
        const PxVec3 ab = b - a;
        const float len2 = ab.magnitudeSquared();
        uint32_t pushed = 0, woken = 0;
        for (uint32_t s = 0; s < slots_used; ++s) {
            PxRigidDynamic* d = bodies[s].actor;
            if (!d || user_kinematic[s] || blow_of[s] == c.blow) continue;
            const ipc::Vec3& q = poses[s].pos;
            const PxVec3 p(q.x, q.y, q.z);
            const float t = len2 > 0.0f ? PxClamp((p - a).dot(ab) / len2, 0.0f, 1.0f) : 0.0f;
            const PxVec3 axis = a + ab * t;
            const float reach = c.radius + body_radius[s];
            if ((p - axis).magnitudeSquared() >= reach * reach) continue;
            if (frozen[s]) {
                wake(s);
                ++woken;
            }
            if (d->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) continue;
            PxVec3 push = j;
            const float speed = push.magnitude() / std::max(d->getMass(), 0.1f);
            if (speed > c.max_speed) push *= c.max_speed / speed;
            PxVec3 side = axis - p;
            const PxVec3 at = side.normalizeSafe() > 0.0f ? p + side * std::min(0.5f * body_radius[s], 0.5f) : p;
            PxRigidBodyExt::addForceAtPos(*d, push, at, PxForceMode::eIMPULSE);
            born_excused[s] = 1;
            blow_of[s] = c.blow;
            ++pushed;
        }
        ++stat_sweeps;
        stat_swept += pushed;
        if (sweeps_logged < 8) {
            ++sweeps_logged;
            logf("melee sweep from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f), radius %.1f m, impulse %.0f N*s (at most "
                 "%.0f m/s): %u bodies pushed, %u of them woken",
                 a.x, a.y, a.z, b.x, b.y, b.z, c.radius, j.magnitude(), c.max_speed, pushed, woken);
        }
    }

    // Statics went (a building lost pieces, or a whole structure went): frozen pieces in and above their bounds move
    // again. Found in the grid of frozen pieces, or in the list of them when that is shorter; a body larger than
    // 400 m across is not looked at.
    uint32_t wake_unsupported(PxBounds3 b) {
        if (b.isEmpty() || !frozen_count || !b.isFinite()) return 0;
        if (sh.control->test_flags.load(std::memory_order_relaxed) & ipc::kTestNoUnsupportedWakes) return 0;
        if (wakes_wait()) {
            unsupported_later.push_back(b);
            return 0;
        }
        b.minimum -= PxVec3(0.5f, 0.5f, 0.5f);
        b.maximum += PxVec3(0.5f, 3.0f, 0.5f);
        if (b.getDimensions().maxElement() > 400.0f) return 0;
        uint32_t n = 0;
        const int x0 = cell_of(b.minimum.x), x1 = cell_of(b.maximum.x), y0 = cell_of(b.minimum.y),
                  y1 = cell_of(b.maximum.y), z0 = cell_of(b.minimum.z), z1 = cell_of(b.maximum.z);
        const double cells = double(x1 - x0 + 1) * double(y1 - y0 + 1) * double(z1 - z0 + 1);
        if (cells > double(frozen_slots.size())) {
            for (size_t i = 0; i < frozen_slots.size(); ++i) {
                const uint32_t s = frozen_slots[i];
                if (!frozen[s]) continue;
                const ipc::Vec3& p = poses[s].pos;
                if (b.contains(PxVec3(p.x, p.y, p.z)) && wake_soon(s)) {
                    ++stat_unsupported;
                    ++n;
                }
            }
            return n;
        }
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) {
                    auto it = frozen_grid.find(cell_key(x, y, z));
                    if (it == frozen_grid.end()) continue;
                    for (uint32_t s : it->second) {
                        if (!frozen[s]) continue;  // stale
                        const ipc::Vec3& p = poses[s].pos;
                        if (b.contains(PxVec3(p.x, p.y, p.z)) && wake_soon(s)) {
                            ++stat_unsupported;
                            ++n;
                        }
                    }
                }
        return n;
    }

    // Pieces forced through the ground fall under the terrain and were lost, one in ten in a Wrecking Crew match: a
    // walker's foot or a building chunk coming down on them (pushers, kinematic and so infinitely heavy here) pinned
    // them against it. Ten times a second, a piece falling fast whose nearest static straight above it is the terrain
    // (seen from below) is put back on top of it, at rest.
    static constexpr float kFallingSpeed = 6.0f;  // m/s downwards
    uint32_t stat_rescued = 0;
    void rescue_fallen() {
        if (step_index % 6) return;
        HOST_ZONE("host.rescue");
        const PxQueryFilterData statics_only(PxQueryFlag::eSTATIC);
        for (uint32_t s : awake_slots) {
            if (user_kinematic[s] || frozen[s] || !bodies[s].actor) continue;
            PxRigidDynamic* a = bodies[s].actor;
            if (a->getLinearVelocity().y > -kFallingSpeed) continue;
            const PxTransform t = a->getGlobalPose();
            // A chunk's origin is its building's, tens of metres off: its middle tells, and goes to just above the
            // ground. By its origin and its reach from there, it went back up 100 m, for good if it fell through again.
            const PxVec3 from = chunk[s] ? t.transform(pusher_mid[s]) : t.p;
            const float clearance = chunk[s] ? pusher_half[s] : body_radius[s];
            PxRaycastBuffer hit;
            if (!scene->raycast(from, PxVec3(0.0f, 1.0f, 0.0f), 500.0f, hit,
                                PxHitFlag::eDEFAULT | PxHitFlag::eMESH_BOTH_SIDES, statics_only) ||
                !hit.hasBlock || !hit.block.shape ||
                hit.block.shape->getGeometry().getType() != PxGeometryType::eHEIGHTFIELD)
                continue;
            a->setGlobalPose(PxTransform(t.p + PxVec3(0.0f, hit.block.position.y + clearance + 0.05f - from.y, 0.0f), t.q));
            a->setLinearVelocity(PxVec3(0.0f));
            a->setAngularVelocity(PxVec3(0.0f));
            ++stat_rescued;
        }
    }

    // Frozen pieces within `radius` of `c` move again (with light_only, heavy pieces stay: nothing of the player's
    // touches them).
    void thaw_near(const PxVec3& c, float radius, bool light_only = false) {
        const float r2 = radius * radius;
        size_t keep = 0;
        for (size_t i = 0; i < frozen_slots.size(); ++i) {
            const uint32_t s = frozen_slots[i];
            if (!frozen[s]) continue;  // stale
            const ipc::Vec3& p = poses[s].pos;
            const PxVec3 d(p.x - c.x, p.y - c.y, p.z - c.z);
            if (d.magnitudeSquared() < r2 && !(light_only && heavy[s])) wake_soon(s);
            else frozen_slots[keep++] = s;
        }
        frozen_slots.resize(keep);
    }

    void explosion(const ipc::Explosion& e) {
        flush_pending();
        const PxVec3 c(e.center.x, e.center.y, e.center.z);
        const float r2 = e.radius * e.radius;
        {
            const float wake_r2 = r2 * 2.25f;  // 1.5 x the radius
            for (size_t i = 0; i < frozen_slots.size(); ++i) {
                const uint32_t s = frozen_slots[i];
                if (!frozen[s]) continue;
                const ipc::Vec3& p = poses[s].pos;
                const PxVec3 d(p.x - c.x, p.y - c.y, p.z - c.z);
                if (d.magnitudeSquared() < wake_r2) wake(s);
            }
        }
        uint32_t pushed = 0;
        for (uint32_t s = 0; s < slots_used; ++s) {
            PxRigidDynamic* a = bodies[s].actor;
            if (!a) continue;
            const ipc::Vec3& p = poses[s].pos;
            PxVec3 d(p.x - c.x, p.y - c.y, p.z - c.z);
            const float d2 = d.magnitudeSquared();
            if (d2 >= r2) continue;
            const float dist = std::sqrt(d2);
            PxVec3 dir = dist > 1e-3f ? d / dist : PxVec3(0, 1, 0);
            dir = (dir + PxVec3(0, e.up, 0)).getNormalized();
            if (a->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC) continue;
            // A loose chunk gets the game's own push instead (the plugin passes on the impulse the game gave its
            // keyframed Havok body): this one is a velocity change sized for pieces, and its origin is its building's.
            if (chunk[s]) continue;
            a->addForce(dir * (e.impulse * (1.0f - dist / e.radius)),
                        e.mode == ipc::kExplosionVelocityChange ? PxForceMode::eVELOCITY_CHANGE : PxForceMode::eIMPULSE);
            born_excused[s] = 1;
            ++pushed;
        }
        logf("explosion at (%.1f, %.1f, %.1f) r %.1f m: %u bodies pushed", c.x, c.y, c.z, e.radius, pushed);
    }

    // Applies the complete commands in the ring, for at most kDrainBudgetMs per step: a whole static world (the open
    // world sends 2,000+ bodies, 11 MB of commands, height fields and meshes to cook) arrives over several steps,
    // while the host keeps stepping and its heartbeat keeps going (the game ends a host silent for 3 s). Returns
    // false if the ring is corrupt.
    static constexpr double kDrainBudgetMs = 8.0, kDrainBudgetIdleMs = 40.0;  // idle: no bodies to keep moving
    bool drain() {
        HOST_ZONE("host.drain");
        ipc::Control* ctl = sh.control;
        const uint32_t head = ctl->ring_head.load(std::memory_order_acquire);
        const uint32_t size = ctl->ring_size;
        const double t0 = now_ms(), budget = live > 1 ? kDrainBudgetMs : kDrainBudgetIdleMs;  // 1: the player
        uint32_t applied = 0;
        while (sh.tail != head) {
            const uint32_t off = sh.tail & sh.ring_mask;
            const auto* h = reinterpret_cast<const ipc::CmdHeader*>(sh.ring + off);
            if (h->size < sizeof(ipc::CmdHeader) || (h->size & 7) || h->size > size - off || h->size > head - sh.tail) {
                logf("command ring corrupt at %u (type %u, size %u)", sh.tail, h->type, h->size);
                return false;
            }
            const double tc = now_ms();
            apply(*h);
            const double took = now_ms() - tc;
            if (took >= 100.0) logf("slow command: type %u, %u bytes, %.0f ms", unsigned(h->type), h->size, took);
            sh.tail += h->size;
            ++applied;
            ctl->ring_tail.store(sh.tail, std::memory_order_release);
            if (now_ms() - t0 >= budget) break;  // the rest after the next step
        }
        ctl->ring_tail.store(sh.tail, std::memory_order_release);
        if (changes_now() || !pending_chunks.empty()) flush_pending();
        commands_since_snapshot += applied;
        stat_commands += applied;
        return true;
    }

    void apply(const ipc::CmdHeader& h) {
        switch (ipc::Cmd(h.type)) {
            case ipc::Cmd::Pad:
                break;
            case ipc::Cmd::Reset:
                reset();
                break;
            case ipc::Cmd::AddStaticPlane: {
                const auto& c = reinterpret_cast<const ipc::StaticPlane&>(h);
                PxRigidStatic* st = PxCreatePlane(*physics, PxPlane(c.normal.x, c.normal.y, c.normal.z, c.distance), *material);
                scene->addActor(*st);
                statics.push_back(st);
                break;
            }
            case ipc::Cmd::AddStaticBox: {
                const auto& c = reinterpret_cast<const ipc::StaticBox&>(h);
                PxRigidStatic* st = PxCreateStatic(*physics,
                                                   PxTransform(PxVec3(c.pos.x, c.pos.y, c.pos.z),
                                                               PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized()),
                                                   PxBoxGeometry(c.half_extents.x, c.half_extents.y, c.half_extents.z),
                                                   *material);
                scene->addActor(*st);
                statics.push_back(st);
                break;
            }
            case ipc::Cmd::ShapeBox: {
                const auto& c = reinterpret_cast<const ipc::ShapeBox&>(h);
                add_shape(c.shape, physics->createShape(PxBoxGeometry(c.half_extents.x, c.half_extents.y, c.half_extents.z),
                                                        *material, false, PxShapeFlag::eSIMULATION_SHAPE));
                break;
            }
            case ipc::Cmd::ShapeConvex: {
                const auto& c = reinterpret_cast<const ipc::ShapeConvex&>(h);
                if (c.vertex_count >= 4 && sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) <= h.size)
                    add_shape(c.shape, make_convex(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count, 0.0f));
                break;
            }
            case ipc::Cmd::CreateBody: {
                HOST_ZONE("host.drain.create_body");
                create_body(reinterpret_cast<const ipc::CreateBody&>(h));
                break;
            }
            case ipc::Cmd::DestroyBody: {
                HOST_ZONE("host.drain.destroy_body");
                const auto& c = reinterpret_cast<const ipc::DestroyBody&>(h);
                if (c.slot >= max_bodies) break;
                // A pusher going (a loose chunk that broke up, a vehicle out of range, a proxy rebuilt): the frozen
                // pieces resting on it move again, or they would hang in the air.
                PxBounds3 gone = PxBounds3::empty();
                if (!c.quiet && (pusher[c.slot] || chunk[c.slot]) && bodies[c.slot].actor && bodies[c.slot].actor->getScene())
                    gone = bodies[c.slot].actor->getWorldBounds();
                release_body(c.slot);
                if (!gone.isEmpty()) wake_unsupported(gone);
                break;
            }
            case ipc::Cmd::Explosion: {
                HOST_ZONE("host.drain.explosion");
                explosion(reinterpret_cast<const ipc::Explosion&>(h));
                break;
            }
            case ipc::Cmd::Frame: {
                const auto& c = reinterpret_cast<const ipc::Frame&>(h);
                last_frame = c.frame;
                last_frame_qpc = c.qpc;
                const double latency = double(qpc() - c.qpc) * 1000.0 / double(g_freq.QuadPart);
                stat_cmd_latency_ms += latency;
                ++stat_cmd_latency_n;
                break;
            }
            case ipc::Cmd::TestCrash:
                logf("TestCrash: ending the host process abruptly");
                TerminateProcess(GetCurrentProcess(), 3);
                break;
            case ipc::Cmd::StaticMesh:
                static_mesh(reinterpret_cast<const ipc::StaticMesh&>(h), h.size);
                break;
            case ipc::Cmd::StaticConvex:
                static_convex(reinterpret_cast<const ipc::StaticConvex&>(h), h.size);
                break;
            case ipc::Cmd::StaticConvexPlaced:
                static_convex_placed(reinterpret_cast<const ipc::StaticConvexPlaced&>(h), h.size);
                break;
            case ipc::Cmd::RemoveStatics:
                apply_deferred();  // a new group can take its number at once
                remove_statics(reinterpret_cast<const ipc::RemoveStatics&>(h).group);
                break;
            case ipc::Cmd::RaycastBatch:
                raycast_batch(reinterpret_cast<const ipc::RaycastBatch&>(h), h.size);
                break;
            case ipc::Cmd::RemoveStaticKeys: {
                if (defer(h)) break;
                HOST_ZONE("host.drain.remove_static_keys");
                remove_static_keys(reinterpret_cast<const ipc::RemoveStaticKeys&>(h), h.size);
                break;
            }
            case ipc::Cmd::StaticHeightField:
                static_height_field(reinterpret_cast<const ipc::StaticHeightField&>(h), h.size);
                break;
            case ipc::Cmd::ShapePart: {
                const auto& c = reinterpret_cast<const ipc::ShapePart&>(h);
                if (c.vertex_count >= 3 && sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) <= h.size) {
                    PxShape* s = make_convex(reinterpret_cast<const ipc::Vec3*>(&c + 1), c.vertex_count, c.radius);
                    if (s && c.flags) s->setSimulationFilterData(filter_for(c.flags));
                    add_shape(c.shape, s, c.part == 0);
                }
                break;
            }
            case ipc::Cmd::ShapePartPlaced: {
                HOST_ZONE("host.drain.shape_part");
                const auto& c = reinterpret_cast<const ipc::ShapePartPlaced&>(h);
                if (c.vertex_count >= 3 && sizeof(c) + size_t(c.vertex_count) * sizeof(ipc::Vec3) <= h.size) {
                    PxShape* s = make_convex_placed(c);
                    if (s && c.flags) s->setSimulationFilterData(filter_for(c.flags));
                    // Hulls are grown by the convex radius, so a new chunk's parts overlap the parts they just came
                    // off by both radii, along the whole break. Resting that deep, the chunk is not pushed sideways.
                    if (s && (c.flags & ipc::kShapeChunk)) {
                        s->setRestOffset(-2.0f * c.radius);
                        std::vector<ChunkPart>& parts = parts_of_shape[c.shape];
                        if (c.part == 0) parts.clear();
                        if (parts.size() <= c.part) parts.resize(size_t(c.part) + 1);
                        parts[c.part] = {s, c.break_energy, c.break_strength};
                    }
                    add_shape(c.shape, s, c.part == 0);
                }
                break;
            }
            case ipc::Cmd::KinematicTarget: {
                HOST_ZONE("host.drain.kinematic_target");
                const auto& c = reinterpret_cast<const ipc::KinematicTarget&>(h);
                if (c.slot < max_bodies && bodies[c.slot].actor &&
                    (bodies[c.slot].actor->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) {
                    if (!bodies[c.slot].actor->getScene()) flush_pending();
                    if (!bodies[c.slot].actor || !bodies[c.slot].actor->getScene()) break;  // refused by the scene
                    const PxTransform pose(PxVec3(c.pos.x, c.pos.y, c.pos.z),
                                           PxQuat(c.rot.x, c.rot.y, c.rot.z, c.rot.w).getNormalized());
                    // A jump is a teleport: swept to its target, a kinematic body would hit everything in between at
                    // the speed of the jump (pieces flung out of a pile when the player got out of a vehicle).
                    if (c.flags & ipc::kTargetTeleport) bodies[c.slot].actor->setGlobalPose(pose);
                    else bodies[c.slot].actor->setKinematicTarget(pose);
                }
                break;
            }
            case ipc::Cmd::Impulse:
                impulse(reinterpret_cast<const ipc::Impulse&>(h));
                break;
            case ipc::Cmd::MeleeSweep:
                melee_sweep(reinterpret_cast<const ipc::MeleeSweep&>(h));
                break;
            case ipc::Cmd::RemoveParts: {
                if (defer(h)) break;
                HOST_ZONE("host.drain.remove_parts");
                remove_parts(reinterpret_cast<const ipc::RemoveParts&>(h), h.size);
                break;
            }
        }
    }

    // ---- step and snapshot ----

    void step() {
        const int64_t t0 = qpc();
        const bool changed = changes != 0;
        for (uint32_t k = 0; k < kChangeCauses; ++k) stat_change_by[k] += (changes >> k) & 1;
        changes = 0;
        measure_chunks_before();
        {
            HOST_ZONE("host.simulate");
            scene->simulate(dt);
            scene->fetchResults(true);  // contact reports arrive here (on_contact)
        }
        {
            HOST_ZONE("host.break_parts");
            break_parts();
            pay_crush();
            publish_events();
            trace_chunks();
        }
        const int64_t t1 = qpc();
        const double step_ms = double(t1 - t0) * 1000.0 / double(g_freq.QuadPart);
        if (step_ms >= 250.0) logf("slow step: %.0f ms (%u statics, %u bodies)", step_ms, static_count, live);
        stat_steps_changed += changed;
        stat_ms_changed += changed ? step_ms : 0.0;
        g_last_progress_qpc.store(t1);
        ++step_index;
        sim_time += dt;

        HOST_ZONE("host.after_step");
        const bool hurry = awake_slots.size() >= kHurryFreezingAt;
        stat_hurried_steps += hurry;
        const uint32_t freeze_steps = uint32_t((hurry ? 0.25f : 1.0f) * kFreezeSeconds / dt + 0.5f);
        // Bodies that moved in the previous step but not in this one stop interpolating.
        for (uint32_t s : awake_slots) was_awake[s] = 1;
        PxU32 n = 0;
        PxActor** active = scene->getActiveActors(n);
        now_awake.clear();
        for (PxU32 i = 0; i < n; ++i) {
            const uint32_t s = uint32_t(reinterpret_cast<uintptr_t>(active[i]->userData));
            if (s >= max_bodies || !bodies[s].actor) continue;
            const PxTransform t = static_cast<PxRigidDynamic*>(active[i])->getGlobalPose();
            ipc::BodyPose& p = poses[s];
            p.prev_pos = p.pos;
            p.prev_rot = p.rot;
            p.pos = {t.p.x, t.p.y, t.p.z};
            p.rot = {t.q.x, t.q.y, t.q.z, t.q.w};
            p.flags = ipc::kPoseAlive | ipc::kPoseAwake;
            if (!was_awake[s]) awake_since[s] = step_index;
            was_awake[s] = 0;
            now_awake.push_back(s);
            if (user_kinematic[s]) continue;
            const PxRigidDynamic* d = static_cast<PxRigidDynamic*>(active[i]);
            if (!born_excused[s] && step_index - born_step[s] < kBubbleSteps &&
                d->getLinearVelocity().y > kBubbleSpeed) {
                born_excused[s] = 1;
                ++stat_bubbled;
            }
            bool freeze = false;
            const float speed2 = d->getLinearVelocity().magnitudeSquared();
            stat_fastest2 = std::max(stat_fastest2, speed2);
            stat_fastest_up = std::max(stat_fastest_up, d->getLinearVelocity().y);
            if (speed2 < kCalmSpeed * kCalmSpeed && d->getAngularVelocity().magnitudeSquared() < kCalmSpin * kCalmSpin) {
                freeze = ++calm[s] >= freeze_steps;
            } else {
                calm[s] = 0;
            }
            if (step_index - still_step[s] >= freeze_steps) {
                const ipc::BodyPose& from = still_from[s];
                const float dx = p.pos.x - from.pos.x, dy = p.pos.y - from.pos.y, dz = p.pos.z - from.pos.z;
                const float dot = std::fabs(p.rot.x * from.rot.x + p.rot.y * from.rot.y + p.rot.z * from.rot.z +
                                            p.rot.w * from.rot.w);
                if (!freeze && dx * dx + dy * dy + dz * dz < kStillDist * kStillDist && dot > kStillCos) {
                    freeze = true;
                    ++stat_frozen_still;
                }
                still_from[s] = p;
                still_step[s] = step_index;
            }
            if (freeze && !freeze_pending[s]) {
                freeze_pending[s] = 1;
                freeze_list.push_back(s);
            }
        }
        // Made kinematic after the loop: the active-actor buffer belongs to the scene until the next simulate.
        if (changes_now()) {
            HOST_ZONE("host.freeze");
            for (uint32_t s : freeze_list) {
                if (!freeze_pending[s]) continue;  // released meanwhile
                freeze_pending[s] = 0;
                // It went on after it was looked at: what rested on it has not heard of it.
                if (moved_late[s]) {
                    moved_late[s] = 0;
                    const float moved2 = moved_since_woken(s);
                    if (moved2 >= kMovedLateSq) {
                        ++stat_cascade_late;
                        tell_what_rested_on(s);  // while frozen_bounds still has where it lay
                    }
                }
                // Hit in the steps since it was found calm.
                if (bodies[s].actor->getLinearVelocity().magnitudeSquared() > 1.0f) continue;
                changes |= 1u << kChangeFreeze;
                frozen_bounds[s] = bodies[s].actor->getWorldBounds(1.0f);
                bodies[s].actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
                frozen[s] = 1;
                poses[s].flags = ipc::kPoseAlive | ipc::kPoseFrozen;  // its last pose stands
                frozen_slots.push_back(s);
                grid_add(s);
                ++frozen_count;
                ++stat_frozen;
            }
            freeze_list.clear();
        }
        run_cascade();
        run_pushers();
        rescue_fallen();
        while (!gentle.empty() && step_index >= gentle.front().second) {  // the usual speed again
            const auto [g, ends] = gentle.front();
            gentle.pop_front();
            if (bodies[g].actor && born_step[g] + kGentleSteps == ends)
                bodies[g].actor->setMaxDepenetrationVelocity(kMaxDepenetration);
        }
        if (step_index % 600 == 0) grid_rebuild();
        // The player (a user kinematic body that is not a pusher) wakes the light pieces it comes near, ten times a
        // second once it has moved kPlayerThawStep since it last did (pusher_woke_at). Standing still on a pile it
        // kept the pieces around it awake, and they never came to rest.
        if (step_index % 6 == 0 && frozen_count) {
            for (uint32_t s = 0; s < slots_used; ++s)
                if (user_kinematic[s] && !pusher[s] && bodies[s].actor) {
                    const ipc::Vec3& p = poses[s].pos;
                    ipc::Vec3& last = pusher_woke_at[s].pos;
                    const float dx = p.x - last.x, dy = p.y - last.y, dz = p.z - last.z;
                    if (dx * dx + dy * dy + dz * dz < kPlayerThawStep * kPlayerThawStep) continue;
                    last = p;
                    thaw_near(PxVec3(p.x, p.y, p.z), 2.5f, true);
                }
        }
        for (uint32_t s : awake_slots) {
            if (!was_awake[s]) continue;
            was_awake[s] = 0;
            ipc::BodyPose& p = poses[s];
            if (!(p.flags & ipc::kPoseAlive)) continue;
            p.prev_pos = p.pos;
            p.prev_rot = p.rot;
            p.flags = ipc::kPoseAlive | (frozen[s] ? ipc::kPoseFrozen : 0u);
        }
        awake_slots.swap(now_awake);
        if (changes_now()) apply_changes();

        publish(t0, float(double(t1 - t0) * 1000.0 / double(g_freq.QuadPart)), uint32_t(n));
    }

    void publish(int64_t step_qpc, float step_ms, uint32_t awake) {
        HOST_ZONE("host.publish");
        ipc::SnapshotHeader* snap = sh.snapshot(sh.back);
        auto* records = reinterpret_cast<ipc::BodyPose*>(snap + 1);
        uint8_t* states = ipc::snapshot_states(snap, max_bodies);
        // The player's slot is the last one, so slots_used is all of them: 17 MB a step at 262,144 slots, 1.8 ms.
        for (uint32_t b = 0; slots_used && b <= (slots_used - 1) >> kPoseBlockShift; ++b) {
            if (!pose_block_used[b]) continue;
            const uint32_t first = b << kPoseBlockShift, end = std::min(first + (1u << kPoseBlockShift), slots_used);
            memcpy(records + first, poses.data() + first, size_t(end - first) * sizeof(ipc::BodyPose));
            for (uint32_t s = first; s < end; ++s) states[s] = uint8_t(poses[s].flags);
        }
        snap->sequence = ++sequence;
        snap->step = step_index;
        snap->sim_time = sim_time;
        snap->step_qpc = step_qpc;
        snap->step_ms = step_ms;
        snap->bodies = live;
        snap->awake = awake;
        snap->slots = slots_used;
        snap->last_frame = last_frame;
        snap->commands = commands_since_snapshot;
        snap->frozen = frozen_count;
        commands_since_snapshot = 0;
        snap->publish_qpc = qpc();
        snap->work_ms = float(double(snap->publish_qpc - step_qpc) * 1000.0 / double(g_freq.QuadPart));
        const uint32_t prev = sh.control->snapshot_latest.exchange(sh.back | ipc::kFresh, std::memory_order_acq_rel);
        sh.back = prev & ipc::kIndexMask;

        ++stat_steps;
        stat_step_ms += step_ms;
        stat_step_max = std::max(stat_step_max, double(step_ms));
        stat_work_ms += snap->work_ms;
        if (steps_csv)
            fprintf(steps_csv, "%u,%.3f,%.3f,%.3f,%u,%u,%u,%u\n", step_index, now_ms(), step_ms, snap->work_ms, live,
                    awake, snap->commands, last_frame);
    }

    void log_stats() {
        if (stat_cooked || stat_cook_failed) {
            logf("static world: %u statics in %zu groups; cooked %u (%u of them alike others, cooked once; %u failed, "
                 "%u hulls without a volume) in %.1f ms; %zu hulls cached", static_count, groups.size(), stat_cooked,
                 stat_hulls_alike, stat_cook_failed, stat_hull_skipped, stat_cook_ms, hull_cache_count);
            stat_cooked = stat_cook_failed = stat_hull_skipped = stat_hulls_alike = 0;
            stat_cook_ms = 0;
        }
        if (stat_add_refused || g_errors.unlogged) {
            logf("bodies the scene refused (given up) %u; PhysX errors past the log's budget %d", stat_add_refused,
                 g_errors.unlogged.exchange(0));
            stat_add_refused = 0;
        }
        if (!stat_steps) return;
        // What PhysX needed in its last step: contact pairs and, on the GPU, the solver's pinned temp buffer (it grew
        // past 1.9 GB and failed with 20,000 bodies in B5b's first whole-avenue stress cycle) and its counts.
        PxSimulationStatistics ss;
        scene->getSimulationStatistics(ss);
        const PxGpuDynamicsMemoryConfigStatistics& gs = ss.gpuDynamicsMemoryConfigStatistics;
        vram_read();
        logf("PhysX's last step: %u contact pairs (%u touching), %u new, %u lost; %u active constraints, %u partitions; GPU "
             "temp buffer %.1f MB, rigid contacts %u, patches %u, found/lost pairs %u; GPU heap %.0f MB (solver %.0f, "
             "narrowphase %.0f, broadphase %.0f); VRAM %.0f MB in use by all programs, at most %.0f, of %.0f; the fastest piece since the last report %.1f m/s (upward %.1f)",
             ss.nbDiscreteContactPairsTotal, ss.nbDiscreteContactPairsWithContacts, ss.nbNewPairs, ss.nbLostPairs,
             ss.nbActiveConstraints, ss.nbPartitions, double(gs.tempBufferCapacity) / 1048576.0, gs.rigidContactCount,
             gs.rigidPatchCount, gs.foundLostPairs, double(ss.gpuMemHeap) / 1048576.0,
             double(ss.gpuMemHeapSolver) / 1048576.0, double(ss.gpuMemHeapNarrowPhase) / 1048576.0,
             double(ss.gpuMemHeapBroadPhase) / 1048576.0, double(vram_used) / 1048576.0, double(vram_peak) / 1048576.0,
             double(vram_total) / 1048576.0, double(std::sqrt(stat_fastest2)), double(stat_fastest_up));
        stat_fastest2 = 0;
        stat_fastest_up = 0;
        const double now = now_ms();
        const double world_speed = now > stat_since_ms ? (sim_time - stat_since_sim) * 1000.0 / (now - stat_since_ms) : 0.0;
        stat_since_ms = now;
        stat_since_sim = sim_time;
        logf("%u steps: world speed %.0f%%; step %.2f ms mean, %.2f max; work %.2f ms mean; %u bodies, %u awake, %u frozen (+%u (%u going "
             "nowhere), -%u; woken by removed statics %u (%u whole bodies), by the cascade %u (from %u of the %u looked at, %u later), by %zu pushers %u); %u "
             "impulses; %u commands; frame command latency %.2f ms",
             stat_steps, world_speed * 100.0, stat_step_ms / stat_steps, stat_step_max, stat_work_ms / stat_steps, live,
             uint32_t(awake_slots.size()), frozen_count, stat_frozen, stat_frozen_still, stat_thawed, stat_unsupported,
             stat_unsupported_bodies, stat_cascade, stat_cascade_moved, stat_cascade_looked, stat_cascade_late, pusher_slots.size(), stat_pushed_awake, stat_impulses, stat_commands,
             stat_cmd_latency_n ? stat_cmd_latency_ms / stat_cmd_latency_n : 0.0);
        if (stat_wakes_waited || stat_hurried_steps)
            logf("  wakes waited in %u steps (%zu moved pieces and %zu removals still to tell now); pieces hurried to freeze in %u", stat_wakes_waited,
                 resting_on_later.size(), unsupported_later.size(), stat_hurried_steps);
        stat_wakes_waited = stat_hurried_steps = 0;
        look_for_hanging();
        if (stat_hanging_looked) logf("  frozen pieces with nothing under them: %u of %u looked at", stat_hanging, stat_hanging_looked);
        stat_hanging = stat_hanging_looked = 0;
        if (stat_steps_changed)
            logf("  %u steps with bodies changing, %.2f ms mean; %u without, %.2f ms; steps with a thaw %u, a freeze %u, a new body %u, "
                 "one gone %u, a part broke %u, parts gone %u, statics %u", stat_steps_changed,
                 stat_ms_changed / stat_steps_changed, stat_steps - stat_steps_changed,
                 stat_steps > stat_steps_changed ? (stat_step_ms - stat_ms_changed) / (stat_steps - stat_steps_changed) : 0.0,
                 stat_change_by[kChangeThaw], stat_change_by[kChangeFreeze], stat_change_by[kChangeNew], stat_change_by[kChangeGone],
                 stat_change_by[kChangeBroke], stat_change_by[kChangeParts], stat_change_by[kChangeStatics]);
#if RFGPHYS_HOST_ZONES
        g_zones.report(stat_steps);
#endif
        stat_frozen = stat_frozen_still = stat_thawed = stat_unsupported = stat_cascade = stat_pushed_awake = 0;
        stat_cascade_moved = stat_cascade_looked = stat_cascade_late = 0;
        stat_steps_changed = 0;
        stat_ms_changed = 0;
        for (uint32_t& n : stat_change_by) n = 0;
        stat_impulses = stat_unsupported_bodies = 0;
        if (stat_sweeps) logf("  melee sweeps %u: %u bodies pushed", stat_sweeps, stat_swept);
        stat_sweeps = stat_swept = 0;
        if (stat_event_pairs || stat_parts_broken || stat_parts_removed)
            logf("  loose chunks: %llu contact pairs reported; %llu parts broke under their load, taking %.0f kJ, %llu of them over a "
                 "static piece through rubble (%llu not told to the game, the ring full); %u parts the game broke off taken off their bodies",
                 static_cast<unsigned long long>(stat_event_pairs), static_cast<unsigned long long>(stat_parts_broken),
                 stat_break_energy / 1000.0, static_cast<unsigned long long>(stat_through_rubble),
                 static_cast<unsigned long long>(stat_events_dropped), stat_parts_removed);
        stat_event_pairs = stat_parts_broken = stat_events_dropped = stat_through_rubble = 0;
        stat_break_energy = 0;
        stat_parts_removed = 0;
        if (stat_overlap[0] + stat_overlap[1] + stat_overlap[2] + stat_overlap[3] + stat_overlap[4])
            logf("  pieces coming over, 1 in %u measured, by their deepest overlap: none %u, under 1 cm %u, 1-3 cm %u, "
                 "3-10 cm %u, deeper %u", kOverlapEvery, stat_overlap[0], stat_overlap[1], stat_overlap[2],
                 stat_overlap[3], stat_overlap[4]);
        if (stat_overlap_deep[0] + stat_overlap_deep[1] + stat_overlap_deep[2] + stat_overlap_deep[3] +
            stat_overlap_deep[4])
            logf("  of them over 3 cm deep into the static world %u, a frozen piece %u, an awake one %u (one that came "
                 "over in the last 0.5 s %u), a pusher %u",
                 stat_overlap_deep[0], stat_overlap_deep[1], stat_overlap_deep[2] + stat_overlap_deep[4],
                 stat_overlap_deep[4], stat_overlap_deep[3]);
        for (uint32_t& o : stat_overlap) o = 0;
        for (uint32_t& o : stat_overlap_deep) o = 0;
        if (stat_bubbled)
            logf("  pieces pushed upward in their first second here (they came over inside others): %u", stat_bubbled);
        stat_bubbled = 0;
        // Pieces that have not come to rest for kRestlessSteps: a pile that jitters keeps them awake.
        uint32_t restless = 0, restless_light = 0;
        double restless_speed = 0, restless_spin = 0;
        for (uint32_t s : awake_slots) {
            if (user_kinematic[s] || !bodies[s].actor || step_index - awake_since[s] < kRestlessSteps) continue;
            ++restless;
            restless_light += heavy[s] ? 0 : 1;
            restless_speed += bodies[s].actor->getLinearVelocity().magnitude();
            restless_spin += bodies[s].actor->getAngularVelocity().magnitude();
        }
        if (restless)
            logf("  restless pieces (awake over %u steps): %u (%u light), %.2f m/s and %.2f rad/s mean", kRestlessSteps,
                 restless, restless_light, restless_speed / restless, restless_spin / restless);
        if (stat_hull_thickened || stat_hull_boxes || stat_no_shape || stat_inertia_fixed || stat_born_frozen ||
            stat_rescued)
            logf("  hulls thickened to 4 mm %u, boxes for hulls that did not cook %u; bodies without a shape %u, with "
                 "their inertia replaced %u, made frozen (at rest in Havok) %u; pieces put back on the ground %u",
                 stat_hull_thickened, stat_hull_boxes, stat_no_shape, stat_inertia_fixed, stat_born_frozen, stat_rescued);
        stat_hull_thickened = stat_hull_boxes = stat_no_shape = stat_inertia_fixed = stat_born_frozen = 0;
        stat_rescued = 0;
        // The pushers that woke the most pieces in this window.
        std::vector<uint32_t> top;
        for (uint32_t s : pusher_slots)
            if (pusher_woken[s]) top.push_back(s);
        std::sort(top.begin(), top.end(), [this](uint32_t a, uint32_t b) { return pusher_woken[a] > pusher_woken[b]; });
        for (size_t i = 0; i < top.size() && i < 3; ++i)
            logf("  pusher %u (%.1f m across, reaching %.1f m from its origin): %u wake-ups, %u pieces woken", top[i],
                 double(2 * pusher_half[top[i]]), double(body_radius[top[i]]), pusher_wakes[top[i]], pusher_woken[top[i]]);
        for (uint32_t s : pusher_slots) pusher_wakes[s] = pusher_woken[s] = 0;
        stat_steps = stat_commands = stat_cmd_latency_n = 0;
        stat_step_ms = stat_step_max = stat_work_ms = stat_cmd_latency_ms = 0;
        if (steps_csv) fflush(steps_csv);
    }
};

std::wstring arg_value(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    return {};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    const std::wstring map_name = arg_value(argc, argv, L"--map");
    const std::wstring log_path = arg_value(argc, argv, L"--log");
    const std::wstring csv_path = arg_value(argc, argv, L"--steps-csv");
    // Shared reads: the logs can be followed while the host runs.
    if (!log_path.empty()) g_log = _wfsopen(log_path.c_str(), L"w", _SH_DENYNO);
    g_log_dir = log_path.substr(0, log_path.find_last_of(L'\\'));
    logf("rfgphysx_host starting, mapping '%ls'", map_name.c_str());

    Host host;
    if (map_name.empty() || !host.sh.open(map_name.c_str())) {
        logf("cannot open the shared memory (error %lu)", GetLastError());
        return 2;
    }
    ipc::Control* ctl = host.sh.control;
    ctl->host_pid.store(GetCurrentProcessId(), std::memory_order_relaxed);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, ctl->game_pid);
    if (!game) {
        host.fail("cannot open the game process");
        return 2;
    }
    host.max_bodies = ctl->max_bodies;
    host.dt = ctl->step_hz > 0 ? 1.0f / ctl->step_hz : 1.0f / 60.0f;
    host.bodies.resize(host.max_bodies);
    host.poses.assign(host.max_bodies, ipc::BodyPose{});
    host.was_awake.assign(host.max_bodies, 0);
    host.awake_since.assign(host.max_bodies, 0);
    host.calm.assign(host.max_bodies, 0);
    host.still_from.assign(host.max_bodies, ipc::BodyPose{});
    host.still_step.assign(host.max_bodies, 0);
    host.born_step.assign(host.max_bodies, 0);
    host.born_excused.assign(host.max_bodies, 1);
    host.frozen.assign(host.max_bodies, 0);
    host.user_kinematic.assign(host.max_bodies, 0);
    host.heavy.assign(host.max_bodies, 0);
    host.pusher.assign(host.max_bodies, 0);
    host.chunk.assign(host.max_bodies, 0);
    host.pusher_woke_at.assign(host.max_bodies, ipc::BodyPose{});
    host.blow_of.assign(host.max_bodies, 0);
    host.pusher_mid.assign(host.max_bodies, PxVec3(0.0f));
    host.pusher_half.assign(host.max_bodies, 0.0f);
    host.pusher_wakes.assign(host.max_bodies, 0);
    host.pusher_woken.assign(host.max_bodies, 0);
    host.body_radius.assign(host.max_bodies, 0.0f);
    host.cascade_mark.assign(host.max_bodies, 0);
    host.wake_pos.assign(host.max_bodies, ipc::Vec3{0, 0, 0});
    host.wake_due.assign(host.max_bodies, 0);
    host.frozen_bounds.assign(host.max_bodies, PxBounds3::empty());
    host.moved_late.assign(host.max_bodies, 0);
    host.freeze_pending.assign(host.max_bodies, 0);
    host.thaw_pending.assign(host.max_bodies, 0);
    host.change_every = std::max(ctl->change_every, 1u);
    host.rest_strength = std::max(ctl->chunk_rest_strength, 1.0f);
    host.crush_at_contact = ctl->chunk_crush_at_contact != 0;
    host.chunk_wake_ahead = ctl->chunk_wake_ahead != 0;
    host.pose_block_used.assign((host.max_bodies >> Host::kPoseBlockShift) + 1, 0);
    host.sh.back = 0;
    if (!csv_path.empty() && (host.steps_csv = _wfsopen(csv_path.c_str(), L"w", _SH_DENYNO)) != nullptr)
        fprintf(host.steps_csv, "step,t_ms,step_ms,work_ms,bodies,awake,commands,last_frame\n");

    // Normal priority: at above-normal the host's threads took cores from the game's own (the multithreaded Havok
    // step slowed by 1.0-1.4 ms in heavy frames with 20,000 awake test bodies, 06 §13), and a step that starts a
    // little late costs nothing in an asynchronous world.
    if (!host.init_physx(ctl->backend)) return 2;
    ctl->host_gpu.store(host.gpu ? 1u : 0u, std::memory_order_relaxed);
    host.sh.message(host.gpu ? "GPU PhysX ready" : "CPU PhysX ready");
    ctl->host_state.store(uint32_t(ipc::HostState::Ready), std::memory_order_release);
    logf("ready");

    // The heartbeat says the process lives; it comes from a thread of its own, since cooking one large static (the
    // open world's meshes and height fields) can keep the main loop busy for seconds. The game judges progress by
    // the steps (snapshots) separately.
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    SetUnhandledExceptionFilter(&log_crash);
    g_last_progress_qpc.store(qpc());
    std::thread([ctl] {
        bool told = false;
        for (;;) {
            ctl->host_heartbeat.fetch_add(1, std::memory_order_relaxed);
            Sleep(100);
            const double stuck = double(qpc() - g_last_progress_qpc.load()) * 1000.0 / double(g_freq.QuadPart);
            if (stuck > 15000.0 && !told) {
                told = true;
                log_main_stack(stuck);
            } else if (stuck < 1000.0) {
                told = false;
            }
        }
    }).detach();

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double period_ms = 1000.0 / (ctl->step_hz > 0 ? ctl->step_hz : 60.0);
    double next_step = now_ms();
    double next_stats = now_ms() + 5000.0;
    const char* reason = "quit requested";
    for (;;) {
        ctl->host_heartbeat.fetch_add(1, std::memory_order_relaxed);
        if (ctl->quit.load(std::memory_order_acquire)) break;
        if (WaitForSingleObject(game, 0) == WAIT_OBJECT_0) {
            reason = "the game process is gone";
            break;
        }
        const double t = now_ms();
        if (t >= next_step) {
            if (!host.drain()) {
                host.fail("command ring corrupt");
                return 2;
            }
            host.step();
            if (host.gpu_failed()) {
                host.log_stats();
                host.fail("the GPU failed: PhysX's scene cannot go on");
                return 3;
            }
#if RFGPHYS_HOST_ZONES
            g_zones.end_step(now_ms() - t);
#endif
            next_step += period_ms;
            if (now_ms() - next_step > 3 * period_ms) next_step = now_ms();  // fell behind: no catch-up burst
        } else {
            // Sleep until about half a millisecond before the step, then poll; commands are applied at step time.
            const double wait = next_step - t;
            if (wait > 1.0 && timer) {
                LARGE_INTEGER due;
                due.QuadPart = -LONGLONG((wait - 0.5) * 10000.0);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, 20);
            } else {
                Sleep(0);
            }
        }
        if (now_ms() >= next_stats) {
            host.log_stats();
            next_stats += 5000.0;
        }
    }
    host.log_stats();
    logf("exiting: %s", reason);
    ctl->host_state.store(uint32_t(ipc::HostState::Exited), std::memory_order_release);
    if (host.steps_csv) fclose(host.steps_csv);
    if (g_log) fclose(g_log);
    // The process exits without tearing PhysX down: the OS frees everything at once, and a CUDA context release can
    // take long enough to matter when the game is closing.
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
