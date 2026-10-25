#include "experiments/keep_debris.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "physx/debris_offload.h"
#include "telemetry/object_census.h"

namespace experiments {
namespace {

namespace r = rfg::rva;

// object_pool<T, N, 1>: an object_pool_server<T> at offset 0 (elements, links, list ids, constructor callbacks,
// capacity, free-list head, free count), the pool's client after it.
constexpr uint32_t kPoolCapacity = 0x14, kPoolFree = 0x1C;
constexpr uint32_t kGameDebrisPool = 3072;
constexpr uint32_t kMaxDebrisPool = 65000;  // the free list links elements with 16-bit indices

bool g_on = false;
uint32_t g_pool_capacity = 0;  // requested general_mover capacity (0: the game's)

// ---- world zone budget ---------------------------------------------------------------------------------------------

// Every world object takes a handle from its world zone, and a zone has 7,280 of them (handle_manager<7280>, a bitmap
// in world_zone). Debris goes to the zone at its position; in Wrecking Crew the level is one zone. When a zone runs out
// of handles nothing more can be created in it: no pieces, but also no projectiles, and none of the objects the level
// streams in by distance, so far objects vanish and come back while handles come and go. The budget keeps
// Experiment.ZoneReserve handles free in every zone: shattering keeps its extra pieces only while more than half the
// reserve is free, and below the reserve the debris farthest from the camera is recycled the way the game's load
// governor does it (out of view: removed; in view: faded out).
constexpr uint32_t kWorldAllObjects = 0x64;       // world::all_objects.elt: object* by object::all_index
constexpr uint32_t kWorldObjectCount = 0x6C;      // world::all_objects.num
constexpr uint32_t kWorldGeneralMovers = 0x3C4;   // world::subtype_objects[OT_SUB_MOVER_GENERAL].elt: all_index list
constexpr uint32_t kWorldGeneralMoverCount = 0x3CC;
constexpr uint32_t kWorldZoneCount = 0x271C, kWorldZones = 0x2720;  // world::num_territory_zones, all_zones
constexpr uint32_t kZoneHandleBits = 0xE0;        // world_zone::obj_handle_mgr.status: one bit per handle in use
constexpr uint32_t kZoneGid = 0x47C;              // world_zone::gid: the upper 16 bits of the zone's handles
constexpr int kZoneHandles = 7280;
constexpr uint32_t kObjHandle = 0x6C, kObjFlags = 0x54, kMoverFlags = 0xE0;  // object::handle, obj_flags; mover_flags
constexpr uint8_t kObjDestroyed = 0x10;           // object_flags::destroyed (world::destroy_object; freed later)
constexpr uint32_t kMoverLoadBalanced = 4;        // movers the load governor may delete (rfgl_should_delete): debris
constexpr uint32_t kCameraPos = 0x2C;             // Rfg_camera position, what rfgl_should_delete measures from
constexpr uint32_t kObjPos = 0x04;                // object::pos
constexpr int kRecycleBatch = 64;                 // recycle at least this many at a time

int g_zone_reserve = 1200;                        // Experiment.ZoneReserve
std::atomic<int> g_zone_free{kZoneHandles};       // free handles in the fullest zone, less the pieces kept since
std::atomic<uint32_t> g_recycled{0}, g_gate_deletes{0}, g_handle_failures{0};
int32_t g_zone_used = 0;                          // the fullest zone's handles in use at the last frame end
bool g_recycling = false;                         // sim thread: our own fade marks pass the fade filter
bool g_recycle_logged = false;

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }
int32_t world_objects() { return *reinterpret_cast<int32_t*>(world() + kWorldObjectCount); }

int zone_used(const uint8_t* zone) {
    const uint8_t* bits = zone + kZoneHandleBits;
    constexpr int bytes = kZoneHandles / 8;  // 910
    int used = 0;
    int i = 0;
    for (; i + 4 <= bytes; i += 4) used += int(__popcnt(*reinterpret_cast<const uint32_t*>(bits + i)));
    for (; i < bytes; ++i) used += int(__popcnt(bits[i]));
    return used;
}

const uint8_t* fullest_zone(int& used) {
    uint8_t* w = world();
    const int n = *reinterpret_cast<int*>(w + kWorldZoneCount);
    uint8_t* const* zones = *reinterpret_cast<uint8_t* const**>(w + kWorldZones);
    const uint8_t* best = nullptr;
    used = 0;
    for (int i = 0; zones && i < n; ++i) {
        if (!zones[i]) continue;
        const int u = zone_used(zones[i]);
        if (u > used) {
            used = u;
            best = zones[i];
        }
    }
    return best;
}

bool object_visible(uint8_t* obj) {
    if (!rfg::fn<bool(__cdecl*)(uint8_t*)>(r::object_mover_is_visible)(obj)) return false;
    float bmin[3], bmax[3];
    using GetBbox = void(__thiscall*)(uint8_t*, float*, float*);  // vtable slot 1: object::get_bbox(min, max)
    (*reinterpret_cast<GetBbox* const*>(obj))[1](obj, bmin, bmax);
    return rfg::fn<bool(__cdecl*)(float*, float*)>(r::game_render_is_object_visible_bbox)(bmin, bmax);
}

struct Candidate {
    float d2;
    uint8_t* obj;
};
std::vector<Candidate> g_candidates;  // sim thread
uint32_t g_rng = 0x9E3779B9u;

// Frees `need` handles in `zone` (pieces already fading or destroyed count): debris farthest from the camera first.
void recycle(const uint8_t* zone, int need) {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<int*>(w + kWorldGeneralMoverCount);
    if (!all || !movers || n <= 0) return;
    const uint32_t gid = *reinterpret_cast<const uint16_t*>(zone + kZoneGid);
    const float* cam = reinterpret_cast<const float*>(static_cast<uint8_t*>(rfg::addr(r::Rfg_camera)) + kCameraPos);
    const auto is_fading = rfg::fn<bool(__cdecl*)(uint8_t*)>(r::object_mover_is_fading_for_delete);
    int in_zone = 0, leaving = 0;
    g_candidates.clear();
    for (int i = 0; i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (!obj || (*reinterpret_cast<uint32_t*>(obj + kObjHandle) >> 16) != gid) continue;
        ++in_zone;
        if (!(*reinterpret_cast<uint32_t*>(obj + kMoverFlags) & kMoverLoadBalanced)) continue;
        if ((obj[kObjFlags] & kObjDestroyed) || is_fading(obj)) {
            ++leaving;
            continue;
        }
        const float* p = reinterpret_cast<const float*>(obj + kObjPos);
        const float dx = p[0] - cam[0], dy = p[1] - cam[1], dz = p[2] - cam[2];
        g_candidates.push_back({dx * dx + dy * dy + dz * dz, obj});
    }
    if (!g_recycle_logged) {
        g_recycle_logged = true;
        LOG("experiment KeepDebris: world zone %u is %d handles short of its reserve: %d general movers in it, %d of "
            "them debris (%d already leaving); recycling the farthest", gid, need, in_zone,
            int(g_candidates.size()) + leaving, leaving);
    }
    need -= leaving;
    if (need <= 0 || g_candidates.empty()) return;
    const size_t take = size_t(need) < g_candidates.size() ? size_t(need) : g_candidates.size();
    std::nth_element(g_candidates.begin(), g_candidates.begin() + ptrdiff_t(take), g_candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.d2 > b.d2; });
    // havok_get_status(HAVOK_SYSTEM_MAIN) == 2: the game removes instead of fading (rfgl_find_and_delete_object_mover).
    const bool remove_all = rfg::fn<int(__cdecl*)(int)>(r::havok_get_status)(0) == 2;
    const auto has_script_effect =
        rfg::fn<bool(__thiscall*)(uint8_t*, uint32_t, int*)>(r::object_mover__has_script_point_effect);
    const auto mark_for_fade = rfg::fn<void(__cdecl*)(uint8_t*, int)>(r::object_mover_mark_for_fade);
    const auto destroy = rfg::fn<bool(__thiscall*)(void*, uint8_t*, bool)>(r::world__destroy_object);
    uint32_t done = 0;
    g_recycling = true;
    for (size_t i = 0; i < take; ++i) {
        uint8_t* obj = g_candidates[i].obj;
        if (has_script_effect(obj, 0xFFFFFFFFu, nullptr)) continue;
        if (!remove_all && object_visible(obj)) {
            g_rng = g_rng * 1664525u + 1013904223u;
            mark_for_fade(obj, int((g_rng >> 8) % 2000u));  // like the game: within 2 s
        } else {
            destroy(w, obj, false);
        }
        ++done;
    }
    g_recycling = false;
    g_recycled.fetch_add(done, std::memory_order_relaxed);
}

