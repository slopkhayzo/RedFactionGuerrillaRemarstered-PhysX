#include "test/stress_pieces.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "physx/bridge.h"
#include "physx/debris_offload.h"
#include "physx/world_mirror.h"
#include "test/memory_census.h"
#include "test/viewpoints.h"

namespace stress_pieces {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// object: pos, Havok handle, flags, handle. rfg_mover: rdd.model_instance_p and rdd.m_loaded; that
// rl_destroyable_model_instance: m_num_sub_instances (what still stands) and m_shared_base, whose
// m_num_base_sub_instances are all of the building's (its pieces once it is levelled). world: all_objects.elt,
// subtype_objects[OT_SUB_MOVER_RFG] (base_array<ushort>: elt, num), and the general movers' count (the debris still
// in Havok). hkpRigidBody: motion type.
constexpr uint32_t kObjPos = 0x04, kObjHavokHandle = 0x4C, kObjFlags = 0x54, kObjHandle = 0x6C;
constexpr uint8_t kObjDestroyed = 0x10;
constexpr uint32_t kMoverInstance = 0x150, kMoverLoaded = 0x154;
constexpr uint32_t kInstNumSubs = 0x120, kInstShared = 0x148, kBaseNumSubs = 0x24;
constexpr uint32_t kWorldAllObjects = 0x64, kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr uint32_t kWorldGeneralMoverCount = 0x3CC;
constexpr uint32_t kBodyMotionType = 0xE8;
constexpr uint8_t kMotionFixed = 5;
constexpr uint32_t kExplosionInfoSize = 0xF0;
// hkLargeBlockAllocator, Havok's heap (s_largeBlockAllocator): m_fixedBlockServer (its m_start and m_end: the whole
// heap, when m_server is that one) and m_sumAllocatedSize, the bytes in use (the pages it took so far, +0xBC, only grow
// as needed).
constexpr uint32_t kHeapFixedServer = 0x9C, kServerStart = 0x04, kServerEnd = 0x08, kHeapServer = 0xB0,
                   kHeapInUse = 0xD8;

// Buildings: standing rfg_movers with a fixed body and at least kMinSubs sub-instances, kMinVolume or more.
constexpr float kMinVolume = 300.0f;  // m3
constexpr uint32_t kMinSubs = 16;
constexpr float kClearance = 4.0f;    // m between the player and a building's box
constexpr size_t kMaxBuildings = 96;
// A round: vehicle bombs throughout the building's box (kBlastStep apart, wider for big ones, at most
// kMaxBlastsPerRound), top first, kBlastsPerFrame a frame. A building with more than kStandingShare of its
// sub-instances left gets another round, up to kMaxRounds; one that loses less than kNoShare in a cycle does not come
// apart and is left out. The next building waits while Havok holds more than kHavokDebrisGate pieces (they go to
// PhysX 2 s after their shatter).
constexpr int kBlastsPerFrame = 4, kMaxBlastsPerRound = 160, kMaxRounds = 3;
constexpr float kBlastStepXZ = 7.0f, kBlastStepY = 6.0f;
constexpr double kStandingShare = 0.2, kNoShare = 0.05;
constexpr int kHavokDebrisGate = 2500;
// Times (s): after a round; the settling (the debris in PhysX, the piles caught up); the overview held, measured from
// kMeasureFromS, its screenshot at kShotAtS; the restored buildings' wait; the hold at the end, measured from
// kHoldMeasureFromS; the room the hold and the viewpoints need before ExitAfterGameplaySeconds.
constexpr double kRoundGapS = 3.0, kSettleMinS = 8.0, kSettleMaxS = 45.0;
constexpr double kViewS = 4.0, kMeasureFromS = 1.0, kShotAtS = 3.5;
constexpr double kRestoreMinS = 2.0, kRestoreWaitS = 10.0;
constexpr double kWorldWaitS = 120.0;  // the most the first cycle waits for the host's static world
constexpr double kHoldS = 10.0, kHoldMeasureFromS = 4.0;
constexpr double kEndRoomS = 75.0;
// Stops: before the slots could not take one more cycle (with kSlotMargin to spare), Havok's heap past kHeapStop,
// fewer render instances than kInstancesStop, little address space; after two cycles of fewer than kSmallCycle pieces.
constexpr uint32_t kSlotMargin = 4096;
constexpr double kHeapStop = 0.88;
constexpr int32_t kInstancesStop = 6000;
constexpr double kVaFreeStopMb = 120, kVaLargestStopMb = 24;
constexpr uint32_t kSmallCycle = 500;
constexpr int kMaxCycles = 200;

struct Building {
    uint32_t handle;
    float lo[3], hi[3];
    uint32_t subs;  // its sub-instances
    bool out;       // left out: does not come apart, or was not restored
};

struct Frames {
    double mean, p50, p95, p99;
    size_t n;
};

struct Point {
    float p[3];
};

enum Phase { kWait, kBlast, kSettle, kView, kRestore, kHold, kViews, kDone };
Phase g_phase = kWait;
uint32_t g_target = 0;
float g_radius = 120.0f;
double g_start_s = 15.0, g_stop_at_s = 0;  // g_stop_at_s: no cycle starts after it (0: no limit)
bool g_viewpoints = false, g_end_game = false;
std::vector<Building> g_buildings;
float g_centre[3] = {}, g_extent = 0;  // the buildings' area: its middle at its lowest ground, its horizontal reach
const uint8_t* g_info = nullptr;       // the vehicle bomb
std::vector<Point> g_queue;
size_t g_queue_at = 0;
int g_cycle = 0, g_round = 0, g_small_cycles = 0;
uint32_t g_restore_scans = 0;  // world_mirror::flushed_scans() when the buildings were restored
int g_movers_base = 0;  // general movers at the cycle's start: the props, not debris
size_t g_next = 0;      // the next building of the round
double g_phase_at = 0, g_last_blast_at = 0, g_cycle_at = 0;
uint32_t g_live_before = 0, g_last_added = 0, g_explosions = 0;
debris_offload::Census g_before{};  // at the cycle's start
int64_t g_since = 0, g_last_qpc = 0;
bool g_shot = false;
std::vector<double> g_frames;
std::vector<std::pair<uint32_t, double>> g_results;  // per cycle: pieces, the overview's mean frame time

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

uint8_t* object_from_handle(uint32_t handle) {
    return rfg::fn<uint8_t*(__thiscall*)(void*, uint32_t)>(r::world__object_from_handle)(world(), handle);
}

const uint8_t* body_of(const uint8_t* obj) {
    const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
    return handle == 0xFFFFFFFFu ? nullptr : static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies))[handle];
}

