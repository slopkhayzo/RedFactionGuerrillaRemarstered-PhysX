// rfgphysx_host: the 64-bit PhysX side of rfgphys (research/docs/06 §3, §13).
//
// rfgphys.asi (inside the 32-bit game) creates a shared-memory mapping and starts this process with its name. The
// host steps one PhysX scene at a fixed rate, applies the game's commands before each step and publishes a snapshot
// of every body after it (src/shared/rfgphysx_ipc.h). It never waits for the game, and it exits when the game asks,
// when the game process is gone, or when the game's job object closes.
//
//   rfgphysx_host.exe --map <mapping name> [--log <file>] [--steps-csv <file>]
#include <PxPhysicsAPI.h>
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
    if ((d0.word0 & d1.word1) || (d1.word0 & d0.word1)) return PxFilterFlag::eSUPPRESS;
    flags = PxPairFlag::eCONTACT_DEFAULT;
    return PxFilterFlag::eDEFAULT;
}

PxFilterData filter_for(uint32_t shape_flags) {
    if (shape_flags & ipc::kShapePlayer) return PxFilterData(ipc::kShapePlayer, ipc::kShapeHeavy, 0, 0);
    return PxFilterData(shape_flags & ipc::kShapeHeavy, 0, 0, 0);
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

void log_main_stack(double stuck_ms) {
    logf("main loop stuck: no step finished for %.0f s; its stack:", stuck_ms / 1000.0);
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    DWORD64 frames[48];
    int n = 0;
    if (SuspendThread(g_main_thread) == DWORD(-1)) return;
    if (GetThreadContext(g_main_thread, &ctx)) {
        STACKFRAME64 f{};
        f.AddrPC.Offset = ctx.Rip, f.AddrPC.Mode = AddrModeFlat;
        f.AddrFrame.Offset = ctx.Rbp, f.AddrFrame.Mode = AddrModeFlat;
        f.AddrStack.Offset = ctx.Rsp, f.AddrStack.Mode = AddrModeFlat;
        while (n < 48 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), g_main_thread, &f, &ctx, nullptr,
                                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) &&
               f.AddrPC.Offset)
            frames[n++] = f.AddrPC.Offset;
    }
    ResumeThread(g_main_thread);
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