constexpr int kGameplayState = 1;  // GS_GAMEPLAY

// Sim thread, end of frame; registered with or without KeepDebris (telemetry reports the zones either way). Zones are
// only read in gameplay: loading and leaving a level rebuild them.
void log_shatter_burst(int free_at_start);  // with the shatter hook below

void update_budget(int game_state) {
    if (game_state != kGameplayState) return;
    log_shatter_burst(kZoneHandles - g_zone_used);
    int used = 0;
    const uint8_t* zone = fullest_zone(used);
    g_zone_used = used;
    const int free = kZoneHandles - used;
    g_zone_free.store(free, std::memory_order_relaxed);
    if (g_on && zone && free < g_zone_reserve && rfg::global<uint8_t>(r::Multiplayer) == 0)
        recycle(zone, g_zone_reserve + kRecycleBatch - free);
}

// handle_manager<7280>::allocate_handle(): 0xFFFFFFFF when the zone is full (the game goes on with that handle).
uint32_t(__fastcall* o_allocate_handle)(void*, void*);
uint32_t __fastcall d_allocate_handle(void* mgr, void* edx) {
    const uint32_t h = o_allocate_handle(mgr, edx);
    if (h == 0xFFFFFFFFu) {
        const uint32_t n = g_handle_failures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 5 || n % 1000 == 0) LOG("experiment KeepDebris: a world zone is out of object handles (%u so far)", n);
    }
    return h;
}