const uint8_t* instance_of(const uint8_t* obj) {
    return obj && obj[kMoverLoaded] ? *reinterpret_cast<const uint8_t* const*>(obj + kMoverInstance) : nullptr;
}

// The sub-instances a building still holds, and all of its own.
uint32_t subs_left(const uint8_t* obj) {
    const uint8_t* inst = instance_of(obj);
    const int32_t n = inst ? *reinterpret_cast<const int32_t*>(inst + kInstNumSubs) : 0;
    return n > 0 ? uint32_t(n) : 0;
}

uint32_t subs_total(const uint8_t* obj) {
    const uint8_t* inst = instance_of(obj);
    const uint8_t* base = inst ? *reinterpret_cast<const uint8_t* const*>(inst + kInstShared) : nullptr;
    const int32_t n = base ? *reinterpret_cast<const int32_t*>(base + kBaseNumSubs) : 0;
    return n > 0 ? uint32_t(n) : 0;
}

bool standing(const uint8_t* obj) {
    if (!obj || (obj[kObjFlags] & kObjDestroyed) || !instance_of(obj)) return false;
    const uint8_t* body = body_of(obj);
    return body && body[kBodyMotionType] == kMotionFixed;
}

int general_movers() { return *reinterpret_cast<const int*>(world() + kWorldGeneralMoverCount); }
int havok_debris() { return general_movers() - g_movers_base; }