struct ErrorCallback : PxErrorCallback {
    int reported = 0;
    void reportError(PxErrorCode::Enum code, const char* message, const char* file, int line) override {
        if (reported++ < 50) logf("PhysX error %d: %s (%s:%d)", int(code), message, file, line);
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
    // Freezing (06 §12): a pile is one island and never falls asleep, so a piece that stays calm for kFreezeSteps
    // becomes kinematic until an explosion or the player comes near it. That cuts the pile's island apart, so the
    // rest can sleep, and the game stops moving its render instance.
    static constexpr uint16_t kFreezeSteps = 60;
    static constexpr float kCalmSpeed = 0.15f, kCalmSpin = 0.5f;
    // Or a piece that went nowhere for kFreezeSteps (less than kStillDist, turned less than 12 degrees): one rocking in
    // place in a pile never slows down enough to be calm (a thousand of them rocked in a Wrecking Crew match, the
    // piles bubbling), yet stays where it is.
    static constexpr float kStillDist = 0.06f, kStillCos = 0.99452f;  // cos(12 deg / 2): quaternion dot product
    // Overlaps (pieces coming over from Havok's piles, the player's capsule walked into a pile) are resolved at this
    // speed at most: faster, pieces popped up out of piles and fell back.
    static constexpr float kMaxDepenetration = 1.5f;  // m/s, PxRigidBody::setMaxDepenetrationVelocity of debris
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
    // Pushers: kinematic bodies standing for heavy game bodies (vehicles and walkers, ipc::kBodyPusher). A frozen
    // piece is kinematic too, and two kinematic bodies pass through each other, so every step a pusher wakes the
    // frozen pieces it is about to reach, whatever their mass.
    // A pusher wakes them once a point of it has gone kPushStep since it last did: a loose chunk of a building lying
    // in a pile jitters by millimetres, and woke the whole pile every step for nothing (100,000 thaws in a Wrecking
    // Crew match, the awake pieces doubled). How far its points went is told by its shape (pusher_moved), not its
    // origin: a chunk has its building's origin, often 10-20 m away, and a chunk of a metre rolling over at 0.3 m/s
    // swung that origin half a metre a step; it woke everything within metres of it every step (its speed widens
    // the reach), and the piles around it bubbled as the woken pieces settled again.
    static constexpr float kPushReach = 1.0f;       // m beyond the pusher's bounds
    static constexpr float kPushLookahead = 4.0f;   // steps of its current speed, also beyond its bounds
    static constexpr float kPushStep = 0.1f;        // m
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
    // the frozen pieces in and above their bounds, and a woken piece that then moves (2 cm within 4 steps: it lost its
    // support or was pushed) wakes the frozen pieces touching it from above, which do the same, ring by ring. A woken
    // piece that stays put passes nothing on (otherwise the player's capsule, which wakes what it comes near ten
    // times a second, kept whole piles churning). Frozen pieces are found in a grid of 2 m cells; entries go stale
    // when they thaw, and the grid is rebuilt now and then.
    static constexpr float kCell = 2.0f, kTouch = 0.3f, kNeighbourReach = 3.0f, kMovedSq = 0.02f * 0.02f;
    static constexpr uint32_t kCascadePerStep = 400, kCascadeDelaySteps = 4;
    std::vector<ipc::Vec3> wake_pos;    // per slot: where it was when woken
    std::vector<uint32_t> wake_due;     // per slot: the step its movement is checked
    std::vector<float> body_radius;  // per slot: bounds of its shapes around its origin
    std::unordered_map<uint64_t, std::vector<uint32_t>> frozen_grid;
    std::vector<uint32_t> cascade_now, cascade_next;
    std::vector<uint8_t> cascade_mark;
    uint32_t stat_cascade = 0, stat_unsupported = 0;
    std::unordered_map<uint32_t, std::vector<PxShape*>> shapes;  // body shapes by id: one or more convex parts
    std::vector<PxRigidStatic*> statics;                                  // test planes and boxes
    // The mirrored world, one group per Havok body: its convex pieces as shapes of one static actor (found by key,
    // so a building can lose single pieces), and its meshes and height fields as actors of their own.
    struct Group {
        PxRigidStatic* pieces = nullptr;
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

    uint32_t step_index = 0, sequence = 0, last_frame = 0, commands_since_snapshot = 0;
    double sim_time = 0;
    int64_t last_frame_qpc = 0;

    // Statistics for the periodic log line.
    uint32_t stat_steps = 0, stat_commands = 0;
    double stat_step_ms = 0, stat_step_max = 0, stat_work_ms = 0, stat_cmd_latency_ms = 0;
    uint32_t stat_cmd_latency_n = 0;
    FILE* steps_csv = nullptr;

    bool init_physx(uint32_t backend) {
        foundation = PxCreateFoundation(PX_PHYSICS_VERSION, g_allocator, g_errors);
        if (!foundation) return fail("PxCreateFoundation failed");
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
        // GPU dynamics leave PhysX little CPU work to spread; the CPU version takes what the game leaves over.
        dispatcher = PxDefaultCpuDispatcherCreate(gpu ? 2 : 6);
        sd.cpuDispatcher = dispatcher;
        sd.filterShader = filter_shader;
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
        } else {
            sd.broadPhaseType = PxBroadPhaseType::ePABP;
        }
        scene = physics->createScene(sd);
        if (!scene) return fail("createScene failed");
        logf("PhysX %d.%d.%d scene ready: %s dynamics, TGS solver, %.0f Hz steps, %u body slots", PX_PHYSICS_VERSION_MAJOR,
             PX_PHYSICS_VERSION_MINOR, PX_PHYSICS_VERSION_BUGFIX, gpu ? "GPU" : "CPU", 1.0 / dt, max_bodies);
        return true;
    }

    bool fail(const char* what) {
        logf("failed: %s", what);
        sh.message(what);
        sh.control->host_state.store(uint32_t(ipc::HostState::Failed), std::memory_order_release);
        return false;
    }

    // ---- commands ----

    void reset() {
        flush_pending();
        for (uint32_t s = 0; s < slots_used; ++s) release_body(s);
        for (PxRigidStatic* st : statics) st->release();
        statics.clear();
        for (auto& [g, group] : groups) release_group(group);
        groups.clear();
        static_count = 0;
        sh.control->statics.store(0, std::memory_order_relaxed);
        for (auto& [id, parts] : shapes)
            for (PxShape* shape : parts) shape->release();
        shapes.clear();
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
        if (b.actor->getScene()) {
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
        const bool gentle_start = (c.flags & ipc::kBodyGentle) != 0;
        a->setMaxDepenetrationVelocity(gentle_start ? kGentleDepenetration : kMaxDepenetration);
        if (gentle_start) gentle.emplace_back(c.slot, step_index + kGentleSteps);
        a->setLinearVelocity(PxVec3(c.lin_vel.x, c.lin_vel.y, c.lin_vel.z));
        a->setAngularVelocity(PxVec3(c.ang_vel.x, c.ang_vel.y, c.ang_vel.z));
        a->userData = reinterpret_cast<void*>(uintptr_t(c.slot));
        if (c.flags & ipc::kBodyKinematic) a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
        user_kinematic[c.slot] = (c.flags & ipc::kBodyKinematic) ? 1 : 0;
        pusher[c.slot] = (c.flags & ipc::kBodyKinematic) && (c.flags & ipc::kBodyPusher) ? 1 : 0;
        if (pusher[c.slot] && std::find(pusher_slots.begin(), pusher_slots.end(), c.slot) == pusher_slots.end()) {
            pusher_slots.push_back(c.slot);
            pusher_wakes[c.slot] = pusher_woken[c.slot] = 0;  // a pusher before it went out of the list unlogged
        }
        // Made where it stands: it wakes nothing until it moves (frozen pieces it overlaps are kinematic too, so they
        // do not touch it until then).
        pusher_woke_at[c.slot].pos = c.pos;
        pusher_woke_at[c.slot].rot = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
        heavy[c.slot] = (it->second[0]->getSimulationFilterData().word0 & ipc::kShapeHeavy) ? 1 : 0;
        float radius = 0;
        for (PxShape* shape : it->second) {
            const PxBounds3 b = PxShapeExt::getWorldBounds(*shape, *a, 1.0f);
            radius = std::max(radius, (b.getCenter() - pose.p).magnitude() + b.getExtents().magnitude());
        }
        body_radius[c.slot] = radius;
        if (pusher[c.slot]) {
            PxBounds3 local = PxBounds3::empty();
            for (PxShape* shape : it->second) {
                PxBounds3 b;
                PxGeometryQuery::computeGeomBounds(b, shape->getGeometry(), shape->getLocalPose(), 0.0f, 1.0f);
                local.include(b);
            }
            pusher_mid[c.slot] = local.getCenter();
            pusher_half[c.slot] = local.getExtents().magnitude();
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
        ipc::BodyPose& p = poses[c.slot];
        p.pos = c.pos;
        p.prev_pos = c.pos;
        p.rot = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
        p.prev_rot = p.rot;
        p.flags = ipc::kPoseAlive;
        ++live;
        slots_used = std::max(slots_used, c.slot + 1);
        // A piece that lay still in Havok starts frozen. A whole pile that had settled there came over awake and in
        // contact at once, and the solver could not quite settle it again: hundreds of pieces rocked for good.
        if ((c.flags & ipc::kBodyAtRest) && !user_kinematic[c.slot]) {
            a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            frozen[c.slot] = 1;
            p.flags = ipc::kPoseAlive | ipc::kPoseFrozen;
            frozen_slots.push_back(c.slot);
            grid_add(c.slot);
            ++frozen_count;
            ++stat_born_frozen;
        }
        if (!user_kinematic[c.slot] && ++overlap_count % kOverlapEvery == 0) measure_overlap(a, c.slot);
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

    void flush_pending() {
        if (pending_add.empty()) return;
        scene->addActors(pending_add.data(), PxU32(pending_add.size()));
        pending_add.clear();
    }

    // ---- the static world mirror ----

    void count_statics(int delta) {
        static_count = uint32_t(int(static_count) + delta);
        sh.control->statics.store(static_count, std::memory_order_relaxed);
    }

    void release_group(Group& g) {
        if (g.pieces) g.pieces->release();
        for (PxRigidStatic* st : g.actors) st->release();
        count_statics(-int(g.shape_count));
        g = Group{};
    }

    void add_static_actor(uint32_t group, PxRigidStatic* st) {
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
        Group& g = groups[c.group];
        if (!g.pieces) {
            g.pieces = physics->createRigidStatic(PxTransform(PxIdentity));
            scene->addActor(*g.pieces);
        }
        PxShape* shape = PxRigidActorExt::createExclusiveShape(*g.pieces, PxConvexMeshGeometry(mesh), *material);
        mesh->release();
        if (!shape) return;
        shape->setLocalPose(PxTransform(centre));
        g.by_key[c.key].push_back(shape);
        ++g.shape_count;
        count_statics(1);
    }

    void remove_static_keys(const ipc::RemoveStaticKeys& c, uint32_t size) {
        if (sizeof(c) + size_t(c.count) * 4 > size) return;
        auto it = groups.find(c.group);
        if (it == groups.end() || !it->second.pieces) return;
        Group& g = it->second;
        const auto* keys = reinterpret_cast<const uint32_t*>(&c + 1);
        uint32_t removed = 0;
        PxBounds3 gone = PxBounds3::empty();
        for (uint32_t i = 0; i < c.count; ++i) {
            auto k = g.by_key.find(keys[i]);
            if (k == g.by_key.end()) continue;
            for (PxShape* shape : k->second) {
                gone.include(PxShapeExt::getWorldBounds(*shape, *g.pieces, 1.0f));
                g.pieces->detachShape(*shape);
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
        if (g.pieces) bounds.include(g.pieces->getWorldBounds());
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
        PxRigidDynamic* a = bodies[s].actor;
        a->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, false);
        a->wakeUp();
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
        wake_pos[s] = poses[s].pos;
        wake_due[s] = step_index + kCascadeDelaySteps;
        if (!cascade_mark[s]) {
            cascade_mark[s] = 1;
            cascade_next.push_back(s);
        }
    }

    // Once per step: woken pieces whose check is due and that moved wake the frozen pieces touching them from above.
    void run_cascade() {
        if (cascade_next.empty()) return;
        cascade_now.clear();
        size_t keep = 0;
        for (size_t i = 0; i < cascade_next.size(); ++i) {
            const uint32_t t = cascade_next[i];
            if (step_index < wake_due[t] || cascade_now.size() >= kCascadePerStep) cascade_next[keep++] = t;  // later
            else cascade_now.push_back(t);
        }
        cascade_next.resize(keep);
        for (uint32_t t : cascade_now) {
            cascade_mark[t] = 0;
            if (!bodies[t].actor || frozen[t]) continue;
            const ipc::Vec3 c = poses[t].pos;
            const float mx = c.x - wake_pos[t].x, my = c.y - wake_pos[t].y, mz = c.z - wake_pos[t].z;
            if (mx * mx + my * my + mz * mz < kMovedSq) continue;  // still where it was: it holds what rests on it
            const float reach = body_radius[t] + kNeighbourReach + kTouch;
            const int x0 = cell_of(c.x - reach), x1 = cell_of(c.x + reach), y0 = cell_of(c.y - 0.1f),
                      y1 = cell_of(c.y + reach), z0 = cell_of(c.z - reach), z1 = cell_of(c.z + reach);
            for (int x = x0; x <= x1; ++x)
                for (int y = y0; y <= y1; ++y)
                    for (int z = z0; z <= z1; ++z) {
                        auto it = frozen_grid.find(cell_key(x, y, z));
                        if (it == frozen_grid.end()) continue;
                        for (uint32_t f : it->second) {
                            if (!frozen[f] || f == t) continue;
                            const ipc::Vec3& p = poses[f].pos;
                            if (p.y < c.y - 0.1f) continue;  // below: it does not rest on t
                            const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z, r = body_radius[t] + body_radius[f] + kTouch;
                            if (dx * dx + dy * dy + dz * dz < r * r) {
                                wake(f);
                                ++stat_cascade;
                            }
                        }
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
                        const ipc::Vec3& p = poses[f].pos;
                        const float r = body_radius[f];
                        if (p.x > b.minimum.x - r && p.x < b.maximum.x + r && p.y > b.minimum.y - r &&
                            p.y < b.maximum.y + r && p.z > b.minimum.z - r && p.z < b.maximum.z + r) {
                            wake(f);
                            ++n;
                        }
                    }
                }
        return n;
    }

    // Frozen pieces near a pusher's parts (each part's bounds, `reach` around them) move again. Its parts, not a sphere
    // around its origin: a loose chunk of a building reaches 30 m and more from its origin, and woke everything that
    // far whenever it moved.
    uint32_t wake_near_parts(PxRigidDynamic* a, float reach) {
        PxShape* parts[256];
        const PxU32 n = a->getShapes(parts, 256);
        uint32_t woken = 0;
        for (PxU32 i = 0; i < n; ++i) {
            PxBounds3 b = PxShapeExt::getWorldBounds(*parts[i], *a, 1.0f);
            b.fattenFast(reach);
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
        size_t keep = 0;
        for (size_t i = 0; i < pusher_slots.size(); ++i) {
            const uint32_t s = pusher_slots[i];
            if (!pusher[s] || !bodies[s].actor) continue;  // gone
            pusher_slots[keep++] = s;
            const ipc::BodyPose& p = poses[s];
            ipc::BodyPose& at = pusher_woke_at[s];
            // Still about where it last woke what it reaches.
            if (pusher_moved(s, at.pos, at.rot, p.pos, p.rot) < kPushStep) continue;
            at = p;
            const float speed_per_step = pusher_moved(s, p.prev_pos, p.prev_rot, p.pos, p.rot);
            const uint32_t woken =
                wake_near_parts(bodies[s].actor, kPushReach + kPushLookahead * std::min(speed_per_step, 2.0f));
            stat_pushed_awake += woken;
            ++pusher_wakes[s];
            pusher_woken[s] += woken;
        }
        pusher_slots.resize(keep);
    }

    // A push on one piece at a point (a hammer blow, a bullet): it moves again if it was frozen.
    void impulse(const ipc::Impulse& c) {
        if (c.slot >= max_bodies || !bodies[c.slot].actor || user_kinematic[c.slot]) return;
        flush_pending();
        if (frozen[c.slot]) wake(c.slot);
        born_excused[c.slot] = 1;
        PxRigidDynamic* a = bodies[c.slot].actor;
        const PxVec3 j(c.impulse.x, c.impulse.y, c.impulse.z), at(c.point.x, c.point.y, c.point.z);
        if (!j.isFinite() || !at.isFinite()) return;
        PxRigidBodyExt::addForceAtPos(*a, c.mode == ipc::kExplosionVelocityChange ? j * a->getMass() : j, at,
                                      PxForceMode::eIMPULSE);
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
                if (b.contains(PxVec3(p.x, p.y, p.z))) {
                    wake(s);
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
                        if (b.contains(PxVec3(p.x, p.y, p.z))) {
                            wake(s);
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
        const PxQueryFilterData statics_only(PxQueryFlag::eSTATIC);
        for (uint32_t s : awake_slots) {
            if (user_kinematic[s] || frozen[s] || !bodies[s].actor) continue;
            PxRigidDynamic* a = bodies[s].actor;
            if (a->getLinearVelocity().y > -kFallingSpeed) continue;
            const PxTransform t = a->getGlobalPose();
            PxRaycastBuffer hit;
            if (!scene->raycast(t.p, PxVec3(0.0f, 1.0f, 0.0f), 500.0f, hit,
                                PxHitFlag::eDEFAULT | PxHitFlag::eMESH_BOTH_SIDES, statics_only) ||
                !hit.hasBlock || !hit.block.shape ||
                hit.block.shape->getGeometry().getType() != PxGeometryType::eHEIGHTFIELD)
                continue;
            a->setGlobalPose(PxTransform(PxVec3(t.p.x, hit.block.position.y + body_radius[s] + 0.05f, t.p.z), t.q));
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
            if (d.magnitudeSquared() < r2 && !(light_only && heavy[s])) wake(s);
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
        flush_pending();
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
            case ipc::Cmd::CreateBody:
                create_body(reinterpret_cast<const ipc::CreateBody&>(h));
                break;
            case ipc::Cmd::DestroyBody: {
                const auto& c = reinterpret_cast<const ipc::DestroyBody&>(h);
                if (c.slot >= max_bodies) break;
                // A pusher going (a loose chunk that broke up, a vehicle out of range, a proxy rebuilt): the frozen
                // pieces resting on it move again, or they would hang in the air.
                PxBounds3 gone = PxBounds3::empty();
                if (pusher[c.slot] && bodies[c.slot].actor && bodies[c.slot].actor->getScene())
                    gone = bodies[c.slot].actor->getWorldBounds();
                release_body(c.slot);
                if (!gone.isEmpty()) wake_unsupported(gone);
                break;
            }
            case ipc::Cmd::Explosion:
                explosion(reinterpret_cast<const ipc::Explosion&>(h));
                break;
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
            case ipc::Cmd::RemoveStatics:
                remove_statics(reinterpret_cast<const ipc::RemoveStatics&>(h).group);
                break;
            case ipc::Cmd::RaycastBatch:
                raycast_batch(reinterpret_cast<const ipc::RaycastBatch&>(h), h.size);
                break;
            case ipc::Cmd::RemoveStaticKeys:
                remove_static_keys(reinterpret_cast<const ipc::RemoveStaticKeys&>(h), h.size);
                break;
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
            case ipc::Cmd::KinematicTarget: {
                const auto& c = reinterpret_cast<const ipc::KinematicTarget&>(h);
                if (c.slot < max_bodies && bodies[c.slot].actor &&
                    (bodies[c.slot].actor->getRigidBodyFlags() & PxRigidBodyFlag::eKINEMATIC)) {
                    flush_pending();
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
        }
    }

    // ---- step and snapshot ----

    void step() {
        const int64_t t0 = qpc();
        scene->simulate(dt);
        scene->fetchResults(true);
        const int64_t t1 = qpc();
        const double step_ms = double(t1 - t0) * 1000.0 / double(g_freq.QuadPart);
        if (step_ms >= 250.0) logf("slow step: %.0f ms (%u statics, %u bodies)", step_ms, static_count, live);
        g_last_progress_qpc.store(t1);
        ++step_index;
        sim_time += dt;

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
            if (d->getLinearVelocity().magnitudeSquared() < kCalmSpeed * kCalmSpeed &&
                d->getAngularVelocity().magnitudeSquared() < kCalmSpin * kCalmSpin) {
                freeze = ++calm[s] >= kFreezeSteps;
            } else {
                calm[s] = 0;
            }
            if (step_index - still_step[s] >= kFreezeSteps) {
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
            if (freeze) freeze_list.push_back(s);
        }
        // Made kinematic after the loop: the active-actor buffer belongs to the scene until the next simulate.
        for (uint32_t s : freeze_list) {
            bodies[s].actor->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
            frozen[s] = 1;
            poses[s].flags = ipc::kPoseAlive | ipc::kPoseFrozen;  // its last pose stands
            frozen_slots.push_back(s);
            grid_add(s);
            ++frozen_count;
            ++stat_frozen;
        }
        freeze_list.clear();
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

        publish(t0, float(double(t1 - t0) * 1000.0 / double(g_freq.QuadPart)), uint32_t(n));
    }

    void publish(int64_t step_qpc, float step_ms, uint32_t awake) {
        ipc::SnapshotHeader* snap = sh.snapshot(sh.back);
        auto* records = reinterpret_cast<ipc::BodyPose*>(snap + 1);
        memcpy(records, poses.data(), size_t(slots_used) * sizeof(ipc::BodyPose));
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
            logf("static world: %u statics in %zu groups; cooked %u (%u failed, %u hulls without a volume) in %.1f ms",
                 static_count, groups.size(), stat_cooked, stat_cook_failed, stat_hull_skipped, stat_cook_ms);
            stat_cooked = stat_cook_failed = stat_hull_skipped = 0;
            stat_cook_ms = 0;
        }
        if (!stat_steps) return;
        logf("%u steps: step %.2f ms mean, %.2f max; work %.2f ms mean; %u bodies, %u awake, %u frozen (+%u (%u going "
             "nowhere), -%u; woken by removed statics %u (%u whole bodies), by the cascade %u, by %zu pushers %u); %u "
             "impulses; %u commands; frame command latency %.2f ms",
             stat_steps, stat_step_ms / stat_steps, stat_step_max, stat_work_ms / stat_steps, live,
             uint32_t(awake_slots.size()), frozen_count, stat_frozen, stat_frozen_still, stat_thawed, stat_unsupported,
             stat_unsupported_bodies, stat_cascade, pusher_slots.size(), stat_pushed_awake, stat_impulses, stat_commands,
             stat_cmd_latency_n ? stat_cmd_latency_ms / stat_cmd_latency_n : 0.0);
        stat_frozen = stat_frozen_still = stat_thawed = stat_unsupported = stat_cascade = stat_pushed_awake = 0;
        stat_impulses = stat_unsupported_bodies = 0;
        if (stat_sweeps) logf("  melee sweeps %u: %u bodies pushed", stat_sweeps, stat_swept);
        stat_sweeps = stat_swept = 0;
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
            next_step += period_ms;
            if (now_ms() - next_step > 3 * period_ms) next_step = now_ms() + period_ms;  // fell behind: no catch-up burst
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