// Whether to keep debris right now: single player, and more than half the zone reserve free.
bool keeping() {
    return g_on && rfg::global<uint8_t>(r::Multiplayer) == 0 &&
           g_zone_free.load(std::memory_order_relaxed) > g_zone_reserve / 2;
}
std::atomic<uint32_t> g_fades_blocked{0}, g_deletes_blocked{0}, g_vaporize_refused{0};

bool single_player() { return rfg::global<uint8_t>(r::Multiplayer) == 0; }
inline bool within(uint32_t rva, uint32_t fn, uint32_t size) { return rva - fn < size; }

// Census fade filter (telemetry/object_census.h): whether a fade mark goes through.
bool allow_fade(uint8_t*, uint32_t caller) {
    if (g_recycling || !keeping()) return true;
    // AI vehicles clear the debris they are stuck on (vehicle_ai_process_stuck): gameplay, left alone.
    if (within(caller, r::do_debris_cleanup, rfg::size::do_debris_cleanup)) return true;
    // With the governor's deletion pass off, rfgl_find_and_delete_object_mover only runs for its emergency cleanup
    // (Havok is out of memory: without it the game stops in havok_memory_listener::allocationFailure).
    if (within(caller, r::rfgl_find_and_delete_object_mover, rfg::size::rfgl_find_and_delete_object_mover)) return true;
    g_fades_blocked.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// rfgl_desired_values(rfgl_balance_values* out, int pieces, int flags), called by rfg_dyn_shardify_setup when a
// chunk shatters: out = {quality_count, delete_count}, delete_count = Recent_history_objects * 0.015 * load^2 *
// pieces and at least all but 20. The setup deletes that many of the small pieces on the spot and keeps at most
// Max_quality_objects_per_frame (10) per frame as lasting pieces; the rest become shards that fade after 2-5 s.
// Flags 0x20 (farther than 75 m from the camera, NEAR_PLAYER_DIST_SQRD) and 0x40 (out of view) count at load 1.5 and
// make the small pieces world-only.
// delete_count is not capped at the pieces: after a big shatter it is several times their number (12 times when a
// shockwave mod shattered a building), so only up to `pieces` of it is real. Counting the rest against the zone
// budget used the budget up within the frame, and the building's other chunks were deleted at once.
// A frame's shatters are logged when they come to kBurstPieces or more (update_budget).
constexpr uint32_t kBurstPieces = 500;
constexpr int kShatterFar = 0x20, kShatterHidden = 0x40;
std::atomic<uint32_t> g_burst_chunks{0}, g_burst_pieces{0}, g_burst_far{0}, g_burst_hidden{0}, g_burst_kept{0},
    g_burst_gated{0};

void(__cdecl* o_desired)(int32_t*, int, int);
void __cdecl d_desired(int32_t* out, int pieces, int flags) {
    o_desired(out, pieces, flags);
    if (!out || !g_on || rfg::global<uint8_t>(r::Multiplayer) != 0) return;
    const int n = pieces > 0 ? pieces : 0;
    g_burst_chunks.fetch_add(1, std::memory_order_relaxed);
    g_burst_pieces.fetch_add(uint32_t(n), std::memory_order_relaxed);
    if (flags & kShatterFar) g_burst_far.fetch_add(uint32_t(n), std::memory_order_relaxed);
    if (flags & kShatterHidden) g_burst_hidden.fetch_add(uint32_t(n), std::memory_order_relaxed);
    const int deletes = std::clamp(out[1], 0, n);
    // The pieces the game creates anyway, then the extra ones that fit while half the zone reserve stays free.
    const int created = n - deletes;
    const int room = g_zone_free.load(std::memory_order_relaxed) - g_zone_reserve / 2 - created;
    const int kept = room <= 0 ? 0 : room < deletes ? room : deletes;
    g_zone_free.fetch_sub(created + kept, std::memory_order_relaxed);
    out[1] = deletes - kept;
    if (kept > 0) {
        g_deletes_blocked.fetch_add(uint32_t(kept), std::memory_order_relaxed);
        g_burst_kept.fetch_add(uint32_t(kept), std::memory_order_relaxed);
    }
    if (deletes > kept) {
        g_gate_deletes.fetch_add(uint32_t(deletes - kept), std::memory_order_relaxed);
        g_burst_gated.fetch_add(uint32_t(deletes - kept), std::memory_order_relaxed);
    }
}

void log_shatter_burst(int free_at_start) {
    const uint32_t pieces = g_burst_pieces.exchange(0, std::memory_order_relaxed);
    const uint32_t chunks = g_burst_chunks.exchange(0, std::memory_order_relaxed);
    const uint32_t distant = g_burst_far.exchange(0, std::memory_order_relaxed);
    const uint32_t hidden = g_burst_hidden.exchange(0, std::memory_order_relaxed);
    const uint32_t kept = g_burst_kept.exchange(0, std::memory_order_relaxed);
    const uint32_t gated = g_burst_gated.exchange(0, std::memory_order_relaxed);
    if (pieces >= kBurstPieces)
        LOG("experiment KeepDebris: shatter burst: %u chunks, %u pieces (in shatters over 75 m from the camera %u, out "
            "of view %u), %u kept from the game's deletion, %u deleted at once (the zone budget, %d handles free)",
            chunks, pieces, distant, hidden, kept, gated, free_at_start);
}

// rfg_dyn_maybe_delete(mover, ...): object_mover_process_explosion_hit asks whether an explosion's impulse is strong
// enough to "vaporize" the piece; if so it marks it for fade and returns WITHOUT applying the impulse. With fades
// refused such pieces would just sit there, so they take the impulse instead.
bool(__cdecl* o_maybe_delete)(void*, float, void*, float);
bool __cdecl d_maybe_delete(void* mover, float a, void* impulse, float b) {
    if (keeping()) {
        g_vaporize_refused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return o_maybe_delete(mover, a, impulse, b);
}

// havok_collect_objects_fast(center, radius, handles, max, flags, filter, data): explosion_apply_damage collects the
// objects an explosion hits into a 256-handle buffer on its stack, in broadphase order. In a pile of thousands of
// pieces that is 256 arbitrary ones, often not those at the blast. With kept debris the explosion gets the 256
// nearest to its center instead.
constexpr int kExplosionCollectMax = 16384;
using CollectSphere = int(__cdecl*)(const float*, float, uint32_t*, int, uint32_t, void*, void*);
CollectSphere o_collect_sphere = nullptr;
std::atomic<uint32_t> g_explosion_hits_dropped{0};

int __cdecl d_collect_sphere(const float* center, float radius, uint32_t* handles, int max, uint32_t flags,
                             void* filter, void* data) {
    const uint32_t caller = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base());
    if (!g_on || !center || max <= 0 || !within(caller, r::explosion_apply_damage, rfg::size::explosion_apply_damage) ||
        !single_player())
        return o_collect_sphere(center, radius, handles, max, flags, filter, data);
    static std::vector<uint32_t> all(kExplosionCollectMax);  // sim thread only (explosion_do_frame)
    const int n = o_collect_sphere(center, radius, all.data(), kExplosionCollectMax, flags, filter, data);
    if (n <= max) {
        std::copy(all.begin(), all.begin() + n, handles);
        return n;
    }
    struct Hit {
        float d2;
        uint32_t handle;
    };
    static std::vector<Hit> hits;
    hits.clear();
    void* world = rfg::addr(r::World);
    const auto from_handle = rfg::fn<uint8_t*(__fastcall*)(void*, void*, uint32_t)>(r::world__object_from_handle);
    for (int i = 0; i < n; ++i) {
        const uint8_t* obj = from_handle(world, nullptr, all[size_t(i)]);
        if (!obj) continue;
        const float* p = reinterpret_cast<const float*>(obj + kObjPos);
        const float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
        hits.push_back({dx * dx + dy * dy + dz * dz, all[size_t(i)]});
    }
    const int keep = int(hits.size()) < max ? int(hits.size()) : max;
    std::nth_element(hits.begin(), hits.begin() + keep, hits.end(), [](const Hit& a, const Hit& b) { return a.d2 < b.d2; });
    for (int i = 0; i < keep; ++i) handles[i] = hits[size_t(i)].handle;
    g_explosion_hits_dropped.fetch_add(uint32_t(n - keep), std::memory_order_relaxed);
    return keep;
}

// world::collect_sorted_list(list, bpi): world streaming walks each object's parent chain through object::parent; a
// parent that no longer resolves (gone, or flagged for destruction) makes the lookup return null and the next read
// crashes (+0x70 of null). Kept debris can outlive such a parent; the walk is guarded and the hierarchy skipped.
void(__fastcall* o_collect_sorted)(void*, void*, void*, void*) = nullptr;
std::atomic<uint32_t> g_collect_faults{0};

bool collect_sorted_guarded(void* world, void* list, void* bpi) {  // no C++ objects here: SEH
    __try {
        o_collect_sorted(world, nullptr, list, bpi);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void __fastcall d_collect_sorted(void* world, void* edx, void* list, void* bpi) {
    (void)edx;
    if (collect_sorted_guarded(world, list, bpi)) return;
    const uint32_t n = g_collect_faults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || n % 1000 == 0)
        LOG("experiment KeepDebris: world streaming met an object whose parent is gone (%u so far); skipped it", n);
}

// rfgl_maybe_delete_objects(): the load governor deletes movers and debris objects when the physics load is high
// or the destroyable render instances run low.
void(__cdecl* o_governor)();
void __cdecl d_governor() {
    if (keeping()) return;
    o_governor();
}

// ---- general_mover pool ----------------------------------------------------------------------------------------

// A mempool_base for object_pool_server::init: it allocates the element, link and list-id arrays through vtable
// slot 7 (alloc(size, flags)). The other slots are never called by init; they end the process if they ever are.
void* __fastcall pool_alloc(void*, void*, uint32_t size, uint32_t) {
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) LOG("experiment DebrisPool: VirtualAlloc of %u bytes failed", size);
    return p;
}
void __fastcall pool_trap(void*, void*) {
    session::exit_now("experiment DebrisPool: unexpected mempool call during pool initialization");
}
void* g_mempool_vtable[16];
struct {
    void** vtable;
} g_mempool{g_mempool_vtable};

using ServerInit = void(__fastcall*)(uint8_t* server, void* edx, void* mempool, uint32_t capacity);
ServerInit o_server_init = nullptr;

uint8_t* debris_pool() { return static_cast<uint8_t*>(rfg::addr(r::general_moverpool)); }

// object_pool_server<general_mover>::init(mempool_base*, uint capacity): the pool constructor (static
// initialization) calls it with the pool's own inline storage and 3,072 slots; the bigger pool gets our arrays.
void __fastcall d_server_init(uint8_t* server, void* edx, void* mempool, uint32_t capacity) {
    if (server == debris_pool() && capacity == kGameDebrisPool && g_pool_capacity > kGameDebrisPool) {
        o_server_init(server, edx, &g_mempool, g_pool_capacity);
        LOG("experiment DebrisPool: general_mover pool built with %u slots (game: %u)",
            *reinterpret_cast<uint32_t*>(server + kPoolCapacity), capacity);
        return;
    }
    o_server_init(server, edx, mempool, capacity);
}

bool setup_pool() {
    for (void*& slot : g_mempool_vtable) slot = reinterpret_cast<void*>(&pool_trap);
    g_mempool_vtable[7] = reinterpret_cast<void*>(&pool_alloc);
    uint8_t* pool = debris_pool();
    const uint32_t capacity = *reinterpret_cast<uint32_t*>(pool + kPoolCapacity);
    const uint32_t free = *reinterpret_cast<uint32_t*>(pool + kPoolFree);
    if (capacity == 0) {
        // Not constructed yet (the plugin starts before the game's static initializers): enlarge it when it is.
        return hooks::create(r::object_pool_server_general_mover___init, &d_server_init, &o_server_init,
                             "object_pool_server<general_mover>::init");
    }
    if (capacity != kGameDebrisPool || free != capacity) {
        LOG("experiment DebrisPool: pool already in use (%u of %u slots free), left at the game's size", free, capacity);
        return true;
    }
    rfg::fn<ServerInit>(r::object_pool_server_general_mover___init)(pool, nullptr, &g_mempool, g_pool_capacity);
    LOG("experiment DebrisPool: general_mover pool rebuilt with %u slots (game: %u)",
        *reinterpret_cast<uint32_t*>(pool + kPoolCapacity), capacity);
    return true;
}

// world::system_init sizes the world's object lists (all objects, one list per type and per subtype, the handle hash)
// from obj_class_instantiator::max_objects, where general_mover's class registers 3,072, its pool size. General
// movers beyond that are left out of the lists: no per-frame update (general_mover_do_frame_pre_sim), no cleanup at
// the end of a level, and the load governor and the zone budget cannot see them. The class maximum follows the
// pool, as far as the lists' 16-bit indices (65,535 objects in all) and the permanent memory pool allow.
constexpr int32_t kMaxWorldObjects = 0xFFFF;
constexpr uint32_t kListBytesPerObject = 32;  // two object lists, all_objects, the handle hash's bucket and entry (about 30)
constexpr uint32_t kPermanentMargin = 1u << 20;

uint32_t permanent_pool_free() {
    void* pool = rfg::addr(r::Permanent_mempool);
    using Free = uint32_t(__thiscall*)(void*);  // mempool_base vtable slot 3: bytes left (static_mempool_base::alloc)
    return (*reinterpret_cast<Free* const*>(pool))[3](pool);
}

void(__fastcall* o_world_system_init)(void*, void*);
void __fastcall d_world_system_init(void* w, void* edx) {
    const uint32_t hash_size = rfg::global<uint32_t>(r::obj_class_instantiator__hash_size);
    void* const* alloc_funcs = &rfg::global<void*>(r::obj_class_instantiator__alloc_funcs);
    int32_t* max_objects = &rfg::global<int32_t>(r::obj_class_instantiator__max_objects);
    const int32_t* class_index = &rfg::global<int32_t>(r::obj_class_instantiator__class_index);
    const int32_t classes = rfg::global<int32_t>(r::obj_class_instantiator__num_registered_classes);
    int slot = -1;
    for (uint32_t i = 0; i < hash_size && i < 768; ++i)
        if (alloc_funcs[i] == rfg::addr(r::alloc_general_mover)) slot = int(i);
    if (slot < 0) {
        LOG("experiment DebrisPool: general_mover's class is not registered; its object lists stay at the game's size");
        o_world_system_init(w, edx);
        return;
    }
    int32_t total = 0;
    for (int32_t i = 0; i < classes && i < 768; ++i) total += max_objects[class_index[i]];
    const int32_t old = max_objects[slot];
    const uint32_t free = permanent_pool_free();
    int32_t want = int32_t(g_pool_capacity);
    if (want > kMaxWorldObjects - (total - old)) want = kMaxWorldObjects - (total - old);
    const int32_t fits = free > kPermanentMargin ? old + int32_t((free - kPermanentMargin) / kListBytesPerObject) : old;
    if (want > fits) want = fits;
    if (want > old) max_objects[slot] = want;
    o_world_system_init(w, edx);
    LOG("experiment DebrisPool: general_mover object lists %d -> %d (world objects %d -> %d), permanent memory pool "
        "%u -> %u KB free", old, max_objects[slot], total, total - old + max_objects[slot], free >> 10,
        permanent_pool_free() >> 10);
}

// ---- render instances --------------------------------------------------------------------------------------------

// Every building chunk and debris piece is drawn through an rl_destroyable_model_instance. game_render_system_init
// caps them at 2,100 (shared bases 2,200) and, once they run out, shattering creates no more pieces; the load
// governor keeps 1,600 of them in use at most (Desired_destroyable_peak). factory_init then sizes one memory block
// from every render class's m_objects_max ((class size + 20) bytes each), so raising the maxima before it runs is
// all it takes. rl_fp_kdtree's maximum is the sum of several classes' maxima, the destroyable ones among them.
constexpr uint32_t kFactoryObjectsMax = 0x20, kFactoryObjectData = 0x28;
int g_render_instances = 0;  // Experiment.RenderInstances

void(__cdecl* o_factory_init)();
void __cdecl d_factory_init() {
    int32_t& inst = rfg::global<int32_t>(r::rl_destroyable_model_instance__ms_class_factory_info + kFactoryObjectsMax);
    int32_t& shared = rfg::global<int32_t>(r::rl_destroyable_model_instance_shared_base__ms_class_factory_info + kFactoryObjectsMax);
    int32_t& kdtree = rfg::global<int32_t>(r::rl_fp_kdtree__ms_class_factory_info + kFactoryObjectsMax);
    int32_t& visible = rfg::global<int32_t>(r::rl_scene_renderer__ms_visible_renderable_instances_max);
    int32_t& scene = rfg::global<int32_t>(r::rl_scene__ms_renderable_instances_max);
    if (g_render_instances > inst) {
        const int32_t old_inst = inst, old_shared = shared, old_visible = visible;
        const int32_t add = g_render_instances - inst;
        inst += add;
        shared += add;
        kdtree += 2 * add;
        visible += add;
        scene += add;
        LOG("experiment RenderInstances: destroyable model instances %d -> %d, shared bases %d -> %d, visible "
            "renderable instances %d -> %d", old_inst, inst, old_shared, shared, old_visible, visible);
    }
    o_factory_init();
}

bool setup_render_instances() {
    if (rfg::global<uint32_t>(r::rl_destroyable_model_instance__ms_class_factory_info + kFactoryObjectData) != 0) {
        LOG("experiment RenderInstances: the renderer is already initialized, its limits stay");
        return true;
    }
    return hooks::create(r::factory_init, &d_factory_init, &o_factory_init, "factory_init");
}

// ---- Havok heap ----------------------------------------------------------------------------------------------------

// rfg_init_stage_1_prepare gives Havok one 75 MB block (mempool_init_alloc(0x4B00000) from the game's start-up zone,
// then Havok_mempool.init(block, 0x4B00000, ...)); havok_memory_init hands all of it to Havok's allocators. When it
// runs out, havok_memory_listener::allocationFailure prints statistics and loops forever. Kept debris in piles
// fills it (about 15 KB per piece in colliding piles), so Experiment.HavokHeapMB gives Havok a bigger block of
// our own instead.
constexpr uint32_t kGameHavokHeap = 0x4B00000;
uint32_t g_havok_heap_bytes = 0;
void* g_havok_heap = nullptr;

uint8_t*(__cdecl* o_mempool_init_alloc)(int);
uint8_t* __cdecl d_mempool_init_alloc(int size) {
    const uint32_t caller = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base());
    if (uint32_t(size) == kGameHavokHeap && !g_havok_heap &&
        within(caller, r::rfg_init_stage_1_prepare, rfg::size::rfg_init_stage_1_prepare)) {
        g_havok_heap = VirtualAlloc(nullptr, g_havok_heap_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_havok_heap) return static_cast<uint8_t*>(g_havok_heap);
        LOG("experiment HavokHeapMB: could not allocate %u MB, Havok keeps the game's 75 MB", g_havok_heap_bytes >> 20);
    }
    return o_mempool_init_alloc(size);
}