int32_t render_instances_free() {
    return rfg::global<int32_t>(r::rl_destroyable_model_instance__ms_object_pool + 0xC);
}

bool havok_heap(double& used_mb, double& total_mb) {
    const uint8_t* a = rfg::global<const uint8_t*>(r::s_largeBlockAllocator);
    if (!a) return false;
    const uint8_t* fixed = a + kHeapFixedServer;
    if (*reinterpret_cast<const uint8_t* const*>(a + kHeapServer) != fixed) return false;
    const uintptr_t start = *reinterpret_cast<const uintptr_t*>(fixed + kServerStart);
    const uintptr_t end = *reinterpret_cast<const uintptr_t*>(fixed + kServerEnd);
    const uint32_t used = *reinterpret_cast<const uint32_t*>(a + kHeapInUse);
    if (end <= start || used > end - start) return false;
    used_mb = used / 1048576.0;
    total_mb = double(end - start) / 1048576.0;
    return true;
}

// The memory census (test/memory_census.h), with Havok's heap named in it.
void census(const char* when) {
    uintptr_t lo = 0, hi = 0;
    if (const uint8_t* a = rfg::global<const uint8_t*>(r::s_largeBlockAllocator)) {
        const uint8_t* fixed = a + kHeapFixedServer;
        if (*reinterpret_cast<const uint8_t* const*>(a + kHeapServer) == fixed) {
            lo = *reinterpret_cast<const uintptr_t*>(fixed + kServerStart);
            hi = *reinterpret_cast<const uintptr_t*>(fixed + kServerEnd);
        }
    }
    memory_census::log(when, lo, hi);
}

// The address space (MB): free and its largest block; in use, by kind: private memory committed and only reserved,
// mapped views (shared memory, the graphics driver's), images.
struct AddressSpace {
    double free, largest, committed, reserved, mapped, images;
};

AddressSpace address_space() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uint64_t free_total = 0, largest = 0, committed = 0, reserved = 0, mapped = 0, images = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_FREE) {
            free_total += mbi.RegionSize;
            largest = std::max<uint64_t>(largest, mbi.RegionSize);
        } else if (mbi.Type == MEM_IMAGE) {
            images += mbi.RegionSize;
        } else if (mbi.Type == MEM_MAPPED) {
            mapped += mbi.RegionSize;
        } else {
            (mbi.State == MEM_COMMIT ? committed : reserved) += mbi.RegionSize;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    constexpr double kMb = 1048576.0;
    return {double(free_total) / kMb, double(largest) / kMb, double(committed) / kMb, double(reserved) / kMb,
            double(mapped) / kMb, double(images) / kMb};
}

Frames frame_stats(std::vector<double> v) {
    Frames f{0, 0, 0, 0, v.size()};
    if (v.empty()) return f;
    double sum = 0;
    for (const double ms : v) sum += ms;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(double(v.size()) * p))]; };
    f.mean = sum / double(v.size());
    f.p50 = pct(0.5), f.p95 = pct(0.95), f.p99 = pct(0.99);
    return f;
}

const uint8_t* vehicle_bomb() {
    const int n = rfg::global<int>(r::Num_explosion_infos);
    const auto* infos = static_cast<const uint8_t*>(rfg::addr(r::Explosion_infos));
    for (int i = 0; i < n && i < 79; ++i)
        if (_stricmp(reinterpret_cast<const char*>(infos + size_t(i) * kExplosionInfoSize), "vehicle_bomb") == 0)
            return infos + size_t(i) * kExplosionInfoSize;
    return nullptr;
}

