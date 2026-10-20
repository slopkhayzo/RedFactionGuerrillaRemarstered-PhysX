#include "telemetry/telemetry.h"

#include <windows.h>
#include <d3d11.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/contacts.h"
#include "core/gameloop.h"
#include "core/log.h"
#include "core/session.h"
#include "experiments/havok_mt.h"
#include "experiments/keep_debris.h"
#include "experiments/render_tweaks.h"
#include "game/rfg.h"
#include "hooks/hooks.h"
#include "physx/bridge.h"
#include "physx/debris_offload.h"
#include "physx/test_scene.h"
#include "telemetry/object_census.h"
#include "telemetry/render_census.h"
#include "telemetry/render_timeline.h"

namespace telemetry {
namespace {

enum Timer : int {
    kDoFrame,        // rfg_do_frame: the whole main-loop frame on the sim thread
    kSimFrame,       // gamestate_gp_simulate_frame: gameplay simulation (pre-sim, physics, post-sim)
    kHavok,          // havok_simulate: physics including the destruction work inside the step
    kStep,           // hkpWorld::stepDeltaTime: the Havok step (the phases below + post-collide callbacks)
    kIntegrate,      //   hkpSimulation::integrate: solver and integration
    kCollideDetect,  //   hkpContinuousSimulation::collideInternal: broadphase + narrowphase + contact callbacks
    kBroadphase,     //     collideEntitiesBroadPhaseContinuous
    kCollidePost,    //   rfg_dyn_do_collide_post: Geo-Mod destruction applied in the post-collide callback
    kToi,            //   hkpContinuousSimulation::advanceTime: continuous physics (TOI events)
    kDynPre,         // rfg_dyn_do_frame_pre: pre-sim destruction and stress evaluation
    kShapeCutter,    // shape_cutter_do_frame: Geo-Mod holes
    kDebris,         // object_debris_process
    kExplosion,      // explosion_do_frame
    kAi,             // ai_system_do_frame
    kRfgl,           // rfgl_do_frame: destruction load governor
    kHandshake,      // game_handshake_do_frame: sync with the render thread
    kLimiter,        // frametime_calculate: frame timing, including the frame-cap sleep
    kHandoff,        //   game_handshake_do_middle_of_frame: the handoff to the render thread (it waits meanwhile)
    kDeferredRender, //   handshake_deferred_render: also inside the handshake
    kMtJobs,         // multithreaded Havok step (Experiment.HavokThreads): its job phase on all threads, inside step
    kTimerCount
};

constexpr const char* kTimerNames[kTimerCount] = {
    "do_frame_ms",   "sim_frame_ms",    "havok_ms",     "step_ms",      "integrate_ms", "collide_detect_ms",
    "broadphase_ms", "collide_post_ms", "toi_ms",       "dyn_pre_ms",   "shape_cutter_ms", "debris_ms",
    "explosion_ms",  "ai_ms",           "rfgl_ms",      "handshake_ms", "limiter_ms",   "handoff_ms",
    "deferred_render_ms", "mt_jobs_ms"};

struct FrameRecord {
    uint32_t frame;
    int32_t state;
    double t_ms;       // frame start, ms since plugin start
    float frame_ms;    // since the previous frame start (the real frame time seen by the sim thread)
    float timers[kTimerCount];
    uint32_t contacts_added;
    float render_ms;   // render-thread frame work that finished during this frame
    uint32_t render_frames;
    float game_dt_ms;  // the game's Frametime
    int32_t game_havok_us;
    int32_t havok_steps;
    float rfg_load;
    float recent_history;
    int32_t movers_destroyed;
    int32_t active_islands, active_bodies, inactive_islands, inactive_bodies, fixed_bodies;
    uint8_t multiplayer;
    uint32_t draw_calls;          // D3D11 draw calls issued by render frames finished during this frame
    int32_t gov_del_debris;       // load governor deletions this frame (rfgl_find_and_delete_* return values)
    int32_t gov_del_movers;
    int32_t act_type[6];          // active bodies by game object type (sampled every 15 frames)
    int32_t act_layer[6];         // active bodies by collision layer (sampled every 15 frames)
    uint32_t debris_contacts;     // contact points added between two debris bodies (layers 41-44)
    uint32_t objects_destroyed;   // world::destroy_object calls that flagged an object (telemetry/object_census.h)
    uint32_t island_watchdog_del; // of those, by the island solver-memory watchdog
    uint32_t movers_created;      // object_mover objects created (building pieces breaking off, shards)
    uint32_t cb_uploads, cb_redundant, cb_kb;  // render thread: constant-buffer uploads (telemetry/render_census.h)
    uint32_t occl_queries, occl_polls;         // render thread: GPU occlusion queries issued and polled
    uint32_t fades_blocked, deletes_blocked, shards_failed;  // experiments/keep_debris.h (0 when it is off)
    int32_t gm_pool_free, rm_pool_free;        // free general_mover (debris) / rfg_mover (chunk) pool slots
    int32_t render_used;                       // destroyable-model render instances in use
    uint32_t vaporize_refused;                 // pieces an explosion would have vaporized (keep_debris)
    uint32_t explosion_hits_dropped;           // objects in reach of an explosion beyond the 256 it can push
    int32_t world_objects, zone_used;          // objects in the world; handles in use in the fullest world zone
    uint32_t recycled, gate_deletes;           // debris recycled / deleted at shattering to keep the zone reserve
    uint32_t handle_failures;                  // objects created with their zone out of handles
    // PhysX host (physx/bridge.h, physx/test_scene.h)
    uint8_t px_state, px_gpu;                  // pxbridge::State; GPU dynamics on
    uint32_t px_sequence, px_snapshots;        // newest snapshot taken; snapshots taken this frame
    float px_age_ms, px_step_ms, px_work_ms;   // that snapshot's age when taken, its step, the host's work for it
    uint32_t px_bodies, px_awake, px_frozen;
    float px_take_us, px_copy_us, px_interp_us; // taking snapshots; copying one (test scene); interpolating all bodies
    uint32_t px_cmd_bytes, px_cmd_dropped;
    uint32_t render_warnings;                  // rl_renderer_metrics::m_warning_flags raised since the last frame
    uint32_t unbatched_peak;                   // most instances one batched pass asked to draw one by one
    uint32_t sort_peak;                        // most entries the deferred sort list was asked to hold
    uint32_t batched_peak;                     // most instances a frame asked to draw batched
    uint32_t instancing_failed;                // instanced batches the GPU buffer could not take (not drawn)
    uint32_t px_adopted, px_live, px_moved;    // debris offload: pieces taken over, simulated, instances moved
    uint32_t px_stand_ins;                     // Havok stand-ins of heavy pieces near the player
    float px_adopt_ms, px_move_ms;
};

// Active-body buckets. Object types: object_type enum (OT_MOVER 2, OT_DEBRIS 5, OT_HUMAN 0, OT_VEHICLE 3, OT_ITEM 1).
constexpr const char* kTypeColumns[6] = {"act_movers", "act_debris", "act_humans", "act_vehicles", "act_items",
                                         "act_other"};
// Collision layers: havok_layer enum (RFG_NORMAL 40/41, RFG_LITE 42, RFG_ULTRA_LITE 43, RFG_WORLD_ONLY 44, EFFECT 58).
constexpr const char* kLayerColumns[6] = {"lay_rfg_normal", "lay_rfg_lite", "lay_rfg_ultra_lite", "lay_rfg_world_only",
                                          "lay_effect_rb", "lay_other"};

// Owned by the sim thread (published values are atomics).
FrameRecord g_cur{};
int64_t g_frame_start = 0;
std::atomic<uint32_t> g_frame{0};
std::atomic<int> g_state{-1};

// Render thread to sim thread.
std::atomic<uint32_t> g_render_us{0};
std::atomic<uint32_t> g_render_count{0};
std::atomic<DWORD> g_render_tid{0};
std::atomic<uint32_t> g_draws{0};
bool g_timeline = false;  // render_timeline installed (Telemetry.RenderCensus=1)

// Latest body classification (sim thread), carried into every frame record until refreshed.
int32_t g_act_type[6], g_act_layer[6];
int g_matrix_dump_countdown = 2;  // gameplay frames until the matrix is logged (after experiments changed it)

// Output.
std::mutex g_queue_mutex;
std::vector<FrameRecord> g_queue;
std::mutex g_io_mutex;
FILE* g_csv = nullptr;
FILE* g_mem_csv = nullptr;
std::atomic<bool> g_stop{false};

// Summary over gameplay frames (state GS_GAMEPLAY), updated by whoever drains the queue under g_io_mutex.
constexpr int kHistBins = 4000;  // 0.05 ms bins up to 200 ms; the last bin collects everything slower
constexpr double kHistBinMs = 0.05;
uint32_t g_hist[kHistBins + 1];
uint64_t g_gp_frames = 0, g_all_frames = 0;
double g_gp_frame_sum = 0, g_gp_frame_max = 0;
double g_gp_timer_sum[kTimerCount];
uint64_t g_gp_contacts = 0;
int32_t g_gp_max_active_bodies = 0;
uint64_t g_gp_destroyed = 0, g_gp_island_del = 0, g_gp_gov_del = 0, g_gp_movers_created = 0;
int32_t g_gp_last_bodies = 0, g_gp_last_inactive = 0;  // dynamic bodies at the last gameplay frame
uint64_t g_gp_fades_blocked = 0, g_gp_deletes_blocked = 0, g_gp_shards_failed = 0;  // experiments/keep_debris.h
uint64_t g_gp_vaporize_refused = 0, g_gp_explosion_hits_dropped = 0;
uint64_t g_gp_recycled = 0, g_gp_gate_deletes = 0, g_gp_handle_failures = 0;  // world zone budget (keep_debris)
int32_t g_gp_max_zone_used = 0, g_gp_max_world_objects = 0;
// PhysX host, gameplay frames
uint64_t g_px_frames = 0, g_px_snapshots = 0, g_px_dropped = 0, g_px_cmd_bytes = 0;
double g_px_age_sum = 0, g_px_age_max = 0, g_px_step_sum = 0, g_px_step_max = 0, g_px_work_sum = 0;
double g_px_take_sum = 0, g_px_take_max = 0, g_px_copy_sum = 0, g_px_copy_max = 0, g_px_interp_sum = 0,
       g_px_interp_max = 0;
uint64_t g_px_copy_frames = 0;
uint32_t g_px_bodies_max = 0, g_px_awake_max = 0;
uint8_t g_px_state_last = 0, g_px_gpu = 0;
uint64_t g_px_adopted = 0;
uint32_t g_px_live_max = 0;
double g_px_move_ms_sum = 0, g_px_move_ms_max = 0, g_px_adopt_ms_max = 0;
int32_t g_gp_min_gm_free = INT32_MAX, g_gp_min_rm_free = INT32_MAX;                 // fewest free pool slots
int32_t g_gp_max_render_used = 0;   // most destroyable-model render instances in use
int32_t g_render_max = 0;           // the renderer's limit for them
std::atomic<uint64_t> g_other_cpu_100ns{0}, g_gameplay_wall_100ns{0};  // other processes' CPU during gameplay
std::atomic<uint64_t> g_host_cpu_100ns{0};  // the PhysX host's CPU during gameplay (physx/bridge.h)

// The renderer's warnings (rl_render_lib::m_metrics_p->m_warning_flags), which nothing in this build reads or clears.
// rl_batched_pass holds at most 4,096 renderables drawn one by one per pass (bit 0x200 when more were dropped) and
// 24,576 batched instances (bit 0x4): what does not fit is not drawn. Read and cleared once per frame.
constexpr uint32_t kRenderLibInstance = 0x1636910;  // rl_singleton<rl_render_lib>::ms_instance_p (RVA)
constexpr uint32_t kWarnUnbatchedFull = 0x200, kWarnBatchedFull = 0x4;
uint64_t g_gp_unbatched_full_frames = 0, g_gp_batched_full_frames = 0;
uint32_t g_render_warnings_seen = 0;
uint32_t take_render_warnings() {
    const uint8_t* lib = *static_cast<uint8_t* const*>(rfg::addr(kRenderLibInstance));
    if (!lib) return 0;
    uint8_t* metrics = *reinterpret_cast<uint8_t* const*>(lib + 0x5B0);
    if (!metrics) return 0;
    const uint32_t flags = uint32_t(InterlockedExchange(reinterpret_cast<volatile LONG*>(metrics + 8), 0));
    if (flags & ~g_render_warnings_seen) {
        LOG("renderer warning flags %08X raised for the first time this session (0x200: more than 4,096 renderables "
            "drawn one by one in a pass, the rest not drawn; 0x4: more than 24,576 batched instances)",
            flags & ~g_render_warnings_seen);
        g_render_warnings_seen |= flags;
    }
    return flags;
}

inline bool on_sim() { return gameloop::on_sim_thread(); }

// hkArray<T*> in 32-bit Havok: data pointer, size, capacity|flags.
struct HkPtrArray {
    void** data;
    int32_t size;
    int32_t capacity_and_flags;
};
constexpr uint32_t kWorldFixedIsland = 0x20, kWorldActiveIslands = 0x28, kWorldInactiveIslands = 0x34;
constexpr uint32_t kIslandEntities = 0x4C;

struct Scoped {
    Timer t;
    int64_t start = 0;
    bool active;
    explicit Scoped(Timer timer) : t(timer), active(on_sim()) {
        if (active) start = session::qpc();
    }
    ~Scoped() {
        if (active) g_cur.timers[t] += float(session::ticks_to_ms(session::qpc() - start));
    }
};

using VoidFn = void(__cdecl*)();
VoidFn o_sim_frame, o_havok, o_collide_post, o_dyn_pre, o_shape_cutter, o_debris, o_explosion, o_ai, o_rfgl;
int(__cdecl* o_handshake)(void*);
void(__cdecl* o_handoff)(void*);
void(__cdecl* o_deferred_render)(void*, void*);
void(__cdecl* o_frametime_calculate)(bool, float, float);
void(__cdecl* o_render_frame)(void*);
// __thiscall targets are hooked through __fastcall (ecx = this, edx unused).
int(__fastcall* o_step)(void*, void*, float);
int(__fastcall* o_integrate)(void*, void*, float);
void(__fastcall* o_collide_internal)(void*, void*, void*);
void(__fastcall* o_broadphase)(void*, void*, void*, int, void*, char*);
int(__fastcall* o_advance_time)(void*, void*);

template <Timer T, VoidFn* Orig>
void __cdecl timed() {
    Scoped s(T);
    (*Orig)();
}

int __cdecl d_handshake(void* renderer) {
    Scoped s(kHandshake);
    return o_handshake(renderer);
}

void __cdecl d_handoff(void* renderer) {
    Scoped s(kHandoff);
    o_handoff(renderer);
}

void __cdecl d_deferred_render(void* snapshot, void* renderer) {
    Scoped s(kDeferredRender);
    o_deferred_render(snapshot, renderer);
}

void __cdecl d_frametime_calculate(bool quantize, float quantum, float fixed_rate) {
    Scoped s(kLimiter);
    o_frametime_calculate(quantize, quantum, fixed_rate);
}

int __fastcall d_step(void* world, void* edx, float dt) {
    Scoped s(kStep);
    return o_step(world, edx, dt);
}

int __fastcall d_integrate(void* sim, void* edx, float dt) {
    Scoped s(kIntegrate);
    return o_integrate(sim, edx, dt);
}

void __fastcall d_collide_internal(void* sim, void* edx, void* step_info) {
    Scoped s(kCollideDetect);
    o_collide_internal(sim, edx, step_info);
}

void __fastcall d_broadphase(void* sim, void* edx, void* entities, int count, void* world, char* scratch) {
    Scoped s(kBroadphase);
    o_broadphase(sim, edx, entities, count, world, scratch);
}

int __fastcall d_advance_time(void* sim, void* edx) {
    Scoped s(kToi);
    return o_advance_time(sim, edx);
}

// Contact points added (core/contacts.h observer), and those between two debris bodies. A multithreaded Havok step
// reports them from its worker threads too, one at a time.
void on_contact(uint32_t layer_a, uint32_t layer_b) {
    if (!on_sim() && !experiments::havok_mt_in_jobs()) return;
    ++g_cur.contacts_added;
    if (contacts::is_debris_layer(layer_a) && contacts::is_debris_layer(layer_b)) ++g_cur.debris_contacts;
}

// Load governor deletions.
int(__cdecl* o_gov_debris)(int);
int(__cdecl* o_gov_movers)(int, bool);
int __cdecl d_gov_debris(int count) {
    const int r = o_gov_debris(count);
    if (on_sim()) g_cur.gov_del_debris += r;
    return r;
}
int __cdecl d_gov_movers(int count, bool flag) {
    const int r = o_gov_movers(count, flag);
    if (on_sim()) g_cur.gov_del_movers += r;
    return r;
}

// D3D11 draw-call counting. The game imports only D3D11CreateDevice; the Draw* methods are detoured once the device
// exists (before the render loop starts). The render thread and the scene thread (render_timeline.h) each record into
// their own deferred context, so a deferred context's methods are detoured too where they differ from the immediate
// context's. Every drawing thread counts in its own slot (a plain store, no locked increment: an atomic add per draw
// cost ~2.5% of the render thread); the render thread sums the slots once per render frame.
using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
                                        UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
CreateDeviceFn o_create_device;

#pragma warning(push)
#pragma warning(disable : 4324)  // padded to a cache line on purpose
struct alignas(64) DrawSlot {
    std::atomic<uint32_t> count{0};
    std::atomic<DWORD> tid{0};
    uint32_t seen = 0;  // render thread only: the count at the previous render frame
};
#pragma warning(pop)
constexpr int kDrawSlots = 8;
DrawSlot g_draw_slots[kDrawSlots];
std::atomic<int> g_next_draw_slot{0};
thread_local DrawSlot* t_draw_slot = nullptr;

inline void count_draw() {
    DrawSlot* s = t_draw_slot;
    if (!s) {
        int i = g_next_draw_slot.fetch_add(1, std::memory_order_relaxed);
        if (i >= kDrawSlots) i = kDrawSlots - 1;  // more drawing threads than slots: they share the last one
        s = &g_draw_slots[i];
        s->tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        t_draw_slot = s;
    }
    s->count.store(s->count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

// K: 0 the immediate context's methods, 1 a deferred context's (when they are other functions).
template <int K>
struct DrawHooks {
    static inline void(STDMETHODCALLTYPE* o_draw_indexed)(ID3D11DeviceContext*, UINT, UINT, INT);
    static inline void(STDMETHODCALLTYPE* o_draw)(ID3D11DeviceContext*, UINT, UINT);
    static inline void(STDMETHODCALLTYPE* o_draw_indexed_instanced)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
    static inline void(STDMETHODCALLTYPE* o_draw_instanced)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

    static void STDMETHODCALLTYPE draw_indexed(ID3D11DeviceContext* c, UINT a, UINT b, INT d) {
        count_draw();
        o_draw_indexed(c, a, b, d);
    }
    static void STDMETHODCALLTYPE draw(ID3D11DeviceContext* c, UINT a, UINT b) {
        count_draw();
        o_draw(c, a, b);
    }
    static void STDMETHODCALLTYPE draw_indexed_instanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) {
        count_draw();
        o_draw_indexed_instanced(c, a, b, d, e, f);
    }
    static void STDMETHODCALLTYPE draw_instanced(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
        count_draw();
        o_draw_instanced(c, a, b, d, e);
    }

    // ID3D11DeviceContext vtable: 12 DrawIndexed, 13 Draw, 20 DrawIndexedInstanced, 21 DrawInstanced. `skip`: the
    // immediate context's vtable (a slot that holds the same function is already detoured).
    static int install(void** vt, void* const* skip) {
        int hooked = 0;
        auto one = [&](int i, void* detour, void** original, const char* name) {
            if (skip && vt[i] == skip[i]) return;
            if (hooks::create_at(vt[i], detour, original, name) && hooks::enable_at(vt[i])) ++hooked;
        };
        one(12, reinterpret_cast<void*>(&draw_indexed), reinterpret_cast<void**>(&o_draw_indexed), "DrawIndexed");
        one(13, reinterpret_cast<void*>(&draw), reinterpret_cast<void**>(&o_draw), "Draw");
        one(20, reinterpret_cast<void*>(&draw_indexed_instanced), reinterpret_cast<void**>(&o_draw_indexed_instanced),
            "DrawIndexedInstanced");
        one(21, reinterpret_cast<void*>(&draw_instanced), reinterpret_cast<void**>(&o_draw_instanced), "DrawInstanced");
        return hooked;
    }
};

HRESULT WINAPI d_create_device(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, HMODULE sw, UINT flags,
                               const D3D_FEATURE_LEVEL* levels, UINT n_levels, UINT sdk, ID3D11Device** device,
                               D3D_FEATURE_LEVEL* level, ID3D11DeviceContext** context) {
    const HRESULT hr = o_create_device(adapter, type, sw, flags, levels, n_levels, sdk, device, level, context);
    static bool hooked = false;
    if (SUCCEEDED(hr) && context && *context && device && *device && !hooked) {
        hooked = true;
        void** vt = *reinterpret_cast<void***>(*context);
        const int immediate = DrawHooks<0>::install(vt, nullptr);
        int deferred = 0;
        ID3D11DeviceContext* dc = nullptr;
        if (SUCCEEDED((*device)->CreateDeferredContext(0, &dc)) && dc) {
            deferred = DrawHooks<1>::install(*reinterpret_cast<void***>(dc), vt);
            dc->Release();
        }
        LOG("telemetry: D3D11 device created, draw-call counting: %d immediate-context methods, %d other "
            "deferred-context methods detoured", immediate, deferred);
    }
    return hr;
}

// Active bodies by object type and collision layer. hkpEntity: collision filter info at +0x2C (collidable +0x10,
// broadphase handle +0x14, filter info +0x08); layer = low 16 bits. object::obj_type at +0x7E.
void classify_active_bodies() {
    memset(g_act_type, 0, sizeof(g_act_type));
    memset(g_act_layer, 0, sizeof(g_act_layer));
    auto* world = rfg::global<uint8_t*>(rfg::rva::Havok_world);
    if (!world) return;
    auto object_of = rfg::fn<uint8_t*(__cdecl*)(void*)>(rfg::rva::object_from_world_obj);
    const auto& islands = *reinterpret_cast<HkPtrArray*>(world + kWorldActiveIslands);
    for (int32_t i = 0; i < islands.size; ++i) {
        auto* island = static_cast<uint8_t*>(islands.data[i]);
        if (!island) continue;
        const auto& entities = *reinterpret_cast<HkPtrArray*>(island + kIslandEntities);
        for (int32_t e = 0; e < entities.size; ++e) {
            auto* entity = static_cast<uint8_t*>(entities.data[e]);
            if (!entity) continue;
            const uint32_t layer = *reinterpret_cast<uint32_t*>(entity + 0x2C) & 0xFFFF;
            g_act_layer[layer == 40 || layer == 41 ? 0 : layer == 42 ? 1 : layer == 43 ? 2 : layer == 44 ? 3
                        : layer == 58 ? 4 : 5]++;
            const uint8_t* obj = object_of(entity);
            const int type = obj ? int(int8_t(obj[0x7E])) : -1;
            g_act_type[type == 2 ? 0 : type == 5 ? 1 : type == 0 ? 2 : type == 3 ? 3 : type == 1 ? 4 : 5]++;
        }
    }
}

const char* layer_name(int layer) {
    for (const auto& l : rfg::havok_layer_names)
        if (l.value == layer) return l.name + 9;  // strip "HK_LAYER_"
    return "?";
}

// Logs which of the interesting layers collide with each other (havok_game_filter::m_layer_lookup_table, bit
// layerA * 128 + layerB).
void dump_collision_matrix() {
    auto* filter = rfg::global<uint8_t*>(rfg::rva::Havok_filter);
    if (!filter) return;
    const auto* table = *reinterpret_cast<uint32_t**>(filter + 0x34);
    if (!table) return;
    const int layers[] = {2, 3, 4, 5, 6, 7, 9, 36, 37, 38, 39, 40, 41, 42, 43, 44, 46, 58};
    LOG("collision matrix (1 = layers collide); columns in the same order as rows:");
    for (int a : layers) {
        char row[256];
        int n = snprintf(row, sizeof(row), "  %-24s", layer_name(a));
        for (int b : layers) {
            const uint32_t bit = uint32_t(a << 7) + uint32_t(b);
            n += snprintf(row + n, sizeof(row) - n, " %d", int((table[bit >> 5] >> (bit & 31)) & 1));
        }
        LOG("%s", row);
    }
}

void __cdecl d_render_frame(void* writer) {
    DWORD expected = 0;
    const DWORD tid = GetCurrentThreadId();
    if (g_render_tid.compare_exchange_strong(expected, tid))
        LOG("telemetry: do_game_render_frame runs on thread %lu", tid);
    if (g_timeline) render_timeline::begin_render_frame();
    const int64_t start = session::qpc();
    o_render_frame(writer);
    g_render_us.fetch_add(uint32_t(session::ticks_to_ms(session::qpc() - start) * 1000.0), std::memory_order_relaxed);
    g_render_count.fetch_add(1, std::memory_order_relaxed);
    // Draws recorded since the previous render frame, by thread (the scene thread finished its part before the
    // render thread's join, so its draws for this frame are all in).
    const DWORD scene_tid = g_timeline ? render_timeline::scene_thread_id() : 0;
    uint32_t total = 0, render_draws = 0, scene_draws = 0;
    for (DrawSlot& s : g_draw_slots) {
        const uint32_t now = s.count.load(std::memory_order_relaxed);
        const uint32_t d = now - s.seen;
        s.seen = now;
        total += d;
        const DWORD owner = s.tid.load(std::memory_order_relaxed);
        if (owner == tid) render_draws += d;
        else if (owner && owner == scene_tid) scene_draws += d;
    }
    g_draws.fetch_add(total, std::memory_order_relaxed);
    if (g_timeline) render_timeline::end_render_frame(render_draws, scene_draws);
    render_census::publish();
}

void count_bodies(const HkPtrArray& islands, int32_t& n_islands, int32_t& n_bodies) {
    n_islands = islands.size;
    n_bodies = 0;
    for (int32_t i = 0; i < islands.size; ++i) {
        auto* island = static_cast<uint8_t*>(islands.data[i]);
        if (island) n_bodies += reinterpret_cast<HkPtrArray*>(island + kIslandEntities)->size;
    }
}

void sample_havok_world(FrameRecord& r) {
    auto* world = rfg::global<uint8_t*>(rfg::rva::Havok_world);
    if (!world) return;
    count_bodies(*reinterpret_cast<HkPtrArray*>(world + kWorldActiveIslands), r.active_islands, r.active_bodies);
    count_bodies(*reinterpret_cast<HkPtrArray*>(world + kWorldInactiveIslands), r.inactive_islands, r.inactive_bodies);
    auto* fixed = *reinterpret_cast<uint8_t**>(world + kWorldFixedIsland);
    r.fixed_bodies = fixed ? reinterpret_cast<HkPtrArray*>(fixed + kIslandEntities)->size : 0;
}

void frame_begin() {
    const int64_t start = session::qpc();
    memset(&g_cur, 0, sizeof(g_cur));
    g_cur.frame = g_frame.load(std::memory_order_relaxed);
    g_cur.t_ms = session::since_start_ms();
    g_cur.frame_ms = g_frame_start ? float(session::ticks_to_ms(start - g_frame_start)) : 0.f;
    g_frame_start = start;
}

void frame_end(int state) {
    g_cur.timers[kDoFrame] = float(session::ticks_to_ms(session::qpc() - g_frame_start));
    g_cur.state = state;
    g_cur.game_dt_ms = rfg::global<float>(rfg::rva::Frametime) * 1000.f;
    g_cur.game_havok_us = rfg::global<int32_t>(rfg::rva::Gameplay_last_havok_simulate);
    g_cur.havok_steps = rfg::global<int32_t>(rfg::rva::Havok_num_steps_this_frame);
    g_cur.rfg_load = rfg::global<float>(rfg::rva::RFG_load);
    g_cur.recent_history = rfg::global<float>(rfg::rva::Recent_history_objects);
    g_cur.movers_destroyed = rfg::global<int32_t>(rfg::rva::Movers_destroyed_count);
    g_cur.multiplayer = rfg::global<uint8_t>(rfg::rva::Multiplayer);
    if (g_cur.state == 1 /* GS_GAMEPLAY */) {
        sample_havok_world(g_cur);
        // The second gameplay frame: experiments change the layer table at gameplay entry, after this callback.
        if (g_matrix_dump_countdown > 0 && --g_matrix_dump_countdown == 0) dump_collision_matrix();
        if (g_cur.frame % 15 == 0) classify_active_bodies();
        memcpy(g_cur.act_type, g_act_type, sizeof(g_act_type));
        memcpy(g_cur.act_layer, g_act_layer, sizeof(g_act_layer));
    }
    g_cur.timers[kMtJobs] = experiments::havok_mt_take_jobs_ms();
    g_cur.render_ms = float(g_render_us.exchange(0, std::memory_order_relaxed)) / 1000.f;
    g_cur.render_frames = g_render_count.exchange(0, std::memory_order_relaxed);
    g_cur.draw_calls = g_draws.exchange(0, std::memory_order_relaxed);
    const census::FrameCounts objects = census::take_frame_counts();
    g_cur.objects_destroyed = objects.destroyed;
    g_cur.island_watchdog_del = objects.island_watchdog;
    g_cur.movers_created = objects.movers_created;
    const render_census::FrameCounts rc = render_census::take();
    g_cur.cb_uploads = rc.cb_uploads;
    g_cur.cb_redundant = rc.cb_redundant;
    g_cur.cb_kb = rc.cb_kb;
    g_cur.occl_queries = rc.occl_queries;
    g_cur.occl_polls = rc.occl_polls;
    const experiments::KeepDebrisFrame kd = experiments::keep_debris_take_frame();
    g_cur.fades_blocked = kd.fades_blocked;
    g_cur.deletes_blocked = kd.deletes_blocked;
    g_cur.shards_failed = kd.shards_failed;
    g_cur.gm_pool_free = kd.gm_pool_free;
    g_cur.rm_pool_free = kd.rm_pool_free;
    g_cur.render_used = kd.render_used;
    g_cur.vaporize_refused = kd.vaporize_refused;
    g_cur.explosion_hits_dropped = kd.explosion_hits_dropped;
    g_cur.world_objects = kd.world_objects;
    g_cur.zone_used = kd.zone_used;
    g_cur.recycled = kd.recycled;
    g_cur.gate_deletes = kd.gate_deletes;
    g_cur.handle_failures = kd.handle_failures;
    const pxbridge::FrameStats px = pxbridge::take_frame();
    const pxtest::FrameStats pt = pxtest::take_frame();
    g_cur.px_state = uint8_t(px.state);
    g_cur.px_gpu = px.gpu;
    g_cur.px_sequence = px.sequence;
    g_cur.px_snapshots = px.snapshots;
    g_cur.px_age_ms = px.age_ms;
    g_cur.px_step_ms = px.step_ms;
    g_cur.px_work_ms = px.work_ms;
    g_cur.px_bodies = px.bodies;
    g_cur.px_awake = px.awake;
    g_cur.px_frozen = px.frozen;
    g_cur.render_warnings = take_render_warnings();
    g_cur.unbatched_peak = experiments::take_unbatched_peak();
    g_cur.sort_peak = experiments::take_sort_peak();
    g_cur.batched_peak = experiments::take_batched_peak();
    g_cur.instancing_failed = experiments::take_instancing_failed();
    g_cur.px_take_us = px.take_us;
    g_cur.px_copy_us = pt.copy_us;
    g_cur.px_interp_us = pt.interp_us;
    g_cur.px_cmd_bytes = px.cmd_bytes;
    g_cur.px_cmd_dropped = px.cmd_dropped;
    const debris_offload::FrameStats dof = debris_offload::take_frame();
    g_cur.px_adopted = dof.adopted;
    g_cur.px_stand_ins = dof.stand_ins;
    g_cur.px_live = dof.live;
    g_cur.px_moved = dof.moved;
    g_cur.px_adopt_ms = dof.adopt_ms;
    g_cur.px_move_ms = dof.move_ms;
    g_render_max = kd.render_max;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_queue.push_back(g_cur);
    }
    g_state.store(g_cur.state, std::memory_order_relaxed);
    g_frame.store(g_cur.frame + 1, std::memory_order_relaxed);
}

double percentile(double p) {
    if (!g_gp_frames) return 0;
    const uint64_t target = uint64_t(p * double(g_gp_frames));
    uint64_t seen = 0;
    for (int i = 0; i <= kHistBins; ++i) {
        seen += g_hist[i];
        if (seen > target) return (i + 0.5) * kHistBinMs;
    }
    return kHistBins * kHistBinMs;
}

void write_summary_locked() {
    std::wstring path = session::run_dir() + L"\\summary.json";
    FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (!f) return;
    const double mean = g_gp_frames ? g_gp_frame_sum / double(g_gp_frames) : 0;
    fprintf(f, "{\n  \"frames_total\": %llu,\n  \"gameplay_frames\": %llu,\n", g_all_frames, g_gp_frames);
    fprintf(f, "  \"sim_thread\": %lu,\n  \"render_thread\": %lu,\n", gameloop::sim_thread_id(), g_render_tid.load());
    fprintf(f,
            "  \"gameplay_frame_ms\": {\"mean\": %.3f, \"p50\": %.3f, \"p95\": %.3f, \"p99\": %.3f, \"p999\": %.3f, "
            "\"max\": %.3f},\n",
            mean, percentile(0.50), percentile(0.95), percentile(0.99), percentile(0.999), g_gp_frame_max);
    fprintf(f, "  \"gameplay_fps_mean\": %.1f,\n", mean > 0 ? 1000.0 / mean : 0.0);
    fprintf(f, "  \"gameplay_contacts_added_per_frame\": %.1f,\n",
            g_gp_frames ? double(g_gp_contacts) / double(g_gp_frames) : 0.0);
    fprintf(f, "  \"gameplay_max_active_bodies\": %d,\n", g_gp_max_active_bodies);
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        const uint64_t wall = g_gameplay_wall_100ns.load(std::memory_order_relaxed);
        const double cores = double(si.dwNumberOfProcessors);
        // Share of the machine's logical processors other processes kept busy during gameplay, in percent of one
        // core (100 = one full core) and of the whole machine.
        fprintf(f, "  \"other_processes_cpu_cores\": %.2f,\n  \"other_processes_cpu_pct_of_machine\": %.1f,\n",
                wall ? double(g_other_cpu_100ns.load(std::memory_order_relaxed)) / double(wall) : 0.0,
                wall ? 100.0 * double(g_other_cpu_100ns.load(std::memory_order_relaxed)) / (double(wall) * cores) : 0.0);
        fprintf(f, "  \"physx_host_cpu_cores\": %.2f,\n",
                wall ? double(g_host_cpu_100ns.load(std::memory_order_relaxed)) / double(wall) : 0.0);
    }
    fprintf(f, "  \"gameplay_objects_destroyed\": %llu,\n  \"gameplay_island_watchdog_deletions\": %llu,\n",
            g_gp_destroyed, g_gp_island_del);
    fprintf(f, "  \"gameplay_governor_deletions\": %llu,\n  \"gameplay_movers_created\": %llu,\n", g_gp_gov_del,
            g_gp_movers_created);
    fprintf(f, "  \"last_gameplay_frame_bodies\": %d,\n  \"last_gameplay_frame_inactive_bodies\": %d,\n",
            g_gp_last_bodies, g_gp_last_inactive);
    fprintf(f, "  \"keep_debris_fades_refused\": %llu,\n  \"keep_debris_shatter_deletions_refused\": %llu,\n",
            g_gp_fades_blocked, g_gp_deletes_blocked);
    fprintf(f, "  \"keep_debris_vaporizations_refused\": %llu,\n  \"explosion_hits_beyond_256\": %llu,\n",
            g_gp_vaporize_refused, g_gp_explosion_hits_dropped);
    fprintf(f, "  \"shards_not_created_pool_full\": %llu,\n  \"debris_pool_capacity\": %d,\n", g_gp_shards_failed,
            experiments::debris_pool_capacity());
    fprintf(f, "  \"debris_pool_min_free\": %d,\n  \"chunk_pool_min_free\": %d,\n",
            g_gp_min_gm_free == INT32_MAX ? -1 : g_gp_min_gm_free, g_gp_min_rm_free == INT32_MAX ? -1 : g_gp_min_rm_free);
    fprintf(f, "  \"render_instances_max_used\": %d,\n  \"render_instances_limit\": %d,\n", g_gp_max_render_used,
            g_render_max);
    fprintf(f, "  \"world_objects_max\": %d,\n  \"zone_handles_max_used\": %d,\n  \"zone_handle_failures\": %llu,\n",
            g_gp_max_world_objects, g_gp_max_zone_used, g_gp_handle_failures);
    fprintf(f, "  \"keep_debris_recycled\": %llu,\n  \"keep_debris_reserve_deletions\": %llu,\n", g_gp_recycled,
            g_gp_gate_deletes);
    fprintf(f, "  \"render_frames_unbatched_list_full\": %llu,\n  \"render_frames_batched_list_full\": %llu,\n",
            g_gp_unbatched_full_frames, g_gp_batched_full_frames);
    if (g_px_frames) {
        const double n = double(g_px_frames), snaps = g_px_snapshots ? double(g_px_snapshots) : 1.0;
        fprintf(f, "  \"physx\": {\"state_last\": %u, \"gpu\": %u, \"snapshots\": %llu, \"snapshot_age_ms_mean\": %.3f, "
                   "\"snapshot_age_ms_max\": %.3f, \"step_ms_mean\": %.3f, \"step_ms_max\": %.3f, \"host_work_ms_mean\": %.3f, "
                   "\"bodies_max\": %u, \"awake_max\": %u, \"take_us_mean\": %.2f, \"take_us_max\": %.2f, "
                   "\"copy_us_mean\": %.2f, \"copy_us_max\": %.2f, \"interp_us_mean\": %.2f, \"interp_us_max\": %.2f, "
                   "\"command_bytes\": %llu, \"commands_dropped\": %llu, \"debris_adopted\": %llu, \"debris_live_max\": %u, "
                   "\"debris_move_ms_mean\": %.3f, \"debris_move_ms_max\": %.3f, \"debris_adopt_ms_max\": %.3f},\n",
                unsigned(g_px_state_last), unsigned(g_px_gpu), g_px_snapshots, g_px_age_sum / snaps, g_px_age_max,
                g_px_step_sum / snaps, g_px_step_max, g_px_work_sum / snaps, g_px_bodies_max, g_px_awake_max,
                g_px_take_sum / n, g_px_take_max, g_px_copy_frames ? g_px_copy_sum / double(g_px_copy_frames) : 0.0,
                g_px_copy_max, g_px_interp_sum / n, g_px_interp_max, g_px_cmd_bytes, g_px_dropped, g_px_adopted,
                g_px_live_max, g_px_move_ms_sum / n, g_px_move_ms_max, g_px_adopt_ms_max);
    }
    fprintf(f, "  \"gameplay_timer_mean_ms\": {");
    for (int t = 0; t < kTimerCount; ++t)
        fprintf(f, "%s\"%s\": %.4f", t ? ", " : "", kTimerNames[t],
                g_gp_frames ? g_gp_timer_sum[t] / double(g_gp_frames) : 0.0);
    fprintf(f, "}\n}\n");
    fclose(f);
}

void write_records_locked(const std::vector<FrameRecord>& records) {
    for (const FrameRecord& r : records) {
        ++g_all_frames;
        if (r.state == 1 /* GS_GAMEPLAY */ && r.frame_ms > 0) {
            ++g_gp_frames;
            g_gp_frame_sum += r.frame_ms;
            g_gp_frame_max = std::max(g_gp_frame_max, double(r.frame_ms));
            g_hist[std::min(int(r.frame_ms / kHistBinMs), kHistBins)]++;
            for (int t = 0; t < kTimerCount; ++t) g_gp_timer_sum[t] += r.timers[t];
            g_gp_contacts += r.contacts_added;
            g_gp_max_active_bodies = std::max(g_gp_max_active_bodies, r.active_bodies);
            g_gp_destroyed += r.objects_destroyed;
            g_gp_island_del += r.island_watchdog_del;
            g_gp_movers_created += r.movers_created;
            g_gp_gov_del += uint64_t(r.gov_del_debris) + uint64_t(r.gov_del_movers);
            g_gp_last_bodies = r.active_bodies + r.inactive_bodies;
            g_gp_last_inactive = r.inactive_bodies;
            g_gp_fades_blocked += r.fades_blocked;
            g_gp_deletes_blocked += r.deletes_blocked;
            g_gp_shards_failed += r.shards_failed;
            g_gp_vaporize_refused += r.vaporize_refused;
            g_gp_explosion_hits_dropped += r.explosion_hits_dropped;
            g_gp_min_gm_free = std::min(g_gp_min_gm_free, r.gm_pool_free);
            g_gp_min_rm_free = std::min(g_gp_min_rm_free, r.rm_pool_free);
            g_gp_max_render_used = std::max(g_gp_max_render_used, r.render_used);
            g_gp_recycled += r.recycled;
            g_gp_gate_deletes += r.gate_deletes;
            g_gp_handle_failures += r.handle_failures;
            g_gp_max_zone_used = std::max(g_gp_max_zone_used, r.zone_used);
            g_gp_max_world_objects = std::max(g_gp_max_world_objects, r.world_objects);
            if (r.render_warnings & kWarnUnbatchedFull) ++g_gp_unbatched_full_frames;
            if (r.render_warnings & kWarnBatchedFull) ++g_gp_batched_full_frames;
            if (r.px_state) {
                ++g_px_frames;
                g_px_state_last = r.px_state;
                g_px_gpu = r.px_gpu;
                g_px_dropped += r.px_cmd_dropped;
                g_px_cmd_bytes += r.px_cmd_bytes;
                g_px_take_sum += r.px_take_us;
                g_px_take_max = std::max(g_px_take_max, double(r.px_take_us));
                g_px_interp_sum += r.px_interp_us;
                g_px_interp_max = std::max(g_px_interp_max, double(r.px_interp_us));
                if (r.px_copy_us > 0) {
                    ++g_px_copy_frames;
                    g_px_copy_sum += r.px_copy_us;
                    g_px_copy_max = std::max(g_px_copy_max, double(r.px_copy_us));
                }
                if (r.px_snapshots) {
                    g_px_snapshots += r.px_snapshots;
                    g_px_age_sum += r.px_age_ms;
                    g_px_age_max = std::max(g_px_age_max, double(r.px_age_ms));
                    g_px_step_sum += r.px_step_ms;
                    g_px_step_max = std::max(g_px_step_max, double(r.px_step_ms));
                    g_px_work_sum += r.px_work_ms;
                }
                g_px_bodies_max = std::max(g_px_bodies_max, r.px_bodies);
                g_px_adopted += r.px_adopted;
                g_px_live_max = std::max(g_px_live_max, r.px_live);
                g_px_move_ms_sum += r.px_move_ms;
                g_px_move_ms_max = std::max(g_px_move_ms_max, double(r.px_move_ms));
                g_px_adopt_ms_max = std::max(g_px_adopt_ms_max, double(r.px_adopt_ms));
                g_px_awake_max = std::max(g_px_awake_max, r.px_awake);
            }
        }
        if (!g_csv) continue;
        fprintf(g_csv, "%u,%d,%.3f,%.3f", r.frame, r.state, r.t_ms, r.frame_ms);
        for (float v : r.timers) fprintf(g_csv, ",%.3f", v);
        fprintf(g_csv, ",%u,%.3f,%u,%.3f,%d,%d,%.3f,%.1f,%d,%d,%d,%d,%d,%d,%u,%u,%d,%d", r.contacts_added, r.render_ms,
                r.render_frames, r.game_dt_ms, r.game_havok_us, r.havok_steps, r.rfg_load, r.recent_history,
                r.movers_destroyed, r.active_islands, r.active_bodies, r.inactive_islands, r.inactive_bodies,
                r.fixed_bodies, r.multiplayer, r.draw_calls, r.gov_del_debris, r.gov_del_movers);
        for (int32_t v : r.act_type) fprintf(g_csv, ",%d", v);
        for (int32_t v : r.act_layer) fprintf(g_csv, ",%d", v);
        fprintf(g_csv, ",%u,%u,%u,%u", r.debris_contacts, r.objects_destroyed, r.island_watchdog_del, r.movers_created);
        fprintf(g_csv, ",%u,%u,%u,%u,%u", r.cb_uploads, r.cb_redundant, r.cb_kb, r.occl_queries, r.occl_polls);
        fprintf(g_csv, ",%u,%u,%u,%d,%d,%d,%u,%u", r.fades_blocked, r.deletes_blocked, r.shards_failed,
                r.gm_pool_free, r.rm_pool_free, r.render_used, r.vaporize_refused, r.explosion_hits_dropped);
        fprintf(g_csv, ",%d,%d,%u,%u,%u", r.world_objects, r.zone_used, r.recycled, r.gate_deletes, r.handle_failures);
        fprintf(g_csv, ",%u,%u,%u,%u,%.3f,%.3f,%.3f,%u,%u,%.2f,%.2f,%.2f,%u,%u", unsigned(r.px_state), unsigned(r.px_gpu),
                r.px_sequence, r.px_snapshots, r.px_age_ms, r.px_step_ms, r.px_work_ms, r.px_bodies, r.px_awake,
                r.px_take_us, r.px_copy_us, r.px_interp_us, r.px_cmd_bytes, r.px_cmd_dropped);
        fprintf(g_csv, ",%u,%u,%u,%.3f,%.3f,%u,%X,%u,%u,%u,%u,%u", r.px_adopted, r.px_live, r.px_moved,
                r.px_adopt_ms, r.px_move_ms, r.px_frozen, r.render_warnings, r.unbatched_peak, r.sort_peak,
                r.px_stand_ins, r.batched_peak, r.instancing_failed);
        fputc('\n', g_csv);
    }
}

void drain(bool at_exit) {
    std::vector<FrameRecord> batch;
    if (at_exit) {
        if (!g_queue_mutex.try_lock()) return;
        batch.swap(g_queue);
        g_queue_mutex.unlock();
        if (!g_io_mutex.try_lock()) return;
    } else {
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            batch.swap(g_queue);
        }
        g_io_mutex.lock();
    }
    write_records_locked(batch);
    if (g_csv) fflush(g_csv);
    g_io_mutex.unlock();
}