// mempool::init(buffer, size, name, alignment, flags)
void(__fastcall* o_mempool_init)(uint8_t*, void*, void*, uint32_t, const char*, uint32_t, uint8_t);
void __fastcall d_mempool_init(uint8_t* pool, void* edx, void* buffer, uint32_t size, const char* name, uint32_t align,
                               uint8_t flags) {
    if (pool == rfg::addr(r::Havok_mempool) && g_havok_heap && buffer == g_havok_heap && size == kGameHavokHeap) {
        size = g_havok_heap_bytes;
        LOG("experiment HavokHeapMB: Havok heap %u MB at %p (game: %u MB)", size >> 20, buffer, kGameHavokHeap >> 20);
    }
    o_mempool_init(pool, edx, buffer, size, name, align, flags);
}

// ---- restored objects ----------------------------------------------------------------------------------------------

// restore_destroy_object(restored table, handle): an object world::restore_object restores (an activity's building
// when the activity starts over, a structure the EDF rebuilds) is destroyed, with its descendants, before its zone
// streams it in again. The debris that broke off it would have faded out long before in the game; kept debris stays,
// and piled up at a demolition site with every replay of the activity, in the way of the next. It goes along now: the
// general_movers whose original_object is the restored object, and its pieces in PhysX (debris_offload). The hook
// only notes the handle: loading a save restores thousands of objects (a pass over the debris for each one froze the
// load), and the game is still walking the restored object's hierarchy. At the end of the next gameplay frame one
// pass over the general movers takes the debris of every object restored since.
constexpr uint32_t kGmOriginalObject = 0x128;  // general_mover::original_object: the object it broke off
SRWLOCK g_restored_lock = SRWLOCK_INIT;
std::unordered_set<uint32_t> g_restored_handles;  // under g_restored_lock
std::atomic<uint32_t> g_restored_debris{0};
uint32_t g_restore_logs = 0;
std::vector<uint8_t*> g_restore_matches;  // sim thread