// The standing buildings within the radius, nearest first, and the area they cover.
void pick(const float* player) {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    using GetBbox = void(__thiscall*)(uint8_t*, float*, float*);
    std::vector<std::pair<float, Building>> found;
    uint32_t inside = 0;
    for (int i = 0; all && movers && i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (!standing(obj)) continue;
        Building b{};
        (*reinterpret_cast<GetBbox* const*>(obj))[1](obj, b.lo, b.hi);
        const float volume = (b.hi[0] - b.lo[0]) * (b.hi[1] - b.lo[1]) * (b.hi[2] - b.lo[2]);
        const float d = std::hypot(0.5f * (b.lo[0] + b.hi[0]) - player[0], 0.5f * (b.lo[2] + b.hi[2]) - player[2]);
        b.subs = subs_total(obj);
        if (volume < kMinVolume || b.subs < kMinSubs || d > g_radius) continue;
        if (player[0] > b.lo[0] - kClearance && player[0] < b.hi[0] + kClearance &&
            player[2] > b.lo[2] - kClearance && player[2] < b.hi[2] + kClearance) {
            ++inside;
            continue;
        }
        b.handle = *reinterpret_cast<const uint32_t*>(obj + kObjHandle);
        found.emplace_back(d, b);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    g_buildings.clear();
    uint64_t subs = 0;
    for (size_t i = 0; i < found.size() && g_buildings.size() < kMaxBuildings; ++i) {
        g_buildings.push_back(found[i].second);
        subs += found[i].second.subs;
    }
    if (g_buildings.empty()) {
        LOG("stress: no building within %.0f m of the player (%d rfg_movers in all)", double(g_radius), n);
        return;
    }
    // The ground: the buildings' median base (a deep foundation or a shaft reaches far below the rest).
    float sx = 0, sz = 0;
    std::vector<float> bases;
    for (const Building& b : g_buildings) {
        sx += 0.5f * (b.lo[0] + b.hi[0]);
        sz += 0.5f * (b.lo[2] + b.hi[2]);
        bases.push_back(b.lo[1]);
    }
    std::nth_element(bases.begin(), bases.begin() + bases.size() / 2, bases.end());
    g_centre[0] = sx / float(g_buildings.size()), g_centre[1] = bases[bases.size() / 2];
    g_centre[2] = sz / float(g_buildings.size());
    g_extent = 0;
    for (const Building& b : g_buildings)
        for (const float x : {b.lo[0], b.hi[0]})
            for (const float z : {b.lo[2], b.hi[2]})
                g_extent = std::max(g_extent, std::hypot(x - g_centre[0], z - g_centre[2]));
    LOG("stress: %zu buildings within %.0f m of the player (%llu sub-instances: the pieces a cycle can add at most; %u "
        "more were too near the player), around (%.0f, %.0f, %.0f), reaching %.0f m from there; the largest: %u",
        g_buildings.size(), double(g_radius), static_cast<unsigned long long>(subs), inside, double(g_centre[0]),
        double(g_centre[1]), double(g_centre[2]), double(g_extent),
        std::max_element(g_buildings.begin(), g_buildings.end(),
                         [](const Building& a, const Building& b) { return a.subs < b.subs; })->subs);
}

// Vehicle bombs throughout a building's box, a little inside it and above its ground, top first: upper floors come
// apart before what holds them up goes.
void queue_blast(const Building& b) {
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) {
        lo[k] = b.lo[k] + 1.5f;
        hi[k] = b.hi[k] - 1.5f;
        if (hi[k] < lo[k]) lo[k] = hi[k] = 0.5f * (b.lo[k] + b.hi[k]);
    }
    lo[1] = std::min(b.lo[1] + 2.0f, hi[1]);
    auto cells = [](float a, float c, float step) { return std::max(1, int(std::ceil((c - a) / step))); };
    float step_xz = kBlastStepXZ, step_y = kBlastStepY;
    int nx, ny, nz;
    for (;;) {
        nx = cells(lo[0], hi[0], step_xz), ny = cells(lo[1], hi[1], step_y), nz = cells(lo[2], hi[2], step_xz);
        if (nx * ny * nz <= kMaxBlastsPerRound) break;
        step_xz *= 1.15f, step_y *= 1.15f;
    }
    g_queue.clear();
    g_queue_at = 0;
    for (int iy = ny - 1; iy >= 0; --iy)
        for (int ix = 0; ix < nx; ++ix)
            for (int iz = 0; iz < nz; ++iz)
                g_queue.push_back({{lo[0] + (hi[0] - lo[0]) * (float(ix) + 0.5f) / float(nx),
                                    lo[1] + (hi[1] - lo[1]) * (float(iy) + 0.5f) / float(ny),
                                    lo[2] + (hi[2] - lo[2]) * (float(iz) + 0.5f) / float(nz)}});
}