void sample_memory() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uint64_t free_total = 0, largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_FREE) {
            free_total += mbi.RegionSize;
            largest = std::max<uint64_t>(largest, mbi.RegionSize);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    std::lock_guard<std::mutex> lock(g_io_mutex);
    if (g_mem_csv) {
        fprintf(g_mem_csv, "%.1f,%u,%.1f,%.1f,%.1f,%.1f\n", session::since_start_ms(), g_frame.load(),
                double(free_total) / 1048576.0, double(largest) / 1048576.0, double(pmc.PrivateUsage) / 1048576.0,
                double(pmc.WorkingSetSize) / 1048576.0);
        fflush(g_mem_csv);
    }
}

// CPU used by other processes while gameplay runs (a benchmark run is only comparable when this stays low):
// system busy time (kernel + user - idle, over all cores) minus this process's own CPU time.
uint64_t ft64(const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; }
struct CpuSample {
    uint64_t sys_busy = 0, proc = 0, host = 0, wall = 0;
};
CpuSample cpu_sample() {
    CpuSample s;
    FILETIME idle, kernel, user, created, exited, pk, pu, now;
    if (GetSystemTimes(&idle, &kernel, &user)) s.sys_busy = ft64(kernel) + ft64(user) - ft64(idle);
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &pk, &pu)) s.proc = ft64(pk) + ft64(pu);
    s.host = pxbridge::host_cpu_100ns();  // the PhysX host is ours: counted on its own, not as another process
    GetSystemTimeAsFileTime(&now);
    s.wall = ft64(now);
    return s;
}
DWORD WINAPI writer_main(LPVOID) {
    SetThreadDescription(GetCurrentThread(), L"rfgphys telemetry");
    double last_mem = -1e9, last_summary = 0;
    CpuSample last_cpu = cpu_sample();
    while (!g_stop.load()) {
        Sleep(250);
        const CpuSample cpu = cpu_sample();
        if (g_state.load(std::memory_order_relaxed) == 1 /* GS_GAMEPLAY */ && cpu.sys_busy >= last_cpu.sys_busy &&
            cpu.proc >= last_cpu.proc) {
            const uint64_t sys = cpu.sys_busy - last_cpu.sys_busy, own = cpu.proc - last_cpu.proc;
            const uint64_t host = cpu.host >= last_cpu.host ? cpu.host - last_cpu.host : 0;
            g_other_cpu_100ns.fetch_add(sys > own + host ? sys - own - host : 0, std::memory_order_relaxed);
            g_host_cpu_100ns.fetch_add(host, std::memory_order_relaxed);
            g_gameplay_wall_100ns.fetch_add(cpu.wall - last_cpu.wall, std::memory_order_relaxed);
        }
        last_cpu = cpu;
        drain(false);
        const double now = session::since_start_ms();
        if (now - last_mem >= 1000) {
            sample_memory();
            last_mem = now;
        }
        if (now - last_summary >= 10000) {
            std::lock_guard<std::mutex> lock(g_io_mutex);
            write_summary_locked();
            last_summary = now;
        }
    }
    return 0;
}