void(__cdecl* o_restore_destroy)(void*, uint32_t) = nullptr;
void __cdecl d_restore_destroy(void* restored, uint32_t handle) {
    if (handle != 0xFFFFFFFFu) {
        AcquireSRWLockExclusive(&g_restored_lock);
        g_restored_handles.insert(handle);
        ReleaseSRWLockExclusive(&g_restored_lock);
    }
    o_restore_destroy(restored, handle);
}

// Sim thread, end of a frame: the debris of the objects restored since the last gameplay frame.
void take_restored_debris(int game_state) {
    if (game_state != kGameplayState) return;
    std::unordered_set<uint32_t> handles;
    AcquireSRWLockExclusive(&g_restored_lock);
    handles.swap(g_restored_handles);
    ReleaseSRWLockExclusive(&g_restored_lock);
    if (handles.empty() || !single_player()) return;
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<int*>(w + kWorldGeneralMoverCount);
    g_restore_matches.clear();
    for (int i = 0; all && movers && i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (obj && !(obj[kObjFlags] & kObjDestroyed) &&
            handles.count(*reinterpret_cast<const uint32_t*>(obj + kGmOriginalObject)))
            g_restore_matches.push_back(obj);
    }
    const auto destroy = rfg::fn<bool(__thiscall*)(void*, uint8_t*, bool)>(r::world__destroy_object);
    for (uint8_t* obj : g_restore_matches) destroy(w, obj, false);
    g_restored_debris.fetch_add(uint32_t(g_restore_matches.size()), std::memory_order_relaxed);
    debris_offload::forget_pieces_of(handles);
    if (!g_restore_matches.empty() && g_restore_logs++ < 20)
        LOG("experiment KeepDebris: %zu objects restored; the %u pieces of debris that broke off them go with them",
            handles.size(), uint32_t(g_restore_matches.size()));
}