void explode_some() {
    static const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    // explosion_create(info, source, target, pos, orient, dir, weapon_info, bool), as activity raids demolish
    // buildings (no source, target, direction or weapon).
    const auto create = rfg::fn<void(__cdecl*)(const void*, void*, void*, float*, const float*, float*, void*, bool)>(
        r::explosion_create);
    for (int k = 0; k < kBlastsPerFrame && g_queue_at < g_queue.size(); ++k, ++g_explosions)
        create(g_info, nullptr, nullptr, g_queue[g_queue_at++].p, identity, nullptr, nullptr, false);
}

void start_cycle(double t) {
    ++g_cycle;
    g_round = 1;
    g_next = 0;
    g_cycle_at = t;
    g_before = debris_offload::census();
    g_live_before = g_before.live;
    g_movers_base = general_movers();
    g_explosions = 0;
    g_queue.clear();
    g_queue_at = 0;
    g_phase = kBlast;
}

size_t buildings_in() {
    return size_t(std::count_if(g_buildings.begin(), g_buildings.end(), [](const Building& b) { return !b.out; }));
}

// After the last round: a building that hardly came apart is left out of the next cycles.
void leave_out_whole() {
    uint32_t whole = 0;
    for (Building& b : g_buildings) {
        if (b.out) continue;
        const uint8_t* obj = object_from_handle(b.handle);
        if (standing(obj) && double(subs_left(obj)) > (1.0 - kNoShare) * double(b.subs)) {
            b.out = true;
            ++whole;
        }
    }
    if (whole) LOG("stress: cycle %d: %u buildings did not come apart; left out from now on", g_cycle, whole);
}

// The overview looks steeply down on the area from above its middle (a view from the side stood inside the hills
// round a valley).
void begin_overview(double t) {
    const float height = 1.2f * g_extent + 50.0f;
    const float target[3] = {g_centre[0], g_centre[1], g_centre[2]};
    const float from[3] = {g_centre[0], g_centre[1] + height, g_centre[2] - 0.3f * height};
    viewpoints::hold_view(from, target);
    g_frames.clear();
    g_shot = false;
    g_phase = kView;
    g_phase_at = t;
}

void log_cycle(double t) {
    const debris_offload::Census c = debris_offload::census();
    const Frames f = frame_stats(g_frames);
    double heap_used = 0, heap_total = 0;
    const bool heap = havok_heap(heap_used, heap_total);
    const AddressSpace va = address_space();
    const int added = int(c.live) - int(g_live_before);
    g_last_added = added > 0 ? uint32_t(added) : 0;
    g_results.emplace_back(c.live, f.mean);
    char heap_text[64] = "unknown";
    if (heap) snprintf(heap_text, sizeof(heap_text), "%.0f of %.0f MB in use", heap_used, heap_total);
    LOG("stress: cycle %d (%.0f s, %u explosions, %zu buildings): %u pieces in PhysX (%+d) of %u slots, %u piles "
        "holding %u, %u waiting for one; %d render instances free; Havok's heap %s; address space %.0f MB free (the "
        "largest block %.0f MB; private %.0f MB committed and %.0f reserved, mapped %.0f, images %.0f); overview %.2f "
        "ms a frame (%.0f FPS), p50 %.2f, p95 %.2f, p99 %.2f over %zu frames",
        g_cycle, t - g_cycle_at, g_explosions, buildings_in(), c.live, added, c.slots, c.piles, c.piled, c.waiting,
        render_instances_free(), heap_text, va.free, va.largest, va.committed, va.reserved, va.mapped, va.images,
        f.mean, f.mean > 0 ? 1000.0 / f.mean : 0.0, f.p50, f.p95, f.p99, f.n);
    const double frames = double(std::max<uint64_t>(1, c.task_frames - g_before.task_frames));
    auto task = [&](int k) { return (c.task_ms[k] - g_before.task_ms[k]) / frames; };
    LOG("stress: cycle %d piles: %llu joins, %llu left again, %llu pieces given up far away for render instances; %u "
        "still pieces of their own looked over; the render task %.2f ms a frame (moves %.2f, splits out of piles %.2f, "
        "piles %.2f, the rest %.2f)", g_cycle, static_cast<unsigned long long>(c.joined - g_before.joined),
        static_cast<unsigned long long>(c.unpiled - g_before.unpiled),
        static_cast<unsigned long long>(c.recycled - g_before.recycled), c.candidates,
        task(0) + task(1) + task(2) + task(3), task(0), task(1), task(2), task(3));
    char when[32];
    snprintf(when, sizeof(when), "cycle %d", g_cycle);
    census(when);
}