void flush(bool at_exit) {
    g_stop = true;
    drain(at_exit);
    if (at_exit) {
        if (g_io_mutex.try_lock()) {
            write_summary_locked();
            g_io_mutex.unlock();
        }
    } else {
        std::lock_guard<std::mutex> lock(g_io_mutex);
        write_summary_locked();
    }
}

FILE* open_csv(const wchar_t* name, const char* header) {
    std::wstring path = session::run_dir() + L"\\" + name;
    FILE* f = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (f) fputs(header, f);
    return f;
}

}  // namespace

uint32_t current_frame() { return g_frame.load(std::memory_order_relaxed); }
int current_state() { return g_state.load(std::memory_order_relaxed); }
unsigned long sim_thread_id() { return gameloop::sim_thread_id(); }
unsigned long render_thread_id() { return g_render_tid.load(std::memory_order_relaxed); }

bool install(const Config& cfg) {
    std::string header = "frame,state,t_ms,frame_ms";
    for (const char* n : kTimerNames) header += std::string(",") + n;
    header += ",contacts_added,render_ms,render_frames,game_dt_ms,game_havok_us,havok_steps,rfg_load,recent_history,"
              "movers_destroyed,active_islands,active_bodies,inactive_islands,inactive_bodies,fixed_bodies,"
              "multiplayer,draw_calls,gov_del_debris,gov_del_movers";
    for (const char* n : kTypeColumns) header += std::string(",") + n;
    for (const char* n : kLayerColumns) header += std::string(",") + n;
    header += ",debris_contacts,objects_destroyed,island_watchdog_del,movers_created";
    header += ",cb_uploads,cb_redundant,cb_kb,occl_queries,occl_polls";
    header += ",fades_blocked,deletes_blocked,shards_failed,gm_pool_free,rm_pool_free,render_used,vaporize_refused,"
              "explosion_hits_dropped";
    header += ",world_objects,zone_used,recycled,reserve_deletes,handle_failures";
    header += ",px_state,px_gpu,px_seq,px_snapshots,px_age_ms,px_step_ms,px_work_ms,px_bodies,px_awake,px_take_us,"
              "px_copy_us,px_interp_us,px_cmd_bytes,px_cmd_dropped";
    header += ",px_adopted,px_live,px_moved,px_adopt_ms,px_move_ms,px_frozen,render_warnings,unbatched_peak,sort_peak,px_stand_ins,batched_peak,instancing_failed";
    header += "\n";
    g_csv = open_csv(L"telemetry.csv", header.c_str());
    g_mem_csv = open_csv(L"memory.csv", "t_ms,frame,va_free_mb,va_largest_free_mb,private_mb,working_set_mb\n");
    g_queue.reserve(4096);

    namespace r = rfg::rva;
    bool ok = true;
    gameloop::add_begin(&frame_begin);
    gameloop::add_end(&frame_end);
    ok &= hooks::create(r::gamestate_gp_simulate_frame, &timed<kSimFrame, &o_sim_frame>, &o_sim_frame,
                        "gamestate_gp_simulate_frame");
    ok &= hooks::create(r::havok_simulate, &timed<kHavok, &o_havok>, &o_havok, "havok_simulate");
    ok &= hooks::create(r::hkpWorld__stepDeltaTime, &d_step, &o_step, "hkpWorld::stepDeltaTime");
    ok &= hooks::create(r::hkpSimulation__integrate, &d_integrate, &o_integrate, "hkpSimulation::integrate");
    ok &= hooks::create(r::hkpContinuousSimulation__collideInternal, &d_collide_internal, &o_collide_internal,
                        "hkpContinuousSimulation::collideInternal");
    ok &= hooks::create(r::hkpContinuousSimulation__collideEntitiesBroadPhaseContinuous, &d_broadphase, &o_broadphase,
                        "hkpContinuousSimulation::collideEntitiesBroadPhaseContinuous");
    ok &= hooks::create(r::hkpContinuousSimulation__advanceTime, &d_advance_time, &o_advance_time,
                        "hkpContinuousSimulation::advanceTime");
    ok &= hooks::create(r::rfg_dyn_do_collide_post, &timed<kCollidePost, &o_collide_post>, &o_collide_post,
                        "rfg_dyn_do_collide_post");
    ok &= hooks::create(r::rfg_dyn_do_frame_pre, &timed<kDynPre, &o_dyn_pre>, &o_dyn_pre, "rfg_dyn_do_frame_pre");
    ok &= hooks::create(r::shape_cutter_do_frame, &timed<kShapeCutter, &o_shape_cutter>, &o_shape_cutter,
                        "shape_cutter_do_frame");
    ok &= hooks::create(r::object_debris_process, &timed<kDebris, &o_debris>, &o_debris, "object_debris_process");
    ok &= hooks::create(r::explosion_do_frame, &timed<kExplosion, &o_explosion>, &o_explosion, "explosion_do_frame");
    ok &= hooks::create(r::ai_system_do_frame, &timed<kAi, &o_ai>, &o_ai, "ai_system_do_frame");
    ok &= hooks::create(r::rfgl_do_frame, &timed<kRfgl, &o_rfgl>, &o_rfgl, "rfgl_do_frame");
    ok &= hooks::create(r::game_handshake_do_frame, &d_handshake, &o_handshake, "game_handshake_do_frame");
    ok &= hooks::create(r::game_handshake_do_middle_of_frame, &d_handoff, &o_handoff, "game_handshake_do_middle_of_frame");
    ok &= hooks::create(r::handshake_deferred_render, &d_deferred_render, &o_deferred_render, "handshake_deferred_render");
    ok &= hooks::create(r::frametime_calculate, &d_frametime_calculate, &o_frametime_calculate, "frametime_calculate");
    ok &= hooks::create(r::do_game_render_frame, &d_render_frame, &o_render_frame, "do_game_render_frame");
    if (cfg.contact_counters) {
        ok &= contacts::install();
        contacts::add_observer(&on_contact);
    }
    ok &= hooks::create(r::rfgl_find_and_delete_debris_object, &d_gov_debris, &o_gov_debris,
                        "rfgl_find_and_delete_debris_object");
    ok &= hooks::create(r::rfgl_find_and_delete_object_mover, &d_gov_movers, &o_gov_movers,
                        "rfgl_find_and_delete_object_mover");
    ok &= census::install();
    if (cfg.render_census) {
        ok &= render_census::install(cfg.cb_redundancy);
        g_timeline = render_timeline::install();
        ok &= g_timeline;
    }
    o_create_device = reinterpret_cast<CreateDeviceFn>(
        hooks::patch_import("d3d11.dll", "D3D11CreateDevice", reinterpret_cast<void*>(&d_create_device)));
    ok &= o_create_device != nullptr;

    session::add_flush_handler(&flush);
    HANDLE writer = CreateThread(nullptr, 256 * 1024, writer_main, nullptr, 0, nullptr);
    if (writer) CloseHandle(writer);
    LOG("telemetry: %s, writing to %ls", ok ? "all hooks created" : "SOME HOOKS FAILED", session::run_dir().c_str());
    return ok;
}

}  // namespace telemetry