// havok_memory_listener::allocationFailure(size): the game prints Havok's memory statistics and then loops forever.
// With kept debris that can happen; end the game with a log line instead of a frozen window.
void(__fastcall* o_allocation_failure)(void*, void*, uint32_t);
void __fastcall d_allocation_failure(void*, void*, uint32_t size) {
    char reason[160];
    snprintf(reason, sizeof(reason),
             "Havok is out of memory (a %u-byte allocation failed; Experiment.HavokHeapMB raises the heap)", size);
    session::exit_now(reason);
}

}  // namespace

bool install_keep_debris(const Config& cfg) {
    // Always: the game's own handler is an endless loop.
    if (!hooks::create(r::havok_memory_listener__allocationFailure, &d_allocation_failure, &o_allocation_failure,
                       "havok_memory_listener::allocationFailure"))
        return false;
    session::add_frame_callback(&update_budget);
    if (cfg.exp_havok_heap_mb > int(kGameHavokHeap >> 20)) {
        g_havok_heap_bytes = uint32_t(cfg.exp_havok_heap_mb > 1024 ? 1024 : cfg.exp_havok_heap_mb) << 20;
        if (!hooks::create(r::mempool_init_alloc, &d_mempool_init_alloc, &o_mempool_init_alloc, "mempool_init_alloc") ||
            !hooks::create(r::mempool__init, &d_mempool_init, &o_mempool_init, "mempool::init"))
            return false;
    }
    if (cfg.exp_render_instances > 0) {
        g_render_instances = cfg.exp_render_instances > 65000 ? 65000 : cfg.exp_render_instances;
        if (!setup_render_instances()) return false;
    }
    if (cfg.exp_debris_pool > int(kGameDebrisPool)) {
        g_pool_capacity = uint32_t(cfg.exp_debris_pool) > kMaxDebrisPool ? kMaxDebrisPool : uint32_t(cfg.exp_debris_pool);
        if (!setup_pool()) return false;
        if (rfg::global<int32_t>(r::World + kWorldObjectCount) == 0 && rfg::global<uint32_t>(r::World + 0x68) == 0) {
            if (!hooks::create(r::world__system_init, &d_world_system_init, &o_world_system_init, "world::system_init"))
                return false;
        } else {
            LOG("experiment DebrisPool: the world is already initialized; general_mover's object lists stay at 3,072");
        }
    }
    // Kept debris and pieces in PhysX outlive what they broke off; a restored object takes them along.
    if (cfg.exp_keep_debris || cfg.exp_physx_debris) {
        if (!hooks::create(r::restore_destroy_object, &d_restore_destroy, &o_restore_destroy, "restore_destroy_object"))
            return false;
        session::add_frame_callback(&take_restored_debris);
    }
    if (!cfg.exp_keep_debris) return true;
    if (!census::install()) return false;
    census::set_fade_filter(&allow_fade);
    bool ok = hooks::create(r::rfgl_desired_values, &d_desired, &o_desired, "rfgl_desired_values");
    ok &= hooks::create(r::rfgl_maybe_delete_objects, &d_governor, &o_governor, "rfgl_maybe_delete_objects");
    ok &= hooks::create(r::rfg_dyn_maybe_delete, &d_maybe_delete, &o_maybe_delete, "rfg_dyn_maybe_delete");
    ok &= hooks::create(r::havok_collect_objects_in_sphere, &d_collect_sphere, &o_collect_sphere,
                        "havok_collect_objects_fast(sphere)");
    ok &= hooks::create(r::world__collect_sorted_list, &d_collect_sorted, &o_collect_sorted,
                        "world::collect_sorted_list");
    ok &= hooks::create(r::handle_manager_7280___allocate_handle, &d_allocate_handle, &o_allocate_handle,
                        "handle_manager<7280>::allocate_handle");
    if (!ok) return false;
    g_zone_reserve = cfg.exp_zone_reserve < 200 ? 200 : cfg.exp_zone_reserve > 5000 ? 5000 : cfg.exp_zone_reserve;
    g_on = true;
    LOG("experiment KeepDebris enabled: shattering keeps every piece, no fade-outs, no load-governor deletions "
        "(single player); debris pool %u slots; %d of each world zone's %d object handles kept free (the farthest "
        "debris is recycled below that)", g_pool_capacity ? g_pool_capacity : kGameDebrisPool, g_zone_reserve,
        kZoneHandles);
    return true;
}