const char* stop_reason(double t) {
    const debris_offload::Census c = debris_offload::census();
    if (c.live >= g_target) return "the pieces asked for are there";
    if (uint64_t(c.live) + g_last_added + kSlotMargin > c.slots)
        return "the PhysX slots would not take another cycle (Experiment.PhysXMaxBodies)";
    double used = 0, total = 0;
    if (havok_heap(used, total) && used > kHeapStop * total) return "Havok's heap is nearly full";
    if (render_instances_free() < kInstancesStop) return "few render instances are left";
    const AddressSpace va = address_space();
    if (va.free < kVaFreeStopMb || va.largest < kVaLargestStopMb) return "little address space is left";
    g_small_cycles = g_last_added < kSmallCycle ? g_small_cycles + 1 : 0;
    if (g_small_cycles >= 2) return "two cycles in a row added almost nothing";
    if (g_cycle >= kMaxCycles) return "the most cycles were run";
    if (!buildings_in()) return "no building is left to blow up";
    if (g_stop_at_s > 0 && t + (t - g_cycle_at) > g_stop_at_s) return "another cycle would not end in the run's time";
    return nullptr;
}

void restore_all(double t) {
    const auto restore = rfg::fn<bool(__thiscall*)(void*, uint32_t)>(r::world__restore_object);
    uint32_t refused = 0;
    for (Building& b : g_buildings)
        if (!b.out && !restore(world(), b.handle)) {
            b.out = true;
            ++refused;
        }
    if (refused) LOG("stress: cycle %d: world::restore_object refused %u buildings; left out", g_cycle, refused);
    g_phase = kRestore;
    g_phase_at = t;
    g_restore_scans = world_mirror::flushed_scans();
}

void stop(const char* why, double t) {
    LOG("stress: stopping after %d cycles: %s", g_cycle, why);
    g_frames.clear();
    g_phase = kHold;
    g_phase_at = t;
}

void finish() {
    g_phase = kDone;
    for (size_t i = 0; i < g_results.size(); i += 8) {
        std::string line;
        for (size_t k = i; k < g_results.size() && k < i + 8; ++k) {
            char one[48];
            snprintf(one, sizeof(one), "%s%u: %.2f ms", line.empty() ? "" : ", ", g_results[k].first,
                     g_results[k].second);
            line += one;
        }
        LOG("stress: pieces and the overview's frame time, cycles %zu-%zu: %s", i + 1,
            std::min(g_results.size(), i + 8), line.c_str());
    }
    if (g_end_game) session::exit_now("stress test finished");
}