KeepDebrisFrame keep_debris_take_frame() {
    KeepDebrisFrame f{};
    f.fades_blocked = g_fades_blocked.exchange(0, std::memory_order_relaxed);
    f.deletes_blocked = g_deletes_blocked.exchange(0, std::memory_order_relaxed);
    f.vaporize_refused = g_vaporize_refused.exchange(0, std::memory_order_relaxed);
    f.explosion_hits_dropped = g_explosion_hits_dropped.exchange(0, std::memory_order_relaxed);
    f.world_objects = world_objects();
    f.zone_used = g_zone_used;
    f.recycled = g_recycled.exchange(0, std::memory_order_relaxed);
    f.gate_deletes = g_gate_deletes.exchange(0, std::memory_order_relaxed);
    f.handle_failures = g_handle_failures.exchange(0, std::memory_order_relaxed);
    f.shards_failed = census::take_failed_shards();
    f.gm_pool_free = *reinterpret_cast<int32_t*>(debris_pool() + kPoolFree);
    f.rm_pool_free = *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(rfg::addr(r::rfg_moverpool)) + kPoolFree);
    // rl_class_factory_info::m_objects_max (+0x20) minus the factory's free-object list length (pool_list::num,
    // +0xC): what the load governor compares with Desired_destroyable_peak (1,600).
    f.render_max = rfg::global<int32_t>(r::rl_destroyable_model_instance__ms_class_factory_info + 0x20);
    f.render_used = f.render_max - rfg::global<int32_t>(r::rl_destroyable_model_instance__ms_object_pool + 0xC);
    return f;
}

int32_t debris_pool_capacity() { return *reinterpret_cast<int32_t*>(debris_pool() + kPoolCapacity); }

}  // namespace experiments