void on_frame(int game_state) {
    const int64_t now = session::qpc();
    const double frame_ms = g_last_qpc ? session::ticks_to_ms(now - g_last_qpc) : 0.0;
    g_last_qpc = now;
    if (g_phase == kDone) return;
    if (game_state != kGameplay) {
        if (g_phase != kWait) {
            LOG("stress: gameplay ended (state %d) in cycle %d", game_state, g_cycle);
            viewpoints::release_view();
            g_phase = kDone;
        }
        return;
    }
    if (!g_since) g_since = now;
    const double t = session::ticks_to_ms(now - g_since) / 1000.0;
    switch (g_phase) {
    case kWait: {
        if (t < g_start_s) return;
        // Not before the host has applied the static world: the avenue's takes it 35-45 s to cook, and blown up before
        // that, the rubble piled up in Havok (recycled as the zone's handles ran out) and came over inside the statics
        // of the buildings as they stood (research/docs/07, B5b).
        const bool world = world_mirror::flushed_scans() == UINT32_MAX || world_mirror::ready();
        if (!world && t < g_start_s + kWorldWaitS) return;
        const debris_offload::Census c = debris_offload::census();
        uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
        if (!player) return;
        g_phase = kDone;
        if (!world) LOG("stress: the host has not applied the static world %.0f s into gameplay; starting all the same", t);
        else if (t >= g_start_s + 1.0) LOG("stress: the host has the static world %.1f s into gameplay", t);
        if (!c.slots) {
            LOG("stress: debris does not go to PhysX (Experiment.PhysX, PhysXDebris): nothing to measure");
            return;
        }
        g_info = vehicle_bomb();
        if (!g_info) {
            LOG("stress: no vehicle_bomb explosion");
            return;
        }
        pick(reinterpret_cast<const float*>(player + kObjPos));
        if (g_buildings.empty()) return;
        // The rubble is to pile up at rest. The bombs kicked the rubble of the cycles before out of its piles (run 3:
        // 46,000 pieces a cycle, more than the render instances could take on their own); without the kicks, a building
        // restored over its rubble woke all of it when it came down again (run 4: 136,000 wakes, 51,000 out of piles a
        // cycle), where a real map has no rubble inside standing buildings. It stays put now, hidden in the walls.
        debris_offload::kick_pieces_for_test(false);
        pxbridge::set_test_flags(rfgphysx::kTestNoUnsupportedWakes);
        double used = 0, total = 0;
        const bool heap = havok_heap(used, total);
        const AddressSpace va = address_space();
        LOG("stress: at the start %u pieces in PhysX of %u slots, %d general movers in Havok, %d render instances "
            "free, Havok's heap %.0f of %.0f MB in use%s, address space %.0f MB free (the largest block %.0f MB; "
            "private %.0f MB committed and %.0f reserved, mapped %.0f, images %.0f); target %u pieces", c.live,
            c.slots, general_movers(), render_instances_free(), used, total, heap ? "" : " (unreadable: not watched)",
            va.free, va.largest, va.committed, va.reserved, va.mapped, va.images, g_target);
        census("at the start");
        start_cycle(t);
        return;
    }
    case kBlast: {
        if (g_queue_at < g_queue.size()) {
            explode_some();
            g_last_blast_at = t;
            return;
        }
        while (g_next < g_buildings.size()) {
            const Building& b = g_buildings[g_next];
            const uint8_t* obj = b.out ? nullptr : object_from_handle(b.handle);
            if (!standing(obj) || (g_round > 1 && double(subs_left(obj)) <= kStandingShare * double(b.subs))) {
                ++g_next;
                continue;
            }
            if (havok_debris() > kHavokDebrisGate) return;
            queue_blast(b);
            ++g_next;
            explode_some();
            g_last_blast_at = t;
            return;
        }
        if (t < g_last_blast_at + kRoundGapS) return;
        if (g_round < kMaxRounds) {
            ++g_round;
            g_next = 0;
            return;
        }
        leave_out_whole();
        g_phase = kSettle;
        g_phase_at = t;
        return;
    }
    case kSettle: {
        const double in = t - g_phase_at;
        const debris_offload::Census c = debris_offload::census();
        const bool caught_up = havok_debris() <= 100 && c.waiting <= std::max<uint32_t>(200, c.live / 100);
        if (in < kSettleMinS || (!caught_up && in < kSettleMaxS)) return;
        begin_overview(t);
        return;
    }
    case kView: {
        const double in = t - g_phase_at;
        if (in >= kMeasureFromS && in < kShotAtS) g_frames.push_back(frame_ms);
        if (!g_shot && in >= kShotAtS) {  // cycles 1, 2, 4, 8, ...: the screenshots stay few
            g_shot = true;
            if ((g_cycle & (g_cycle - 1)) == 0)
                screenshot::request(session::run_dir() + L"\\stress_cycle_" + std::to_wstring(g_cycle) + L".png");
        }
        if (in < kViewS) return;
        viewpoints::release_view();
        log_cycle(t);
        if (const char* why = stop_reason(t)) {
            stop(why, t);
            return;
        }
        restore_all(t);
        return;
    }
    case kRestore: {
        const double in = t - g_phase_at;
        if (in < kRestoreMinS) return;
        uint32_t back = 0, away = 0;
        for (const Building& b : g_buildings) {
            if (b.out) continue;
            const uint8_t* obj = object_from_handle(b.handle);
            ++(standing(obj) && double(subs_left(obj)) >= 0.9 * double(b.subs) ? back : away);
        }
        if (away && in < kRestoreWaitS) return;
        // and the host has them as statics again: a scan since the restore sent, the host through its commands. Blown
        // up while it still cooked them (34,000 statics took it seconds), the new pieces came over inside the old
        // statics, and thousands were pushed out underneath, through the ground (research/docs/07, B5b).
        const uint32_t scans = world_mirror::flushed_scans();
        const bool mirrored =
            scans == UINT32_MAX || (scans != g_restore_scans && pxbridge::ring_used() < (64u << 10));
        if (!mirrored && in < kRestoreWaitS) return;
        if (away) {
            for (Building& b : g_buildings) {
                const uint8_t* obj = b.out ? nullptr : object_from_handle(b.handle);
                if (!b.out && !(standing(obj) && double(subs_left(obj)) >= 0.9 * double(b.subs))) b.out = true;
            }
            LOG("stress: cycle %d: %u buildings did not stand again within %.0f s; left out", g_cycle, away,
                kRestoreWaitS);
        }
        start_cycle(t);
        return;
    }
    case kHold: {
        const double in = t - g_phase_at;
        if (in >= kHoldMeasureFromS) g_frames.push_back(frame_ms);
        if (in < kHoldS) return;
        const Frames f = frame_stats(g_frames);
        LOG("stress: at rest, from the player's view, %u pieces: %.2f ms a frame (%.0f FPS), p50 %.2f, p95 %.2f, p99 "
            "%.2f over %zu frames", debris_offload::census().live, f.mean, f.mean > 0 ? 1000.0 / f.mean : 0.0, f.p50,
            f.p95, f.p99, f.n);
        if (g_viewpoints) {
            viewpoints::begin();
            g_phase = kViews;
            return;
        }
        finish();
        return;
    }
    case kViews:
        if (viewpoints::finished()) finish();
        return;
    case kDone:
        return;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.test_stress_pieces <= 0) return true;
    g_target = uint32_t(cfg.test_stress_pieces);
    g_radius = float(std::clamp(cfg.test_stress_radius_m, 20, 1000));
    g_start_s = std::max(5, cfg.test_stress_start_s);
    g_viewpoints = cfg.test_viewpoints != 0;
    g_end_game = cfg.exit_after_gameplay_s > 0;
    if (g_end_game) g_stop_at_s = std::max(0.0, double(cfg.exit_after_gameplay_s) - kEndRoomS);
    session::add_frame_callback(&on_frame);
    LOG("stress test armed: %.0f s into gameplay the buildings within %.0f m are blown up and restored over and over, "
        "their rubble kept, up to %u pieces%s", g_start_s, double(g_radius), g_target,
        g_viewpoints ? "; then the viewpoints" : "");
    return true;
}

}  // namespace stress_pieces
