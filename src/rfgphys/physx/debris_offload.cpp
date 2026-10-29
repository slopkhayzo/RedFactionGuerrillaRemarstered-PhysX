#include "physx/debris_offload.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/harness.h"
#include "hooks/hooks.h"
#include "physx/bridge.h"
#include "physx/havok_shapes.h"
#include "physx/render_routing.h"
#include "physx/world_mirror.h"

namespace debris_offload {
namespace {

namespace hs = havok_shapes;
namespace ipc = rfgphysx;
namespace r = rfg::rva;

constexpr int kGameplay = 1;               // GS_GAMEPLAY
constexpr uint32_t kAdoptPerFrame = 400;   // pieces taken over per frame at most (the rest the next frame)
constexpr float kFloorY = -400.0f;         // pieces below this are given up
uint64_t g_fell = 0;                        // pieces given up there (they fell through the world)
// Where pieces go down: the first ones to sink below kLowY (under the ground of the maps without a chasm) are logged
// with their positions over that step, which shows where the world has a hole or an edge (07 B5a).
constexpr float kLowY = -20.0f;
constexpr uint32_t kLowLogged = 16;
uint32_t g_low_slots[kLowLogged], g_low_logged = 0;

// general_mover / object fields (01 §10.10, research/docs/06 §13).
constexpr uint32_t kObjFlags = 0x54, kObjHavokHandle = 0x4C, kObjAttach = 0x40;  // child_constraint, host_constraint, attach_info
constexpr uint8_t kObjDestroyed = 0x10;
constexpr uint32_t kObjHandle = 0x6C, kObjType = 0x7E;
// object::srid: the stream2 container the object's data comes from; a mover's is its building's chunk (mesh,
// materials, destruction data: what the render instance draws). The object holds a reference on it while byte +0x55
// of its flags has 8 (requested) or 0x10 (streamed in); world::free_object gives it back.
constexpr uint32_t kObjSrid = 0x98, kObjStreamFlags = 0x55;
constexpr uint8_t kObjStreamHeld = 0x18;
constexpr uint32_t kMoverFlags = 0xE0, kMoverLoadBalanced = 4, kMoverFading = 0x800;
constexpr uint32_t kMoverChunkUid = 0xA4;  // object_mover::chunk_uid: (c_chunk_base::internal_uid << 16) | pool index
constexpr uint32_t kNoChunk = 0xFFFFFFFFu;
constexpr uint32_t kGmRenderInstance = 0x118;  // general_mover::rdd.model_instance_p
constexpr uint32_t kGmOriginalObject = 0x128;  // general_mover::original_object: the object it broke off
constexpr uint32_t kNoObject = 0xFFFFFFFFu;
// world_zone: state (2: loaded), its obj_zone, its name.
constexpr uint32_t kZoneState = 0x58, kZoneObj = 0xD4, kZoneName = 0x18;
constexpr int32_t kZoneLoaded = 2;
// World lists: all_objects.elt, subtype_objects[OT_SUB_MOVER_GENERAL] (elt, num).
constexpr uint32_t kWorldAllObjects = 0x64, kWorldGeneralMovers = 0x3C4, kWorldGeneralMoverCount = 0x3CC;
// hkpRigidBody: motion at +0xE0 (type +8, m_inertiaAndMassInv +0xC0, m_linearVelocity +0xD0, m_angularVelocity +0xE0).
constexpr uint32_t kBodyMotionType = 0xE8, kBodyInvMass = 0x1AC, kBodyLinVel = 0x1B0, kBodyAngVel = 0x1C0;
// rl_scene_entity: rotation rows (rvec, uvec, fvec) at +0x08, position at +0x2C, flags at +0x40, partition at +0x9C.
constexpr uint32_t kInstRot = 0x08, kInstPos = 0x2C, kInstFlags = 0x40, kInstPartition = 0x9C;
constexpr uint32_t kPlayerShape = 0xFFFF0001u;

bool g_on = false;
// Experiment.PhysXDebrisBatch: rl_scene_renderer::render draws an instance whose flags (+0x40) have bit 7 through
// the batched pass (vtable slot 7, add_to_batch: sub-instances instanced by material) and every other one on its own
// (slot 11, add_to_deferred, then one draw per pass). A destroyable instance starts batchable, but once its compiled
// mesh (its sub-instances merged, rl_start_recompile_mesh_instance) arrives, set_compiled_cache_data clears the bit:
// every piece then costs a draw per pass. With this the pieces' bit is set again every frame, in the render task.
bool g_batch = false;
constexpr uint16_t kInstBatchable = 0x80;
uint32_t g_batch_restored = 0;  // bits set again (they had been cleared), for the log
// Experiment.PhysXDebrisDropMeshes: after a split the game builds every destroyable instance a compiled mesh, its
// sub-instances merged (rl_start_recompile_mesh_instance; rl_mesh_recompile_data: CPU copies of the vertices and
// indices plus GPU buffers, inst +0x158). Drawn batched, a piece never uses it. The render task gives the pieces'
// meshes back (rl_delete_mesh_recompile_data, the game's own deferred delete; a few hundred a frame, within its 2,048
// queue slots), and no new ones are built for them. The set of the pieces' instances is read by that hook.
constexpr uint32_t kInstRecompileData = 0x158;
constexpr uint32_t kDropPerFrame = 400;
bool g_drop_meshes = false;
uint32_t g_meshes_dropped = 0, g_recompiles_skipped = 0;
void(__cdecl* o_start_recompile)(const void*, bool) = nullptr;  // rl_start_recompile_mesh_instance, hooked
SRWLOCK g_piece_set_lock = SRWLOCK_INIT;
std::unordered_set<const void*> g_piece_set;
// The pieces' instances that got a compiled mesh (set_compiled_cache_data, which also clears the batchable bit) or
// were just taken over: the render task gives the mesh back and sets the bit again, for these only (it used to look at
// every piece's instance every frame). Any thread queues.
SRWLOCK g_fix_lock = SRWLOCK_INIT;
std::vector<uint8_t*> g_fix_queue;
uint32_t g_slots = 0, g_player_slot = 0;
constexpr uint32_t kNoStandIn = 0xFFFFFFFFu;
constexpr uint32_t kNoPile = 0xFFFFFFFFu;
struct Piece {
    // What the render task looks at each frame for the pieces that move or changed (move_piece) comes first.
    uint8_t* inst = nullptr;  // the render instance taken over; null: free slot. In a pile: the pile's instance
    uint32_t pile = kNoPile;  // the pile drawing it (Experiment.PhysXDebrisPiles)
    double frozen_since_ms = 0;  // since when it has been still in PhysX: frozen, or asleep (0: it moves)
    uint8_t settle = 0;       // 1 while the last applied pose was an interpolated (moving) one
    uint8_t fresh = 0;        // 1 from adoption until the render task first sees the piece
    uint8_t candidate = 0;    // in g_pile_candidates (still, not in a pile)
    float own_sub[12] = {};   // in a pile: its sub-instance's transform in its own instance, given back when it leaves
    uint16_t sub_index = 0;   // in a pile: its sub-instance's index in its building
    uint16_t submesh = 0;     // in a pile: its sub-instance's submesh
    uint8_t disable_deferred = 0;  // in a pile: its sub-instance's flag of that name
    float piled_pos[3] = {}, piled_q[4] = {};  // in a pile: where PhysX had it when it joined (it is drawn there)
    uint8_t heavy = 0;        // solid to the player (Experiment.PhysXSolidDebris)
    const uint8_t* shape = nullptr;  // its general_mover's hkpConvexVerticesShape, referenced: its stand-ins' shape
    uint32_t stand_in = kNoStandIn;  // Havok handle of its stand-in
    uint32_t wanted = 0;      // the stand-in pass that last wanted one
    float want_key = 0;       // that pass: how much (lower first)
    float si_pos[3] = {}, si_q[4] = {};  // where its stand-in was last put
    float mass = 0;
    uint8_t physical_material = 0, effect_material = 0;  // its Havok body's properties 3 and 4, for its stand-in
    float box_c[3] = {}, box_size[3] = {};  // bounds of its parts in its own frame: centre, full extents
    float radius = 0;         // of those bounds around its origin
    uint32_t srid = 0;        // the chunk container its general_mover held a reference on (0: none): see g_srid_pieces
    uint32_t chunk_uid = kNoChunk;  // its building's chunk
    uint32_t original = kNoObject;  // the object it broke off (its general_mover's original_object)
};
std::vector<Piece> g_pieces;
std::vector<uint32_t> g_free;
// The host state byte each slot had when the render task last acted on it (move_pieces): a slot is looked at again
// only while its piece moves or when its state changed (a frozen pile of 30,000 pieces costs a 64 KB compare).
std::vector<uint8_t> g_seen_state;
// Still pieces not in a pile (Experiment.PhysXDebrisPiles): what piles_frame looks over for new piles.
std::unordered_set<uint32_t> g_pile_candidates;
// Where each piece is, for the stand-ins' search (update_stand_ins): its pose in the newest snapshot and its radius,
// 16 bytes a slot instead of its record and its pose (r < 0: no live piece). move_piece writes it for the pieces that
// move or changed; give_up clears it.
struct Spatial {
    float x, y, z, r;
};
std::vector<Spatial> g_spatial;
std::unordered_map<uint64_t, uint32_t> g_shape_ids;
uint32_t g_next_shape = 1;
bool g_player_ready = false;
FrameStats g_stats{};
uint64_t g_total_adopted = 0;
uint64_t g_adopted_faded = 0;  // went over faded by distance (their instances keep the opacity)
// Masses of the pieces taken over, by powers of two (bucket b: [2^(b-1), 2^b) kg; 0: under 1 kg), and how the game
// decides between pushing a body out of the player's way and standing on it (character_controller_should_push_away:
// lighter than Character_collision_push_away_mass is pushed).
uint32_t g_mass_hist[20] = {};
bool g_tweaks_logged = false;
float g_largest_radius = 0;                             // of the pieces taken over in this level (at most 50 m)
bool g_diag = false;                                    // Telemetry.Enabled: the censuses below
uint32_t g_adopted_top_types[hs::kTypeCount + 1] = {};  // the top shape types of the pieces taken over

int mass_bucket(float mass) {
    int b = 0;
    for (float m = 1.0f; mass >= m && b < 19; m *= 2.0f) ++b;
    return b;
}

float g_radius_min = 1e30f, g_radius_max = 0;  // the convex radii of the pieces' parts taken over
double g_radius_sum = 0;
uint32_t g_radius_n = 0;

void log_masses(const char* when) {
    char text[512];
    int n = 0;
    for (int b = 0; b < 20 && n < int(sizeof(text)) - 32; ++b)
        if (g_mass_hist[b])
            n += snprintf(text + n, sizeof(text) - n, " <%u kg: %u", 1u << b, g_mass_hist[b]);
    LOG("debris offload: masses of the pieces taken over (%s):%s", when, n ? text : " none");
    if (g_radius_n)
        LOG("debris offload: convex radii of their parts: %.4f to %.4f m, %.4f mean (%u parts)", double(g_radius_min),
            double(g_radius_max), g_radius_sum / g_radius_n, g_radius_n);
    g_radius_min = 1e30f, g_radius_max = 0, g_radius_sum = 0, g_radius_n = 0;
}

// ---- shapes ----

struct Parts : hs::Sink {
    std::vector<std::vector<ipc::Vec3>> parts;
    std::vector<float> radii;  // each part's convex radius (the host grows its hull by it)
    bool has_triangles = false;
    void triangle(const float*, const float*, const float*) override { has_triangles = true; }
    void convex(const float (*v)[3], int n, float radius, uint32_t) override {
        if (n < 3) return;
        std::vector<ipc::Vec3> p(size_t(std::min(n, 255)));
        for (size_t i = 0; i < p.size(); ++i) p[i] = {v[i][0], v[i][1], v[i][2]};
        parts.push_back(std::move(p));
        radii.push_back(radius);
    }
    void height_field(const uint8_t*, const hs::Transform&) override { has_triangles = true; }
    void unsupported(uint8_t) override { has_triangles = true; }
};

bool walk_guarded(const uint8_t* shape, Parts& p) {
    __try {
        return hs::walk(shape, hs::Transform::identity(), p);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool send_part(uint32_t shape, uint32_t part, const std::vector<ipc::Vec3>& v, uint32_t flags = 0,
               float radius = 0.0f) {
    std::vector<uint8_t> buf((sizeof(ipc::ShapePart) + v.size() * sizeof(ipc::Vec3) + 7) & ~size_t(7));
    auto* c = reinterpret_cast<ipc::ShapePart*>(buf.data());
    c->h = {uint16_t(ipc::Cmd::ShapePart), 0, uint32_t(buf.size())};
    c->shape = shape;
    c->part = part;
    c->vertex_count = uint32_t(v.size());
    c->flags = flags;
    c->radius = radius;
    memcpy(c + 1, v.data(), v.size() * sizeof(ipc::Vec3));
    return pxbridge::send_record(buf.data(), uint32_t(buf.size()));
}

// The same parts (to the millimetre) with the same flags share one host shape: shards of one building repeat a lot.
uint32_t intern_shape(const Parts& p, uint32_t flags) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(flags);
    for (size_t i = 0; i < p.parts.size(); ++i) {
        const auto& part = p.parts[i];
        mix(part.size());
        mix(uint64_t(int64_t(std::lround(double(i < p.radii.size() ? p.radii[i] : 0.0f) * 10000.0))));
        for (const ipc::Vec3& v : part) {
            mix(uint64_t(int64_t(std::lround(double(v.x) * 1000.0))));
            mix(uint64_t(int64_t(std::lround(double(v.y) * 1000.0))));
            mix(uint64_t(int64_t(std::lround(double(v.z) * 1000.0))));
        }
    }
    auto it = g_shape_ids.find(h);
    if (it != g_shape_ids.end()) return it->second;
    const uint32_t id = g_next_shape++;
    for (size_t i = 0; i < p.parts.size(); ++i)
        if (!send_part(id, uint32_t(i), p.parts[i], flags, i < p.radii.size() ? p.radii[i] : 0.0f))
            return 0;  // ring full: try again later
    g_shape_ids.emplace(h, id);
    return id;
}

// ---- render instances ----

// As object_mover::update_render: mark the transform dirty, update the scene partition, tell the renderer.
void refresh_instance(uint8_t* inst) {
    uint16_t& flags = *reinterpret_cast<uint16_t*>(inst + kInstFlags);
    flags = uint16_t((flags & 0xFFEF) | 1);
    if (rfg::fn<bool(__cdecl*)()>(r::RL_auto_partition_update)()) {
        uint8_t* partition = *reinterpret_cast<uint8_t**>(inst + kInstPartition);
        if (partition && !(flags & 2)) {
            using Update = void(__thiscall*)(uint8_t*, uint8_t*);
            (*reinterpret_cast<Update* const*>(partition))[2](partition, inst);
        }
    }
    rfg::fn<void(__cdecl*)(uint8_t*)>(r::game_render_scene_update)(inst);
}

void rotation_of(const float q[4], float rot[9]) {  // rvec, uvec, fvec: the rotated x, y and z axes
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    rot[0] = 1 - 2 * (y * y + z * z), rot[1] = 2 * (x * y + z * w), rot[2] = 2 * (x * z - y * w);
    rot[3] = 2 * (x * y - z * w), rot[4] = 1 - 2 * (x * x + z * z), rot[5] = 2 * (y * z + x * w);
    rot[6] = 2 * (x * z + y * w), rot[7] = 2 * (y * z - x * w), rot[8] = 1 - 2 * (x * x + y * y);
}

void move_instance(uint8_t* inst, const float pos[3], const float q[4]) {
    rotation_of(q, reinterpret_cast<float*>(inst + kInstRot));
    memcpy(inst + kInstPos, pos, 12);
    refresh_instance(inst);
}

// As move_instance, unless the instance is there already: within 0.05 mm, its axes within 5e-5 (0.003 degrees). PhysX
// keeps a piece awake for a while after it stops, and the partition update is most of a move's cost; after a blast,
// moving 2,000-3,000 awake pieces took up to 4 ms of the frame. Compared with what the instance shows, so a slow
// piece still gets there (in steps of 0.05 mm).
constexpr float kMoveEpsilon = 5e-5f;
uint64_t g_moves_skipped = 0;
bool move_instance_if_moved(uint8_t* inst, const float pos[3], const float q[4]) {
    float rot[9];
    rotation_of(q, rot);
    const float* cur_rot = reinterpret_cast<const float*>(inst + kInstRot);
    const float* cur_pos = reinterpret_cast<const float*>(inst + kInstPos);
    bool there = true;
    for (int k = 0; k < 3 && there; ++k) there = std::fabs(cur_pos[k] - pos[k]) < kMoveEpsilon;
    for (int k = 0; k < 9 && there; ++k) there = std::fabs(cur_rot[k] - rot[k]) < kMoveEpsilon;
    if (there) {
        ++g_moves_skipped;
        return false;
    }
    memcpy(inst + kInstRot, rot, sizeof(rot));
    memcpy(inst + kInstPos, pos, 12);
    refresh_instance(inst);
    return true;
}

void release_instance(uint8_t* inst) {
    using Release = void(__thiscall*)(uint8_t*);
    (*reinterpret_cast<Release* const*>(inst))[1](inst);  // rl_destroyable_model_instance::release
}

std::vector<uint32_t> g_stand_ins_to_free;  // Havok handles, freed at the end of the frame (not in the render task)

// The chunks the pieces draw from. A piece keeps the reference its general_mover held on its building's chunk (adopt),
// so the chunk stays loaded as long as the piece does; given up, the piece's reference goes back from the next render
// task (release_chunk_refs): in the handshake after the frame's deferred render-instance releases (rl_unlock), before
// stream2_do_frame unloads containers nothing references any more. Pieces leave with the zone they lie in, as the
// game's debris does (d_destream_zone), and a chunk unloading under a piece anyway takes the piece with it first
// (d_chunk_unload).
std::unordered_map<uint32_t, uint32_t> g_chunk_pieces;  // chunk uid -> live pieces drawn from it
// A stream2 container counts its references in a short. Each piece kept its general_mover's reference, and a building
// type with 32,700 pieces in PhysX overflowed it (research/docs/07, B5b): the count went negative, the next release
// unloaded the chunk under its pieces (all given up) and Havok, still stepping bodies with the chunk's shapes, crashed.
// The pieces of one container keep one reference between them, the first one's; the others give theirs back at once.
std::unordered_map<uint32_t, uint32_t> g_srid_pieces;  // container -> live pieces sharing our one reference on it
uint32_t g_srid_peak = 0;                              // the most pieces one reference held at once, this session
std::vector<uint32_t> g_refs_to_release;
uint64_t g_refs_taken = 0, g_refs_missing = 0, g_refs_released = 0;
bool g_stream_owner_checked = false;

void leave_pile(uint32_t slot);  // its sub-instance leaves the pile's instance, which stays for the others (piles)
void cancel_compile(uint8_t* inst);  // the instance's pending mesh compile, if any, is aborted (piles)
void queue_instance_fix(uint8_t* inst);  // its compiled mesh given back, its batchable bit set again (render task)

void give_up(uint32_t slot, bool tell_host) {
    Piece& p = g_pieces[slot];
    if (!p.inst) return;
    if (p.stand_in != kNoStandIn) g_stand_ins_to_free.push_back(p.stand_in);
    if (p.pile != kNoPile) {
        leave_pile(slot);
    } else {
        if (g_drop_meshes) {
            AcquireSRWLockExclusive(&g_piece_set_lock);
            g_piece_set.erase(p.inst);
            ReleaseSRWLockExclusive(&g_piece_set_lock);
        }
        release_instance(p.inst);
    }
    // Sim thread, Havok not stepping (end of frame, the handshake, the level's end): the stand-in body (freed at the
    // end of the frame) holds a reference of its own.
    if (p.shape) rfg::fn<void(__thiscall*)(const void*)>(r::hkReferencedObject__removeReference)(p.shape);
    if (p.srid) {
        const auto held = g_srid_pieces.find(p.srid);
        if (held == g_srid_pieces.end() || --held->second == 0) {  // the last of its container's pieces
            if (held != g_srid_pieces.end()) g_srid_pieces.erase(held);
            g_refs_to_release.push_back(p.srid);
        }
    }
    const auto chunk = g_chunk_pieces.find(p.chunk_uid);
    if (chunk != g_chunk_pieces.end() && --chunk->second == 0) g_chunk_pieces.erase(chunk);
    p = Piece{};
    if (slot < g_seen_state.size()) g_seen_state[slot] = 0;
    if (slot < g_spatial.size()) g_spatial[slot].r = -1.0f;
    g_pile_candidates.erase(slot);
    g_free.push_back(slot);
    if (tell_host) pxbridge::destroy_body(slot);
    ++g_stats.released;
}

void release_chunk_refs() {
    if (g_refs_to_release.empty()) return;
    if (!g_stream_owner_checked) {
        g_stream_owner_checked = true;
        const uint32_t owner = rfg::global<uint32_t>(r::Stream_thread_id);
        if (owner != GetCurrentThreadId())  // stream2_release does nothing on another thread
            LOG("debris offload: WARNING streaming belongs to thread %u, not %u: the pieces' chunk references stay "
                "held", owner, GetCurrentThreadId());
    }
    const auto release = rfg::fn<void(__cdecl*)(uint32_t)>(r::stream2_release);
    for (uint32_t srid : g_refs_to_release) release(srid);
    g_refs_released += g_refs_to_release.size();
    g_refs_to_release.clear();
}

// ---- adoption ----

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

constexpr uint32_t kBodyFilterInfo = 0x2C;  // hkpEntity: m_collidable.m_broadPhaseHandle.m_collisionFilterInfo
// hkpWorldObject::m_properties: hkpProperty {key, pad, value (8 bytes)} at +0x7C, count at +0x80. The game keeps
// the object at key 0 (object_from_world_obj), the physical and effect materials at 3 and 4.
constexpr uint32_t kBodyProps = 0x7C, kBodyPropCount = 0x80;
uint32_t body_property(const uint8_t* body, uint32_t key) {
    const uint8_t* props = *reinterpret_cast<uint8_t* const*>(body + kBodyProps);
    const int n = *reinterpret_cast<const int*>(body + kBodyPropCount);
    for (int i = 0; props && i < n; ++i)
        if (*reinterpret_cast<const uint32_t*>(props + i * 16) == key)
            return *reinterpret_cast<const uint32_t*>(props + i * 16 + 8);
    return 0;
}
bool g_solid = false;
float g_push_mass = 0;  // pieces this heavy are solid: Experiment.PhysXSolidMassKg, else the game's push-away mass
int g_solid_mass_kg = 0;

void bounds(const Parts& parts, Piece& p) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& part : parts.parts)
        for (const ipc::Vec3& v : part) {
            const float c[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
        }
    float r2 = 0;
    for (int k = 0; k < 3; ++k) {
        p.box_c[k] = 0.5f * (lo[k] + hi[k]);
        p.box_size[k] = hi[k] - lo[k];
        const float reach = std::max(std::fabs(lo[k]), std::fabs(hi[k]));
        r2 += reach * reach;
    }
    p.radius = std::sqrt(r2);
}

void quat_from_cols(const float* c0, const float* c1, const float* c2, float q[4]) {
    // Havok rotation columns -> quaternion (x, y, z, w).
    const float m00 = c0[0], m10 = c0[1], m20 = c0[2], m01 = c1[0], m11 = c1[1], m21 = c1[2], m02 = c2[0], m12 = c2[1],
                m22 = c2[2];
    const float tr = m00 + m11 + m22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        q[3] = 0.25f * s, q[0] = (m21 - m12) / s, q[1] = (m02 - m20) / s, q[2] = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2;
        q[3] = (m21 - m12) / s, q[0] = 0.25f * s, q[1] = (m01 + m10) / s, q[2] = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2;
        q[3] = (m02 - m20) / s, q[0] = (m01 + m10) / s, q[1] = 0.25f * s, q[2] = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2;
        q[3] = (m10 - m01) / s, q[0] = (m02 + m20) / s, q[1] = (m12 + m21) / s, q[2] = 0.25f * s;
    }
}

// A new piece stays with the game's physics for its first moments (the game's own debris lasts 2-5 s): pieces thrown
// off a blast hit what stands around them and break it (in single player any loose body damages a building it hits
// hard enough: rfg_dyn_add_pre_collide_contact), and pieces lying on a damaged building weigh on it (the game's stress
// evaluation counts them). Handed over at once, a thermobaric rocket into Vista's sphere tower broke off a quarter of
// the pieces it does in the game. With more than kMaxWaiting waiting the wait shrinks, the oldest going first, so
// Havok carries about what it does in the game. While they wait, their compiled meshes are given back as the pieces'
// are (the game builds one for every new piece; a collapse leaves a thousand waiting, and the address space is 32-bit).
// A piece that has lain still for kAtRestMs when it goes over starts frozen in the host (ipc::kBodyAtRest): a pile that
// settled in Havok and came over awake all at once rocked in PhysX for good. Calm is the host's test for freezing.
constexpr uint32_t kMaxWaiting = 768;
constexpr uint32_t kAtRestMs = 250;
// A piece that waited goes over gentle (ipc::kBodyGentle): what it overlaps then (pieces it moved into meanwhile, the
// pieces of its own shatter, which Havok had pushed apart only so far) is undone slowly, without the bubbling.
constexpr uint32_t kGentleAfterMs = 300;
constexpr float kCalmSpeed = 0.15f, kCalmSpin = 0.5f;  // m/s, rad/s
uint32_t g_adopt_delay_ms = 0;             // Experiment.PhysXAdoptDelayMs
// World-only pieces (layer 44, RFG_WORLD_ONLY: every small piece in Wrecking Crew, about half in single player) go
// over at once (Experiment.PhysXWorldOnlyAtOnce), gentle. While they waited they went through the avenue map's ground,
// a building like any other: born above it, they came over under it within a second (B5a: 135-305 pieces of 16,500-
// 22,000 a stress cycle, then lost to the world), though DebrisCollide's layer table lets them meet buildings; made
// ultra-lite meanwhile they did the same. In the game they meet nothing but the terrain and characters, so what the wait
// is for (pieces breaking and weighing on what they hit) never concerned them; in PhysX they meet everything.
constexpr uint32_t kLayerWorldOnly = 44;
bool g_world_only_at_once = false;
uint64_t g_world_only_went = 0;
uint32_t g_waiting = 0;                    // pieces waiting at the last pass
uint32_t g_waiting_peak = 0;               // for the level-end log
uint32_t g_went_at_rest = 0, g_went_moving = 0;  // for the level-end log
struct Waiting {
    uint32_t since_ms;       // game ms adopt() first saw it
    uint32_t calm_since_ms;  // since when it has been calm (valid with calm)
    bool calm;
};
std::unordered_map<uint32_t, Waiting> g_first_seen;  // object handle -> waiting piece

std::vector<std::pair<uint32_t, uint8_t*>> g_waiting_list;  // this pass's waiting pieces (handle, render instance)

// The game's object behind a waiting piece, if it is still the same live general mover; null when the game removed it.
uint8_t* waiting_object(uint32_t handle, const uint8_t* inst) {
    uint8_t* obj = rfg::fn<uint8_t*(__fastcall*)(void*, void*, uint32_t)>(r::world__object_from_handle)(world(), nullptr,
                                                                                                       handle);
    if (!obj || obj[kObjType] != 2 || obj[kObjType + 1] != 0 || (obj[kObjFlags] & kObjDestroyed) ||
        *reinterpret_cast<uint8_t**>(obj + kGmRenderInstance) != inst)
        return nullptr;  // gone, or its handle now names another object
    return obj;
}

void push_new_piece_by_blow(const uint8_t* body, uint32_t handle, uint32_t now_ms);  // with the melee blows below

// general_mover::update_render_opacity fades a debris piece out once its body rests beyond 35 or 50 m from the camera
// (Shard_fade_dist0_sqr, _dist1_sqr) and back in when the camera comes near. A piece handed over faded out kept its
// render instance's opacity, and nothing fades a piece in PhysX back in: rubble a shockwave mod's blow threw that far
// vanished. Pieces in PhysX stay visible at any distance, so the pieces still waiting in Havok do too: they fade with
// the threshold the game gives moving debris (back in when faded, never out). Fades for deletion are unchanged.
constexpr uint32_t kMoverPermanent = 2;  // never distance-faded by the game either
constexpr uint32_t kObjRenderAlpha = 0xA0;  // uint16, Lparam_super_overbrighten when fully visible
constexpr float kNoDistanceFade = 1e8f;  // the game's threshold for moving debris (squared metres)
void(__fastcall* o_update_render_opacity)(uint8_t*, void*) = nullptr;
void __fastcall d_update_render_opacity(uint8_t* mover, void* edx) {
    const uint32_t mflags = *reinterpret_cast<const uint32_t*>(mover + kMoverFlags);
    if (g_on && (mflags & kMoverLoadBalanced) && !(mflags & kMoverPermanent) &&
        *reinterpret_cast<const int32_t*>(mover + kObjHavokHandle) != -1 && rfg::global<uint8_t>(r::Multiplayer) == 0) {
        rfg::fn<int(__thiscall*)(uint8_t*, float, float)>(r::object_mover__handle_fading)(mover, 0.0f, kNoDistanceFade);
        return;
    }
    o_update_render_opacity(mover, edx);
}

// world::object_physically_in_original_zone(object*): whether the object's position lies in the 511 m square of the
// zone its handle belongs to. general_mover::update_pos_and_orient destroys a single-player general mover the moment
// it leaves that square (the zone it lies in may stream out apart from its own). A piece waiting in Havok for PhysX
// that a blow threw over a zone's border so went from the world in mid-flight: a shockwave hammer sent about 1,500 of
// a tower's 2,000 pieces over one within 0.2 s of the hit (a Dust tower, 2026-10-28; "the structure almost
// disappears"). Such a piece goes to PhysX at the end of the frame instead, where every piece goes with the zone it
// lies in when that streams out. Only calls from that destroy are answered (rfg_mover and save code ask too), and
// only for pieces adopt() takes; one that still has not gone over after kZoneKeepFrames is left to the game.
constexpr uint32_t kZoneCheckReturn = 0x45;  // general_mover::update_pos_and_orient: after its call at +0x40
constexpr uint32_t kZoneKeepFrames = 60;
std::unordered_map<uint32_t, uint32_t> g_adopt_now;  // object handle -> the frame it left its zone
uint32_t g_zone_frame = 0;
uint64_t g_zone_kept = 0, g_zone_kept_adopted = 0, g_zone_left_to_game = 0;
// The general movers the game deleted for leaving their zone, by why they were not kept (the log's census).
enum { kZoneNotDebris, kZoneFading, kZoneNoInstance, kZoneNoBody, kZoneAttached, kZoneNotReady, kZoneReasons };
uint64_t g_zone_deleted[kZoneReasons] = {};
const void* g_zone_check_return = nullptr;
bool(__fastcall* o_in_original_zone)(void*, void*, uint8_t*) = nullptr;
bool __fastcall d_in_original_zone(void* w, void* edx, uint8_t* obj) {
    const bool in = o_in_original_zone(w, edx, obj);
    if (in || !g_on || _ReturnAddress() != g_zone_check_return || !obj) return in;
    // Destroyed already (adopt() took it this frame; the object goes at the frame's end): the game's second destroy
    // would do nothing (world::destroy_object skips a destroyed object), so it is not asked to.
    if (obj[kObjFlags] & kObjDestroyed) return true;
    if (!pxbridge::ready() || !world_mirror::ready()) {
        ++g_zone_deleted[kZoneNotReady];
        return in;
    }
    const uint32_t mflags = *reinterpret_cast<const uint32_t*>(obj + kMoverFlags);
    const uint32_t* attach = reinterpret_cast<const uint32_t*>(obj + kObjAttach);
    const bool has_instance = *reinterpret_cast<uint8_t* const*>(obj + kGmRenderInstance) != nullptr;
    const bool has_body = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle) != 0xFFFFFFFFu;
    const int why = !(mflags & kMoverLoadBalanced)         ? kZoneNotDebris
                    : (mflags & kMoverFading)               ? kZoneFading
                    : !has_instance                         ? kZoneNoInstance
                    : !has_body                             ? kZoneNoBody
                    : (attach[0] || attach[1] || attach[2]) ? kZoneAttached
                                                            : -1;
    if (why >= 0) {
        ++g_zone_deleted[why];
        return in;
    }
    const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHandle);
    const auto [it, first] = g_adopt_now.try_emplace(handle, g_zone_frame);
    if (first) {
        ++g_zone_kept;
    } else if (g_zone_frame - it->second > kZoneKeepFrames) {
        g_adopt_now.erase(it);
        ++g_zone_left_to_game;
        return in;
    }
    return true;
}

void adopt() {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldGeneralMoverCount);
    if (!all || !movers || n <= 0) return;
    uint8_t* const* bodies = static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies));
    const auto is_fading = rfg::fn<bool(__cdecl*)(uint8_t*)>(r::object_mover_is_fading_for_delete);
    const auto destroy = rfg::fn<bool(__thiscall*)(void*, uint8_t*, bool)>(r::world__destroy_object);
    uint32_t budget = kAdoptPerFrame;
    const uint32_t now_ms = rfg::global<uint32_t>(r::Frametime_total_milliseconds);
    ++g_zone_frame;
    if (g_adopt_now.size() > 16384) g_adopt_now.clear();  // the game removed them otherwise
    if (g_first_seen.size() > 65536) {  // pieces the game removed while they waited
        for (auto it = g_first_seen.begin(); it != g_first_seen.end();)
            it = now_ms - it->second.since_ms > 4 * g_adopt_delay_ms + 10000 ? g_first_seen.erase(it) : std::next(it);
    }
    const uint32_t wait_ms =
        g_waiting > kMaxWaiting ? uint32_t(uint64_t(g_adopt_delay_ms) * kMaxWaiting / g_waiting) : g_adopt_delay_ms;
    uint32_t waiting = 0;
    g_waiting_list.clear();
    for (int i = 0; i < n && budget && !g_free.empty(); ++i) {
        uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        const uint32_t mflags = *reinterpret_cast<const uint32_t*>(obj + kMoverFlags);
        if (!(mflags & kMoverLoadBalanced) || (mflags & kMoverFading) || is_fading(obj)) continue;
        uint8_t* inst = *reinterpret_cast<uint8_t**>(obj + kGmRenderInstance);
        if (!inst) continue;
        const uint32_t* attach = reinterpret_cast<const uint32_t*>(obj + kObjAttach);
        if (attach[0] || attach[1] || attach[2]) continue;  // constrained or attached: stays with the game
        const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
        if (handle == 0xFFFFFFFFu) continue;
        const uint8_t* body = bodies[handle];
        if (!body) continue;
        const uint8_t motion = body[kBodyMotionType];
        if (motion == 0 || motion == 4 || motion == 5) continue;  // invalid, keyframed, fixed
        const uint32_t obj_handle = *reinterpret_cast<const uint32_t*>(obj + kObjHandle);
        bool at_rest = false, waited = false;
        if (g_adopt_delay_ms) {
            const auto [seen, first_sight] = g_first_seen.try_emplace(obj_handle, Waiting{now_ms, 0, false});
            Waiting& wt = seen->second;
            if (first_sight) push_new_piece_by_blow(body, handle, now_ms);
            const float* lv = reinterpret_cast<const float*>(body + kBodyLinVel);
            const float* av = reinterpret_cast<const float*>(body + kBodyAngVel);
            const bool calm = lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2] < kCalmSpeed * kCalmSpeed &&
                              av[0] * av[0] + av[1] * av[1] + av[2] * av[2] < kCalmSpin * kCalmSpin;
            if (calm && !wt.calm) wt.calm_since_ms = now_ms;
            wt.calm = calm;
            const bool left_zone = !g_adopt_now.empty() && g_adopt_now.count(obj_handle);  // goes over now
            // a world-only piece (it may become one while it waits, settling into its class) goes now, gentle: it may
            // lie in what it broke off (PhysX shot one such through the ground)
            const bool world_only = g_world_only_at_once &&
                                    (*reinterpret_cast<const uint32_t*>(body + kBodyFilterInfo) & 0x7F) == kLayerWorldOnly;
            if (!left_zone && !world_only && now_ms - wt.since_ms < wait_ms) {
                g_waiting_list.emplace_back(obj_handle, inst);
                ++waiting;
                continue;
            }
            at_rest = calm && now_ms - wt.calm_since_ms >= kAtRestMs;
            waited = world_only || now_ms - wt.since_ms >= kGentleAfterMs;
            if (world_only) ++g_world_only_went;
        }
        Parts parts;
        if (!walk_guarded(hs::body_shape(body), parts) || parts.has_triangles || parts.parts.empty()) continue;
        const float inv_mass = *reinterpret_cast<const float*>(body + kBodyInvMass);
        const float mass = inv_mass > 1e-6f ? 1.0f / inv_mass : 100.0f;
        const bool heavy = g_solid && mass >= g_push_mass;
        const uint32_t shape = intern_shape(parts, heavy ? ipc::kShapeHeavy : 0);
        if (!shape) break;  // the ring is full
        const hs::Transform t = hs::body_transform(body);
        float q[4];
        const float c0[3] = {t.rot[0][0], t.rot[1][0], t.rot[2][0]}, c1[3] = {t.rot[0][1], t.rot[1][1], t.rot[2][1]},
                    c2[3] = {t.rot[0][2], t.rot[1][2], t.rot[2][2]};
        quat_from_cols(c0, c1, c2, q);
        const float* lv = reinterpret_cast<const float*>(body + kBodyLinVel);
        const float* av = reinterpret_cast<const float*>(body + kBodyAngVel);
        const uint32_t slot = g_free.back();
        if (!pxbridge::create_body(slot, shape, {t.pos[0], t.pos[1], t.pos[2]}, {q[0], q[1], q[2], q[3]},
                                   {lv[0], lv[1], lv[2]}, {av[0], av[1], av[2]}, mass,
                                   ipc::kBodyMass | (at_rest ? ipc::kBodyAtRest : 0u) |
                                       (waited ? ipc::kBodyGentle : 0u)))
            break;  // the ring is full
        g_free.pop_back();
        ++g_mass_hist[mass_bucket(mass)];
        for (const float radius : parts.radii) {
            g_radius_min = std::min(g_radius_min, radius), g_radius_max = std::max(g_radius_max, radius);
            g_radius_sum += radius;
            ++g_radius_n;
        }
        ++g_adopted_top_types[std::min<uint8_t>(hs::type_of(hs::body_shape(body)), hs::kTypeCount)];
        // The render instance is ours now: with the pointer cleared the general_mover does not release it. Its
        // reference on its chunk becomes the piece's too (with srid cleared world::free_object keeps it): without it
        // the chunk unloaded once the building's zone streamed out, the pieces drawing from freed memory (the first
        // campaign session: stretched triangles at a distance, then a crash in rl_scene_renderer::render).
        if (*reinterpret_cast<const uint16_t*>(obj + kObjRenderAlpha) <
            uint16_t(rfg::global<uint32_t>(r::Lparam_super_overbrighten)))
            ++g_adopted_faded;
        const uint32_t srid = *reinterpret_cast<const uint32_t*>(obj + kObjSrid);
        const bool holds = srid && (obj[kObjStreamFlags] & kObjStreamHeld);
        if (holds) *reinterpret_cast<uint32_t*>(obj + kObjSrid) = 0;
        const uint32_t chunk_uid = *reinterpret_cast<const uint32_t*>(obj + kMoverChunkUid);
        const uint32_t original = *reinterpret_cast<const uint32_t*>(obj + kGmOriginalObject);
        *reinterpret_cast<uint8_t**>(obj + kGmRenderInstance) = nullptr;
        // Its collision shape outlives its body: the piece's stand-ins use it (a convex vertices shape owns its
        // data; any other kind is left alone, and its stand-ins are boxes).
        const uint8_t* own_shape = hs::body_shape(body);
        if (own_shape && hs::type_of(own_shape) == hs::kConvexVertices)
            rfg::fn<void(__thiscall*)(const void*)>(r::hkReferencedObject__addReference)(own_shape);
        else
            own_shape = nullptr;
        destroy(w, obj, false);
        if (g_adopt_delay_ms) {
            g_first_seen.erase(obj_handle);
            ++(at_rest ? g_went_at_rest : g_went_moving);
        }
        if (!g_adopt_now.empty() && g_adopt_now.erase(obj_handle)) ++g_zone_kept_adopted;
        Piece& p = g_pieces[slot];
        p = Piece{};
        p.inst = inst;
        p.fresh = 1;
        p.srid = holds ? srid : 0;
        if (p.srid) {
            uint32_t& sharing = g_srid_pieces[p.srid];
            if (sharing++) g_refs_to_release.push_back(p.srid);  // the container is held already
            g_srid_peak = std::max(g_srid_peak, sharing);
        }
        p.chunk_uid = chunk_uid;
        p.original = original;
        if (chunk_uid != kNoChunk) ++g_chunk_pieces[chunk_uid];
        ++(holds ? g_refs_taken : g_refs_missing);
        if (g_drop_meshes) {
            AcquireSRWLockExclusive(&g_piece_set_lock);
            g_piece_set.insert(inst);
            ReleaseSRWLockExclusive(&g_piece_set_lock);
            // The compile the game asked for when the piece split off makes a mesh that would only be given back:
            // aborted at its next stop (or given back as before if it is done first). A mesh it already has goes in
            // the next render task, where the batchable bit is set again too.
            cancel_compile(inst);
        }
        queue_instance_fix(inst);
        if (slot < g_seen_state.size()) g_seen_state[slot] = 0;
        p.heavy = heavy ? 1 : 0;
        p.shape = own_shape;
        p.mass = mass;
        p.physical_material = uint8_t(body_property(body, 3));
        p.effect_material = uint8_t(body_property(body, 4));
        bounds(parts, p);
        g_largest_radius = std::max(g_largest_radius, std::min(p.radius, 50.0f));
        ++g_stats.adopted;
        ++g_total_adopted;
        --budget;
    }
    g_waiting = waiting;
    g_waiting_peak = std::max(g_waiting_peak, waiting);
}

// ---- per frame ----

// Render task (below): the sim/render handshake, the render thread waiting.
uint32_t g_unpiles_left = 0;  // this frame's budget (piles)
// The render task's parts over the level (it adds to the frame): total and longest per frame, and their time in the
// frames where the whole task took over 1 ms.
struct TaskPart {
    double total = 0, longest = 0, in_slow = 0;
};
enum { kTaskMoves, kTaskSplits, kTaskPiles, kTaskRest, kTaskParts };
TaskPart g_task[kTaskParts];
uint64_t g_task_frames = 0, g_task_slow = 0;
double g_split_ms = 0;  // this frame's time in unpile()
void note_task_frame(const double (&ms)[kTaskParts]) {
    double sum = 0;
    for (double v : ms) sum += v;
    ++g_task_frames;
    const bool slow = sum > 1.0;
    g_task_slow += slow;
    for (int k = 0; k < kTaskParts; ++k) {
        g_task[k].total += ms[k];
        g_task[k].longest = std::max(g_task[k].longest, ms[k]);
        if (slow) g_task[k].in_slow += ms[k];
    }
}
bool unpile(uint32_t slot, const ipc::BodyPose& b, double now_ms);  // piles
bool moved_from_pile(const Piece& p, const ipc::BodyPose& b);       // piles
extern bool g_piles_on;                                             // piles

// Render task, first: the pieces' instances whose compiled mesh arrived (or that were just taken over) give it back
// and are drawn batched again (Experiment.PhysXDebrisDropMeshes, PhysXDebrisBatch). A queued instance that is no
// piece's any more (given up, or a pile's) is left alone.
void fix_piece_instances() {
    static std::vector<uint8_t*> work;
    AcquireSRWLockExclusive(&g_fix_lock);
    work.swap(g_fix_queue);
    ReleaseSRWLockExclusive(&g_fix_lock);
    if (work.empty()) return;
    const auto delete_mesh = rfg::fn<void(__cdecl*)(void*)>(r::rl_delete_mesh_recompile_data);
    size_t done = 0, drops = 0;
    for (; done < work.size() && drops < kDropPerFrame; ++done) {
        uint8_t* inst = work[done];
        AcquireSRWLockShared(&g_piece_set_lock);
        const bool piece = g_piece_set.count(inst) != 0;
        ReleaseSRWLockShared(&g_piece_set_lock);
        if (!piece) continue;
        void*& mesh = *reinterpret_cast<void**>(inst + kInstRecompileData);
        if (g_drop_meshes && mesh) {
            delete_mesh(mesh);
            mesh = nullptr;
            ++drops;
            ++g_meshes_dropped;
        }
        uint16_t& flags = *reinterpret_cast<uint16_t*>(inst + kInstFlags);
        if (g_batch && !(flags & kInstBatchable)) {
            flags |= kInstBatchable;
            ++g_batch_restored;
        }
    }
    if (done < work.size()) {  // the game's delete queue takes a few hundred a frame: the rest next frame
        AcquireSRWLockExclusive(&g_fix_lock);
        g_fix_queue.insert(g_fix_queue.begin(), work.begin() + done, work.end());
        ReleaseSRWLockExclusive(&g_fix_lock);
    }
    work.clear();
}

void queue_instance_fix(uint8_t* inst) {
    AcquireSRWLockExclusive(&g_fix_lock);
    g_fix_queue.push_back(inst);
    ReleaseSRWLockExclusive(&g_fix_lock);
}

// rl_destroyable_model_instance::set_compiled_cache_data(rl_mesh_recompile_data*, rl_mesh_recompile_parameters*): a
// compiled mesh arrives (rl_mesh_recompile_finalize); it returns the one it replaces.
void*(__fastcall* o_set_compiled)(uint8_t*, void*, void*, void*) = nullptr;
void* __fastcall d_set_compiled(uint8_t* inst, void* edx, void* data, void* params) {
    void* old = o_set_compiled(inst, edx, data, params);
    AcquireSRWLockShared(&g_piece_set_lock);
    const bool piece = g_piece_set.count(inst) != 0;
    ReleaseSRWLockShared(&g_piece_set_lock);
    if (piece) queue_instance_fix(inst);
    return old;
}

// One slot that moves or changed state since the render task last acted on it.
void move_piece(uint32_t i, uint8_t state, const ipc::BodyPose& b, float alpha, double now_ms) {
    g_seen_state[i] = state;
    Piece& p = g_pieces[i];
    if (!p.inst || i == g_player_slot) return;
    p.fresh = 0;
    g_spatial[i] = (state & ipc::kPoseAlive) ? Spatial{b.pos.x, b.pos.y, b.pos.z, p.radius} : Spatial{0, 0, 0, -1.0f};
    // Still: frozen by the host, or asleep in PhysX (a third of a levelled map's pieces fell asleep before the host
    // froze them, and stayed out of the piles). Either wakes when something touches it.
    const bool still = (state & ipc::kPoseAlive) && !(state & ipc::kPoseAwake);
    if (!still) {
        p.frozen_since_ms = 0;
    } else {
        if (!p.frozen_since_ms) p.frozen_since_ms = now_ms;
        if (g_piles_on && p.pile == kNoPile && !p.candidate) {
            p.candidate = 1;
            g_pile_candidates.insert(i);
        }
    }
    if (p.pile != kNoPile) {  // its pile draws it; once PhysX moved it, it goes on its own again
        if (!still && moved_from_pile(p, b)) {
            const int64_t t0 = session::qpc();
            if (g_unpiles_left && unpile(i, b, now_ms))
                --g_unpiles_left;
            else
                g_seen_state[i] = 0;  // looked at again next frame
            g_split_ms += session::ticks_to_ms(session::qpc() - t0);
        }
        return;
    }
    if (!(state & ipc::kPoseAlive)) return;  // not created yet in the host
    const bool awake = (state & ipc::kPoseAwake) != 0;
    if (!awake && !p.settle) return;  // at rest and already placed there
    float pos[3], q[4];
    if (awake) {
        pos[0] = b.prev_pos.x + (b.pos.x - b.prev_pos.x) * alpha;
        pos[1] = b.prev_pos.y + (b.pos.y - b.prev_pos.y) * alpha;
        pos[2] = b.prev_pos.z + (b.pos.z - b.prev_pos.z) * alpha;
        ipc::Quat a = b.prev_rot, c = b.rot;
        if (a.x * c.x + a.y * c.y + a.z * c.z + a.w * c.w < 0) c = {-c.x, -c.y, -c.z, -c.w};
        q[0] = a.x + (c.x - a.x) * alpha, q[1] = a.y + (c.y - a.y) * alpha, q[2] = a.z + (c.z - a.z) * alpha,
        q[3] = a.w + (c.w - a.w) * alpha;
        const float inv = 1.0f / std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (float& v : q) v *= inv;
    } else {
        pos[0] = b.pos.x, pos[1] = b.pos.y, pos[2] = b.pos.z;
        q[0] = b.rot.x, q[1] = b.rot.y, q[2] = b.rot.z, q[3] = b.rot.w;
    }
    p.settle = awake ? 1 : 0;
    if (b.pos.y < kLowY && b.prev_pos.y >= kLowY && g_low_logged < kLowLogged &&
        std::find(g_low_slots, g_low_slots + g_low_logged, i) == g_low_slots + g_low_logged) {
        g_low_slots[g_low_logged++] = i;
        LOG("debris offload: piece %u went below %.0f m at (%.1f, %.1f, %.1f), from (%.1f, %.1f, %.1f) a step before", i,
            double(kLowY), double(b.pos.x), double(b.pos.y), double(b.pos.z), double(b.prev_pos.x),
            double(b.prev_pos.y), double(b.prev_pos.z));
    }
    if (pos[1] < kFloorY) {
        give_up(i, true);
        ++g_fell;
        return;
    }
    if (move_instance_if_moved(p.inst, pos, q)) ++g_stats.moved;
}

// The slots to look at: awake in the host, or a state other than the one last acted on; 16 a step (SSE2).
void move_pieces() {
    bool fresh = false;
    const ipc::SnapshotHeader* s = pxbridge::snapshot(&fresh);
    if (!s) return;
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint8_t* states = pxbridge::states(s);
    const uint32_t slots = std::min<uint32_t>(s->slots, g_slots);
    if (g_seen_state.size() < size_t(g_slots) + 16) g_seen_state.resize(size_t(g_slots) + 16, 0);
    if (g_spatial.size() < g_slots) g_spatial.resize(g_slots, Spatial{0, 0, 0, -1.0f});
    const float period_ms = 1000.0f / pxbridge::step_hz();
    float alpha = float(session::ticks_to_ms(session::qpc() - s->publish_qpc)) / period_ms;
    alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    const double now_ms = session::since_start_ms();
    const __m128i awake = _mm_set1_epi8(char(ipc::kPoseAwake));
    for (uint32_t first = 0; first < slots; first += 16) {
        // The state bytes past `slots` are within the snapshot (max_bodies is a multiple of 16) but not looked at.
        const __m128i now = _mm_loadu_si128(reinterpret_cast<const __m128i*>(states + first));
        const __m128i seen = _mm_loadu_si128(reinterpret_cast<const __m128i*>(g_seen_state.data() + first));
        uint32_t mask = (~uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(now, seen))) |
                         uint32_t(_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(now, awake), awake)))) &
                        0xFFFFu;
        if (slots - first < 16) mask &= (1u << (slots - first)) - 1;
        while (mask) {
            unsigned long bit;
            _BitScanForward(&bit, mask);
            mask &= mask - 1;
            const uint32_t i = first + bit;
            move_piece(i, states[i], poses[i], alpha, now_ms);
        }
    }
}

// Render task: the pieces still waiting in Havok give back their compiled meshes and are drawn batched, as the pieces
// are. The game owns their instances, so each is looked up again first (the game may have removed it since adopt()).
void batch_waiting() {
    if (!g_batch) return;
    const auto delete_mesh = rfg::fn<void(__cdecl*)(void*)>(r::rl_delete_mesh_recompile_data);
    uint32_t drops = 0;
    for (const auto& [handle, inst] : g_waiting_list) {
        if (!waiting_object(handle, inst)) continue;
        if (g_drop_meshes && drops < kDropPerFrame) {
            void*& mesh = *reinterpret_cast<void**>(inst + kInstRecompileData);
            if (mesh) {
                delete_mesh(mesh);
                mesh = nullptr;
                ++drops;
                ++g_meshes_dropped;
            }
        }
        uint16_t& flags = *reinterpret_cast<uint16_t*>(inst + kInstFlags);
        if (!(flags & kInstBatchable)) {
            flags |= kInstBatchable;
            ++g_batch_restored;
        }
    }
}

constexpr float kPlayerJump = 3.0f;  // m in one frame: a teleport, not a move

void player_proxy() {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return;
    const float* p = reinterpret_cast<const float*>(player + 0x04);
    const ipc::Vec3 centre{p[0], p[1] + 0.9f, p[2]};
    if (!g_player_ready) {
        // A capsule as a hull: two rings of 12 points, 0.35 m radius, 1.1 m apart, plus the poles.
        std::vector<ipc::Vec3> v;
        for (int ring = 0; ring < 2; ++ring)
            for (int s = 0; s < 12; ++s) {
                const float a = 6.2831853f * float(s) / 12.0f;
                v.push_back({0.35f * std::cos(a), ring ? 0.55f : -0.55f, 0.35f * std::sin(a)});
            }
        v.push_back({0, 0.9f, 0});
        v.push_back({0, -0.9f, 0});
        if (!send_part(kPlayerShape, 0, v, g_solid ? ipc::kShapePlayer : 0)) return;
        if (!pxbridge::create_body(g_player_slot, kPlayerShape, centre, {0, 0, 0, 1}, {0, 0, 0}, {0, 0, 0}, 90.0f,
                                   ipc::kBodyKinematic | ipc::kBodyMass))
            return;
        g_pieces[g_player_slot].inst = nullptr;
        g_player_ready = true;
        return;
    }
    // A jump (a respawn, getting into or out of a vehicle) is a teleport: swept through a pile, the capsule flung
    // the pieces it passed at the speed of the jump.
    static ipc::Vec3 last{};
    const float dx = centre.x - last.x, dy = centre.y - last.y, dz = centre.z - last.z;
    ipc::KinematicTarget c{};
    c.h = {uint16_t(ipc::Cmd::KinematicTarget), 0, sizeof(c)};
    c.slot = g_player_slot;
    c.flags = dx * dx + dy * dy + dz * dz > kPlayerJump * kPlayerJump ? ipc::kTargetTeleport : 0u;
    c.pos = centre;
    c.rot = {0, 0, 0, 1};
    if (pxbridge::send_record(&c, sizeof(c))) last = centre;
}

// ---- vehicles and walkers in the pile ----

// The Havok bodies of vehicles (a walker is several: its hull, legs and arms) have kinematic stand-ins in the host
// that follow them every frame, push the pieces out of their way and wake the frozen ones ahead of them
// (ipc::kBodyPusher). Before, vehicles and walkers went through piles as if nothing was there. One way only: the
// vehicles do not feel the pieces, so a car ploughs through rubble instead of riding over it. Loose chunks of
// buildings (rfg movers come loose, the clusters that break up further when shot) push the pieces the same way: they
// stay in Havok, where the pieces are not.
constexpr uint32_t kVehicleSlots = 256;       // host slots below the player's
constexpr float kVehicleRange = 200.0f;       // m from the player
constexpr float kChunkRange = 100.0f;         // for loose chunks, which give way to vehicles when the slots run out
constexpr double kVehicleScanMs = 250.0;
constexpr float kVehicleJump = 4.0f;          // m in one frame: a teleport (a spawn, a reset), not a move
constexpr size_t kProxyMaxParts = 256;        // a body of more convex parts is left out (a large part of a building)
constexpr int8_t kObjTypeMover = 2, kSubTypeMoverRfg = 1;  // OT_MOVER, OT_SUB_MOVER_RFG
constexpr int8_t kObjTypeVehicle = 3;         // OT_VEHICLE (walkers too)
constexpr uint32_t kEntityWorld = 0x8;        // hkpWorldObject::m_world: null once the body left the world
constexpr uint32_t kDestroyableExisting = 0x48;  // rfg_destroyable_shape::num_existing_child_shapes
uint32_t g_vehicle_first = 0;                 // first of the proxy slots

// A tracked body holds a reference of ours, so its memory stays valid until we let go of it, whatever the game does.
void body_add_ref(const uint8_t* body) { rfg::fn<void(__thiscall*)(const void*)>(r::hkReferencedObject__addReference)(body); }
void body_release(const uint8_t* body) { rfg::fn<void(__thiscall*)(const void*)>(r::hkReferencedObject__removeReference)(body); }

struct VehicleProxy {
    const uint8_t* body;  // referenced
    uint32_t slot;
    uint32_t signature;  // a chunk's shape: 1 + its pieces (it loses some when shot: the proxy is made again)
    float pos[3], q[4];
    bool seen;
    bool chunk;
    float travelled;     // m since the last diagnostics log (Telemetry.Enabled)
    uint32_t targets;    // moves sent since then
    // Telemetry.Enabled: how its shape moved. A chunk's origin can be metres from the chunk (its building's), so the
    // origin's path says little; the middle of its shape's bounds says more.
    float mid[3] = {}, half = 0;                // in its own frame; half the bounds' diagonal
    float mid_from[3] = {}, mid_last[3] = {};   // in the world: at the last log, at its last move
    float mid_path = 0, turned = 0;             // m, degrees since the last log, move by move
    uint32_t turned_back = 0;                   // moves that turned it back against the one before
    float turn_axis[3] = {};
    float top_speed = 0, top_spin = 0;          // Havok's velocities at its moves, the largest
};
std::vector<VehicleProxy> g_vehicles;
std::vector<uint32_t> g_vehicle_free;
double g_next_vehicle_scan = 0;
uint32_t g_vehicle_proxies_made = 0, g_chunk_proxies_made = 0, g_vehicle_proxies_peak = 0, g_vehicle_shapes_failed = 0;
uint32_t g_proxy_shapes_too_large = 0;

uint32_t shape_signature(const uint8_t* body) {
    const uint8_t* shape = hs::body_shape(body);
    if (!shape || *reinterpret_cast<const uintptr_t*>(shape) != uintptr_t(rfg::addr(r::rfg_destroyable_shape_bvtree_vftable)))
        return 0;
    return 1u + *reinterpret_cast<const uint32_t*>(shape + kDestroyableExisting);
}

// A vehicle body's shape for the host: its convex parts, and the box around its triangles if it has any.
struct VehicleParts : Parts {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    bool has_mesh = false;
    void triangle(const float* a, const float* b, const float* c) override {
        has_mesh = true;
        for (const float* p : {a, b, c})
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    }
    void unsupported(uint8_t) override {}
};

uint32_t vehicle_shape(const uint8_t* body, float mid[3], float& half) {
    VehicleParts p;
    walk_guarded(hs::body_shape(body), p);  // what could be read
    if (p.has_mesh && p.hi[0] >= p.lo[0]) {
        std::vector<ipc::Vec3> box;
        for (int i = 0; i < 8; ++i)
            box.push_back({(i & 1) ? p.hi[0] : p.lo[0], (i & 2) ? p.hi[1] : p.lo[1], (i & 4) ? p.hi[2] : p.lo[2]});
        p.parts.push_back(std::move(box));
        p.radii.push_back(0.0f);
    }
    if (p.parts.empty()) return 0;
    if (p.parts.size() > kProxyMaxParts) {
        ++g_proxy_shapes_too_large;
        return 0;
    }
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const std::vector<ipc::Vec3>& part : p.parts)
        for (const ipc::Vec3& v : part) {
            const float c[3] = {v.x, v.y, v.z};
            for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
        }
    for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (lo[k] + hi[k]);
    half = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                            (hi[2] - lo[2]) * (hi[2] - lo[2]));
    return intern_shape(p, 0);
}

// Whether a body's layer collides with debris in the game's layer table: a walker's legs and arms do, the big
// capsule around its hull (layer 14) does not, and in the host it would push a bubble through every pile.
constexpr uint32_t kLayerDebris = 41;  // COLLIDE_RFG_NORMAL_FAST
bool layer_bit(uint32_t a, uint32_t b) {
    const uint8_t* filter = rfg::global<uint8_t*>(r::Havok_filter);
    const uint32_t* table = filter ? *reinterpret_cast<const uint32_t* const*>(filter + 0x34) : nullptr;
    if (!table || a >= 128 || b >= 128) return false;
    const uint32_t bit = a * 128 + b;
    return (table[bit >> 5] >> (bit & 31)) & 1u;
}
bool touches_debris(const uint8_t* body) {
    return layer_bit(*reinterpret_cast<const uint32_t*>(body + kBodyFilterInfo) & 0x7F, kLayerDebris);
}

void body_pose(const uint8_t* body, float pos[3], float q[4]) {
    const hs::Transform t = hs::body_transform(body);
    const float c0[3] = {t.rot[0][0], t.rot[1][0], t.rot[2][0]}, c1[3] = {t.rot[0][1], t.rot[1][1], t.rot[2][1]},
                c2[3] = {t.rot[0][2], t.rot[1][2], t.rot[2][2]};
    quat_from_cols(c0, c1, c2, q);
    memcpy(pos, t.pos, 12);
}

void drop_vehicle(size_t i) {
    pxbridge::destroy_body(g_vehicles[i].slot);
    g_vehicle_free.push_back(g_vehicles[i].slot);
    body_release(g_vehicles[i].body);
    g_vehicles[i] = g_vehicles.back();
    g_vehicles.pop_back();
}

// Four times a second: the vehicle bodies in Havok's simulation islands within range of the player get proxies,
// and proxies whose body left the islands or the range go.
void scan_vehicles(const float* player_pos) {
    uint8_t* hw = rfg::global<uint8_t*>(r::Havok_world);
    if (!hw) return;
    const auto object_of = rfg::fn<uint8_t*(__cdecl*)(const uint8_t*)>(r::object_from_world_obj);
    for (VehicleProxy& v : g_vehicles) v.seen = false;
    for (uint32_t list = 0; list < 2; ++list) {  // m_activeSimulationIslands, m_inactiveSimulationIslands
        const uint8_t* const* islands = *reinterpret_cast<uint8_t* const* const*>(hw + 0x28 + list * 0xC);
        const int ni = *reinterpret_cast<const int*>(hw + 0x2C + list * 0xC);
        for (int i = 0; islands && i < ni; ++i) {
            const uint8_t* island = islands[i];
            if (!island) continue;
            const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + 0x4C);
            const int ne = *reinterpret_cast<const int*>(island + 0x50);
            for (int e = 0; entities && e < ne; ++e) {
                const uint8_t* body = entities[e];
                const uint8_t* obj = body ? object_of(body) : nullptr;
                if (!obj) continue;
                const bool vehicle = int8_t(obj[kObjType]) == kObjTypeVehicle;
                const bool chunk = int8_t(obj[kObjType]) == kObjTypeMover && int8_t(obj[kObjType + 1]) == kSubTypeMoverRfg;
                if (!(vehicle || chunk) || !touches_debris(body)) continue;
                float pos[3], q[4];
                body_pose(body, pos, q);
                const float dx = pos[0] - player_pos[0], dy = pos[1] - player_pos[1], dz = pos[2] - player_pos[2];
                const float range = chunk ? kChunkRange : kVehicleRange;
                if (dx * dx + dy * dy + dz * dz > range * range) continue;
                const uint32_t signature = chunk ? shape_signature(body) : 0;
                auto it = std::find_if(g_vehicles.begin(), g_vehicles.end(),
                                       [body](const VehicleProxy& v) { return v.body == body; });
                if (it != g_vehicles.end()) {
                    if (it->signature == signature) {
                        it->seen = true;
                        continue;
                    }
                    drop_vehicle(size_t(it - g_vehicles.begin()));  // a chunk that lost pieces: made again
                }
                if (g_vehicle_free.empty() && !chunk) {
                    // A vehicle (a walker's leg) before any chunk: the chunk farthest from the player gives way.
                    size_t farthest = g_vehicles.size();
                    float far2 = -1.0f;
                    for (size_t k = 0; k < g_vehicles.size(); ++k) {
                        if (!g_vehicles[k].chunk) continue;
                        const float* vp = g_vehicles[k].pos;
                        const float d2 = (vp[0] - player_pos[0]) * (vp[0] - player_pos[0]) +
                                         (vp[1] - player_pos[1]) * (vp[1] - player_pos[1]) +
                                         (vp[2] - player_pos[2]) * (vp[2] - player_pos[2]);
                        if (d2 > far2) far2 = d2, farthest = k;
                    }
                    if (farthest < g_vehicles.size()) drop_vehicle(farthest);
                }
                if (g_vehicle_free.empty()) continue;
                float mid[3], half = 0;
                const uint32_t shape = vehicle_shape(body, mid, half);
                if (!shape) {
                    ++g_vehicle_shapes_failed;
                    continue;
                }
                const uint32_t slot = g_vehicle_free.back();
                if (!pxbridge::create_body(slot, shape, {pos[0], pos[1], pos[2]}, {q[0], q[1], q[2], q[3]}, {0, 0, 0},
                                           {0, 0, 0}, 1000.0f, ipc::kBodyKinematic | ipc::kBodyMass | ipc::kBodyPusher))
                    return;  // the ring is full: next scan
                g_vehicle_free.pop_back();
                body_add_ref(body);
                VehicleProxy v{body, slot, signature, {pos[0], pos[1], pos[2]}, {q[0], q[1], q[2], q[3]}, true, chunk, 0.0f, 0};
                memcpy(v.mid, mid, sizeof(mid));
                v.half = half;
                hs::body_transform(body).apply(mid, v.mid_from);
                memcpy(v.mid_last, v.mid_from, sizeof(v.mid_last));
                g_vehicles.push_back(v);
                ++(chunk ? g_chunk_proxies_made : g_vehicle_proxies_made);
            }
        }
    }
    for (size_t i = g_vehicles.size(); i-- > 0;)
        if (!g_vehicles[i].seen) drop_vehicle(i);
    g_vehicle_proxies_peak = std::max(g_vehicle_proxies_peak, uint32_t(g_vehicles.size()));
}

// Telemetry.Enabled: a proxy's move to `q`, for log_busy_proxies (v.q is still the pose before it).
void note_proxy_move(VehicleProxy& v, const float q[4]) {
    float m[3];
    hs::body_transform(v.body).apply(v.mid, m);
    const float dx = m[0] - v.mid_last[0], dy = m[1] - v.mid_last[1], dz = m[2] - v.mid_last[2];
    v.mid_path += std::sqrt(dx * dx + dy * dy + dz * dz);
    memcpy(v.mid_last, m, sizeof(m));
    // The turn since its last move, q * conjugate(v.q), with w >= 0.
    const float* a = q;
    const float* b = v.q;
    float t[4] = {-a[3] * b[0] + a[0] * b[3] - a[1] * b[2] + a[2] * b[1],
                  -a[3] * b[1] + a[0] * b[2] + a[1] * b[3] - a[2] * b[0],
                  -a[3] * b[2] - a[0] * b[1] + a[1] * b[0] + a[2] * b[3],
                  a[3] * b[3] + a[0] * b[0] + a[1] * b[1] + a[2] * b[2]};
    if (t[3] < 0) t[0] = -t[0], t[1] = -t[1], t[2] = -t[2], t[3] = -t[3];
    const float s = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    v.turned += 2.0f * std::atan2(s, t[3]) * 57.29578f;
    if (s > 1e-6f) {
        const float axis[3] = {t[0] / s, t[1] / s, t[2] / s};
        if (axis[0] * v.turn_axis[0] + axis[1] * v.turn_axis[1] + axis[2] * v.turn_axis[2] < -0.5f) ++v.turned_back;
        memcpy(v.turn_axis, axis, sizeof(axis));
    }
    const float* lv = reinterpret_cast<const float*>(v.body + kBodyLinVel);
    const float* av = reinterpret_cast<const float*>(v.body + kBodyAngVel);
    v.top_speed = std::max(v.top_speed, std::sqrt(lv[0] * lv[0] + lv[1] * lv[1] + lv[2] * lv[2]));
    v.top_spin = std::max(v.top_spin, std::sqrt(av[0] * av[0] + av[1] * av[1] + av[2] * av[2]));
}

// Every frame: each proxy follows its body (a body that left the world takes its proxy with it).
void move_vehicles() {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return;
    const double now = session::since_start_ms();
    if (now >= g_next_vehicle_scan) {
        g_next_vehicle_scan = now + kVehicleScanMs;
        scan_vehicles(reinterpret_cast<const float*>(player + 0x04));
    }
    for (size_t i = g_vehicles.size(); i-- > 0;) {
        VehicleProxy& v = g_vehicles[i];
        if (!*reinterpret_cast<void* const*>(v.body + kEntityWorld)) {
            drop_vehicle(i);
            continue;
        }
        float pos[3], q[4];
        body_pose(v.body, pos, q);
        const float dx = pos[0] - v.pos[0], dy = pos[1] - v.pos[1], dz = pos[2] - v.pos[2];
        const float moved2 = dx * dx + dy * dy + dz * dz;
        const float same = std::fabs(q[0] * v.q[0] + q[1] * v.q[1] + q[2] * v.q[2] + q[3] * v.q[3]);
        // Parked, or settling by millimetres (and the rounding of its rotation, which alone moved every proxy every
        // frame): within 5 mm and half a degree of where its proxy was last put.
        if (moved2 < 25e-6f && same > 0.99999f) continue;
        if (g_diag) note_proxy_move(v, q);
        ipc::KinematicTarget c{};
        c.h = {uint16_t(ipc::Cmd::KinematicTarget), 0, sizeof(c)};
        c.slot = v.slot;
        c.flags = moved2 > kVehicleJump * kVehicleJump ? ipc::kTargetTeleport : 0u;
        c.pos = {pos[0], pos[1], pos[2]};
        c.rot = {q[0], q[1], q[2], q[3]};
        if (!pxbridge::send_record(&c, sizeof(c))) return;
        memcpy(v.pos, pos, sizeof(pos));
        memcpy(v.q, q, sizeof(q));
        v.travelled += std::sqrt(moved2);
        ++v.targets;
    }
}

// Telemetry.Enabled, every 10 s: the proxies that moved the most (the host logs the ones that woke the most pieces,
// by slot).
void log_busy_proxies() {
    std::vector<const VehicleProxy*> busy;
    for (const VehicleProxy& v : g_vehicles)
        if (v.targets) busy.push_back(&v);
    std::sort(busy.begin(), busy.end(), [](const VehicleProxy* a, const VehicleProxy* b) { return a->targets > b->targets; });
    for (size_t i = 0; i < busy.size() && i < 4; ++i) {
        const VehicleProxy& v = *busy[i];
        const float nx = v.mid_last[0] - v.mid_from[0], ny = v.mid_last[1] - v.mid_from[1],
                    nz = v.mid_last[2] - v.mid_from[2];
        LOG("proxies: slot %u (%s, at %.1f %.1f %.1f, its middle %.1f m from there, %.1f m across): %u moves, %.2f m "
            "in all; its middle %.2f m (%.2f m from where it was), turned %.0f deg (%u times back); Havok's "
            "velocities up to %.1f m/s, %.1f rad/s", v.slot, v.chunk ? "building chunk" : "vehicle", double(v.pos[0]),
            double(v.pos[1]), double(v.pos[2]),
            double(std::sqrt(v.mid[0] * v.mid[0] + v.mid[1] * v.mid[1] + v.mid[2] * v.mid[2])), double(2 * v.half),
            v.targets, double(v.travelled), double(v.mid_path), double(std::sqrt(nx * nx + ny * ny + nz * nz)),
            double(v.turned), v.turned_back, double(v.top_speed), double(v.top_spin));
    }
    if (!busy.empty()) LOG("proxies: %zu of %zu moved in the last 10 s", busy.size(), g_vehicles.size());
    for (VehicleProxy& v : g_vehicles) {
        v.travelled = 0, v.targets = 0;
        v.mid_path = v.turned = v.top_speed = v.top_spin = 0;
        v.turned_back = 0;
        memcpy(v.mid_from, v.mid_last, sizeof(v.mid_from));
    }
}

void reset_vehicles() {  // the level's end (Havok's world still there) and start-up
    for (const VehicleProxy& v : g_vehicles) body_release(v.body);
    g_vehicles.clear();
    g_vehicle_free.clear();
    for (uint32_t s = g_player_slot; s-- > g_vehicle_first;) g_vehicle_free.push_back(s);
    g_next_vehicle_scan = 0;
}

// ---- stand-ins: the pieces as Havok sees them ----

// Pieces in PhysX are invisible to Havok, and so to everything in the game that asks Havok what is where: the
// character controller, melee and bullet traces, projectiles, explosions' line checks, the third-person camera.
// Where it matters a piece gets a stand-in in Havok: a keyframed body with the piece's own collision shape (the
// hkpConvexVerticesShape its general_mover had, kept with a reference), moved with the piece. (The first stand-ins
// were boxes fitted to a piece in its own frame, which for a slanted beam or panel took in empty space around it:
// invisible walls and steps in the air.) Stand-ins carry the zone object, like terrain (zone_object below):
//  - near the player: heavy pieces are solid (Experiment.PhysXSolidDebris). The character controller collides with
//    Havok only, and in the game a body lighter than Character_collision_push_away_mass is pushed out of the
//    player's way while anything else is stood on; so the player stands on heavy rubble, and the player's capsule
//    in the host pushes the light pieces (it does not touch heavy ones: kShapeHeavy / kShapePlayer). Light pieces
//    are ghosts: characters walk through them, melee and bullet traces hit them (the zone object's
//    process_melee_hit and process_bullet_hit, below, pass the blow on to the piece);
//  - ahead of every projectile (rockets, grenades, charges), which then strikes the pile instead of flying through
//    it, and along the player's line of fire (bullets).
// They live in two layers of their own (kLayerSolid, kLayerGhost): the debris layer's row of the layer table without
// the game's own loose bodies (debris, building chunks, ragdolls, props: chunks lying in a pile shot out of it when
// stand-ins appeared inside them as the player came near) and without vehicles (which push the pieces in the host,
// vehicle proxies above); the ghosts' row without characters too.
#pragma pack(push, 4)
struct PhysicsCreateInfo {  // physics_create_info (PDB), the argument of havok_body_create
    uint32_t collision_type;          // +0x00: the collision filter info
    float pos[3];                     // +0x04
    float orient[9];                  // +0x10: rvec, uvec, fvec
    float size[3];                    // +0x34: box: full extents
    float top[3], bottom[3];          // +0x40, +0x4C: capsules
    float radius;                     // +0x58
    float angular_damping, linear_damping, max_angular_vel, max_linear_vel;  // +0x5C
    float mass, friction, restitution;                                      // +0x6C
    void* obj_ptr;                    // +0x78: the game object (property 0); with it, materials go to 3 and 4
    float precalc_tensor[9];          // +0x7C
    float precalc_center_of_mass[3];  // +0xA0
    int32_t flags;                    // +0xAC: 1 precalc tensor, 2 box at local_pos / local_orient
    float local_pos[3];               // +0xB0
    float local_orient[9];            // +0xBC
    uint32_t cm_index;                // +0xE0
    int32_t quality;                  // +0xE4: hkpCollidableQualityType
    int32_t solver_deactivation;      // +0xE8
    int32_t motion_type;              // +0xEC: hkpMotion::MotionType
    void* shape;                      // +0xF0
    void* enter_callback;             // +0xF4
    void* leave_callback;             // +0xF8
    int32_t shape_type;               // +0xFC: havok_shape_type
    uint8_t physical_material, effect_material, auto_remove_level, padding;  // +0x100
};
#pragma pack(pop)
static_assert(sizeof(PhysicsCreateInfo) == 0x104, "physics_create_info");
static_assert(offsetof(PhysicsCreateInfo, obj_ptr) == 0x78 && offsetof(PhysicsCreateInfo, flags) == 0xAC &&
                  offsetof(PhysicsCreateInfo, cm_index) == 0xE0 && offsetof(PhysicsCreateInfo, shape_type) == 0xFC,
              "physics_create_info layout");
constexpr int32_t kMotionKeyframed = 4;          // hkpMotion::MOTION_KEYFRAMED
constexpr int32_t kQualityKeyframed = 1;         // HK_COLLIDABLE_QUALITY_KEYFRAMED
constexpr int32_t kSolverDeactivationLow = 2;    // hkpRigidBodyCinfo::SOLVER_DEACTIVATION_LOW
constexpr int32_t kHavokShapeBox = 3;            // HAVOK_SHAPE_TYPE_BOX
constexpr int32_t kHavokShapeDestroyable = 5;    // HAVOK_SHAPE_TYPE_DESTROYABLE: ci.shape as is, the caller's own
constexpr int32_t kCreateBoxAtLocal = 2;         // physics_create_info::flags
constexpr float kStandInShrink = 0.1f;           // m off each extent of a box: Havok's convex radius sits around it
constexpr float kSolidReach = 2.5f;              // m from the player's middle to a heavy piece's bounds
constexpr float kGhostReach = 2.0f;              // to a light piece's bounds (the hammer's reach and then some)
constexpr float kProjectileAhead = 0.15f;        // s of a projectile's flight covered ahead of it
constexpr float kProjectileMargin = 0.5f;        // m around its path
constexpr float kAimRange = 80.0f, kAimMargin = 0.2f;  // the player's line of fire, from the camera
constexpr size_t kAimMax = 16;                   // its pieces nearest the camera (a bullet stops at the first)
constexpr size_t kMaxStandIns = 384, kCreatePerFrame = 64;
constexpr uint32_t kLayerSolid = 100, kLayerGhost = 101;  // unused by the game, whose layers end at 63
// world: subtype_objects[OT_SUB_ITEM_PROJECTILE] (base_array<ushort>: elt +4, num +0xC), the live projectiles.
constexpr uint32_t kWorldProjectiles = 0x3C0 + 8 * 0x10 + 4, kWorldProjectileCount = 0x3C0 + 8 * 0x10 + 0xC;
std::vector<uint32_t> g_stand_in_slots;          // pieces with a stand-in (may repeat; stand_in is the truth)
uint32_t g_stand_in_pass = 0, g_stand_ins_live = 0;
uint32_t g_stand_ins_created = 0, g_stand_in_failures = 0, g_stand_ins_peak = 0, g_stand_ins_boxes = 0;
uint32_t g_wanted_near = 0, g_wanted_projectile = 0, g_wanted_aim = 0;  // stand-ins wanted, by reason (for the log)

void rows_from_quat(const ipc::Quat& q, float rot[9]) {  // as move_instance: rvec, uvec, fvec
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    rot[0] = 1 - 2 * (y * y + z * z), rot[1] = 2 * (x * y + z * w), rot[2] = 2 * (x * z - y * w);
    rot[3] = 2 * (x * y - z * w), rot[4] = 1 - 2 * (x * x + z * z), rot[5] = 2 * (y * z + x * w);
    rot[6] = 2 * (x * z + y * w), rot[7] = 2 * (y * z - x * w), rot[8] = 1 - 2 * (x * x + y * y);
}

void free_stand_in(uint32_t handle) {
    // As item::disable_physics: remove from the world now, then release.
    rfg::fn<void(__cdecl*)(uint32_t, bool, bool, bool)>(r::havok_body_free)(handle, false, true, false);
    --g_stand_ins_live;
}

// The stand-ins carry a zone object (OT_ZONE). Everything in the game expects an object behind a body (vehicle
// wheel casts read its type without a check), and terrain carries its zone's object, so stand-ins read as level
// geometry to the character controller, wheels, raycasts and explosions. The object of the zone the player is in;
// a zone streaming out frees the stand-ins first if any carries its object (d_destream_zone).
constexpr int8_t kObjTypeZone = 19;  // OT_ZONE
uint8_t* g_zone_obj = nullptr;            // the last one handed out
uint32_t g_zone_changes = 0;
std::vector<uint8_t*> g_stand_in_zones;  // zone objects carried by stand-ins since they were last all freed
// Where each of those was when a stand-in first carried it: nothing may move it (see create_stand_in).
struct CarriedZone {
    const uint8_t* zone;
    float pos[3];
};
std::vector<CarriedZone> g_carried_zones;
uint32_t g_zone_moved_logs = 0;
bool g_stand_in_drives_zone = false;  // Test.StandInDrivesZone

uint8_t* zone_object() {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return nullptr;
    uint8_t* zone = rfg::fn<uint8_t*(__thiscall*)(void*, const float*)>(r::world__get_zone_by_pos)(
        world(), reinterpret_cast<const float*>(player + 0x04));
    if (!zone || int8_t(zone[kObjType]) != kObjTypeZone) return nullptr;
    if (zone != g_zone_obj) {
        if (!g_zone_obj)
            LOG("debris offload: stand-ins carry the object of the zone the player is in (%p, handle %08X)",
                static_cast<void*>(zone), *reinterpret_cast<const uint32_t*>(zone + kObjHandle));
        g_zone_obj = zone;
        ++g_zone_changes;
    }
    return zone;
}

void create_stand_in(uint32_t slot, const float pos[3], const float rot[9]) {
    Piece& p = g_pieces[slot];
    uint8_t* zone = zone_object();
    if (!zone) {
        ++g_stand_in_failures;
        return;
    }
    PhysicsCreateInfo ci{};
    ci.obj_ptr = zone;
    ci.physical_material = p.physical_material;
    ci.effect_material = p.effect_material;
    ci.collision_type = p.heavy && g_solid ? kLayerSolid : kLayerGhost;  // system group 0: the layer table decides
    memcpy(ci.pos, pos, sizeof(ci.pos));
    memcpy(ci.orient, rot, sizeof(ci.orient));
    ci.max_angular_vel = 200.0f, ci.max_linear_vel = 200.0f;
    ci.mass = p.mass > 0 ? p.mass : 100.0f, ci.friction = 0.6f, ci.restitution = 0.1f;
    ci.quality = kQualityKeyframed;
    ci.solver_deactivation = kSolverDeactivationLow;
    ci.motion_type = kMotionKeyframed;
    if (p.shape) {
        ci.shape = const_cast<uint8_t*>(p.shape);
        ci.shape_type = kHavokShapeDestroyable;
    } else {
        for (int k = 0; k < 3; ++k) ci.size[k] = std::max(p.box_size[k] - kStandInShrink, 0.05f);
        ci.flags = kCreateBoxAtLocal;
        memcpy(ci.local_pos, p.box_c, sizeof(ci.local_pos));
        ci.local_orient[0] = ci.local_orient[4] = ci.local_orient[8] = 1.0f;
        ci.shape_type = kHavokShapeBox;
        ++g_stand_ins_boxes;
    }
    const uint32_t handle = rfg::fn<uint32_t(__cdecl*)(PhysicsCreateInfo*)>(r::havok_body_create)(&ci);
    if (handle == kNoStandIn) {
        ++g_stand_in_failures;
        return;
    }
    // Property 7, as a walker's limbs have it: the body does not drive its object. Each step the game sets the object
    // of every active body to the body's pose (havok_update_pos_and_orient), and stand-ins carry the zone object: the
    // zone's minimap tile (obj_zone::get_bbox, its position plus its extents) went wherever a stand-in was (players:
    // the minimap shaking, black voids and seams in it, the sky changing across them).
    if (!g_stand_in_drives_zone)
        if (void* body = static_cast<void* const*>(rfg::addr(r::Hk_rigid_bodies))[handle])
            rfg::fn<void(__cdecl*)(void*, uint32_t, void*)>(r::havok_set_property)(body, 7, reinterpret_cast<void*>(1));
    p.stand_in = handle;
    memcpy(p.si_pos, pos, sizeof(p.si_pos));
    g_stand_in_slots.push_back(slot);
    if (std::find(g_stand_in_zones.begin(), g_stand_in_zones.end(), zone) == g_stand_in_zones.end()) {
        g_stand_in_zones.push_back(zone);
        CarriedZone c{zone, {}};
        memcpy(c.pos, zone + 0x04, sizeof(c.pos));
        g_carried_zones.push_back(c);
    }
    ++g_stand_ins_created;
    g_stand_ins_peak = std::max(g_stand_ins_peak, ++g_stand_ins_live);
}

// The stand-ins' layers in the game's layer table (havok_game_filter: bit a * 128 + b, kept symmetric), made from
// the debris layer's row (as Experiment.DebrisCollide left it). Done again whenever the table lacks them (unused
// layers collide with everything: bit kLayerSolid x kLayerSolid set means not done).
bool g_layers_logged = false;
void ensure_stand_in_layers() {
    const uint8_t* filter = rfg::global<uint8_t*>(r::Havok_filter);
    uint32_t* table = filter ? *reinterpret_cast<uint32_t* const*>(filter + 0x34) : nullptr;
    if (!table || !layer_bit(kLayerSolid, kLayerSolid)) return;
    auto set_pair = [table](uint32_t a, uint32_t b, bool on) {
        for (const uint32_t bit : {a * 128 + b, b * 128 + a}) {
            if (on) table[bit >> 5] |= 1u << (bit & 31);
            else table[bit >> 5] &= ~(1u << (bit & 31));
        }
    };
    // Never: the game's loose bodies (movers 3: building chunks, items 4, ragdolls 5, keyframed 36, physical objects
    // 38, debris 39-44, stress 46), vehicles (9, 12 walkers, 14 walker hulls, 15 flyers, 57 wheel rays; 10 and 13,
    // which no collision type names and the vehicles' rows have), rays after destroyable geometry (45: the code behind
    // them expects a building), the stand-ins themselves.
    static const uint32_t kNever[] = {3, 4, 5, 9, 10, 12, 13, 14, 15, 36, 38, 39, 40, 41, 42, 43, 44, 45, 46, 57,
                                      kLayerSolid, kLayerGhost};
    // Ghosts also not: characters (6 player, 7 NPCs, 50 on ladders, 52, 63) and walking-surface rays (29).
    static const uint32_t kGhostNever[] = {6, 7, 29, 50, 52, 63};
    char solid[400], ghost[400];
    int ns = 0, ng = 0;
    for (uint32_t b = 0; b < 128; ++b) {
        bool on = b < 64 && layer_bit(kLayerDebris, b);
        for (uint32_t n : kNever) on = on && b != n;
        bool ghost_on = on;
        for (uint32_t n : kGhostNever) ghost_on = ghost_on && b != n;
        set_pair(kLayerSolid, b, on);
        set_pair(kLayerGhost, b, ghost_on);
        if (on && ns < 380) ns += snprintf(solid + ns, sizeof(solid) - ns, " %u", b);
        if (ghost_on && ng < 380) ng += snprintf(ghost + ng, sizeof(ghost) - ng, " %u", b);
    }
    if (!g_layers_logged) {
        g_layers_logged = true;
        LOG("debris offload: stand-in layers: solid %u collides with%s; ghost %u with%s", kLayerSolid, solid,
            kLayerGhost, ghost);
    }
}

float distance_to_segment(const ipc::Vec3& p, const float a[3], const float d[3], float len2, float* along) {
    const float px = p.x - a[0], py = p.y - a[1], pz = p.z - a[2];
    float t = len2 > 0 ? (px * d[0] + py * d[1] + pz * d[2]) / len2 : 0.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float ex = px - d[0] * t, ey = py - d[1] * t, ez = pz - d[2] * t;
    if (along) *along = t * std::sqrt(len2);
    return std::sqrt(ex * ex + ey * ey + ez * ez);
}

struct Segment {
    float a[3], d[3];  // from a to a + d
    float len2, margin;
    float lo[3], hi[3];  // its bounds with the margin and the largest piece radius
};

// Sim thread, end of frame (Havok is not stepping): the pieces that want a stand-in now, by priority: heavy pieces
// near the player, then pieces ahead of projectiles, light pieces near the player, the line of fire. At most
// kCreatePerFrame are made in a frame; the others wait for the next.
void update_stand_ins() {
    for (uint32_t h : g_stand_ins_to_free) free_stand_in(h);
    g_stand_ins_to_free.clear();
    ++g_stand_in_pass;
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    static std::vector<uint32_t> wanted;
    wanted.clear();
    if (s && player) {
        ensure_stand_in_layers();
        const float* pp = reinterpret_cast<const float*>(player + 0x04);
        const float px = pp[0], py = pp[1] + 0.9f, pz = pp[2];
        const ipc::BodyPose* poses = pxbridge::poses(s);
        const uint32_t slots = std::min<uint32_t>(s->slots, g_vehicle_first);
        // The paths: each projectile's next kProjectileAhead s, and the line of fire from the camera.
        static std::vector<Segment> paths;
        paths.clear();
        const float largest = g_largest_radius;
        auto add_path = [&](const float a[3], const float d[3], float margin) {
            Segment g{};
            for (int k = 0; k < 3; ++k) {
                g.a[k] = a[k], g.d[k] = d[k];
                g.lo[k] = std::min(a[k], a[k] + d[k]) - margin - largest;
                g.hi[k] = std::max(a[k], a[k] + d[k]) + margin + largest;
            }
            g.len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
            g.margin = margin;
            paths.push_back(g);
        };
        uint8_t* w = world();
        uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
        const uint16_t* projectiles = *reinterpret_cast<const uint16_t* const*>(w + kWorldProjectiles);
        const int np = *reinterpret_cast<const int*>(w + kWorldProjectileCount);
        uint8_t* const* bodies = static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies));
        for (int i = 0; all && projectiles && i < np && i < 64; ++i) {
            const uint8_t* obj = all[projectiles[i]];
            if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
            const uint8_t* body = handle == 0xFFFFFFFFu ? nullptr : bodies[handle];
            const float* pos = reinterpret_cast<const float*>(obj + 0x04);
            float d[3] = {0, 0, 0};
            if (body) {
                const float* v = reinterpret_cast<const float*>(body + kBodyLinVel);
                for (int k = 0; k < 3; ++k) d[k] = std::isfinite(v[k]) ? v[k] * kProjectileAhead : 0.0f;
            }
            add_path(pos, d, kProjectileMargin);
        }
        const size_t projectile_paths = paths.size();
        {
            const uint8_t* cam = static_cast<const uint8_t*>(rfg::addr(r::Rfg_camera));
            const float* cam_pos = reinterpret_cast<const float*>(cam + 0x2C);   // real_pos
            const float* fvec = reinterpret_cast<const float*>(cam + 0x50 + 0x18);  // real_orient.fvec
            const float d[3] = {fvec[0] * kAimRange, fvec[1] * kAimRange, fvec[2] * kAimRange};
            add_path(cam_pos, d, kAimMargin);
        }
        // Keys: the reason (0 heavy near the player, 1 ahead of a projectile, 2 light near the player, 3 the line of
        // fire) * 1000 m + the distance.
        auto want = [&](uint32_t i, float key) {
            Piece& p = g_pieces[i];
            if (p.wanted != g_stand_in_pass) {
                p.wanted = g_stand_in_pass;
                p.want_key = key;
                wanted.push_back(i);
            } else {
                p.want_key = std::min(p.want_key, key);
            }
        };
        static std::vector<std::pair<float, uint32_t>> aim_hits;  // (distance along the line of fire, slot)
        aim_hits.clear();
        const uint32_t scan = std::min<uint32_t>(slots, uint32_t(g_spatial.size()));
        constexpr float kMostReach = std::max(kSolidReach, kGhostReach);
        for (uint32_t i = 0; i < scan; ++i) {
            const Spatial& sp = g_spatial[i];
            if (sp.r < 0) continue;
            const float dx = sp.x - px, dy = sp.y - py, dz = sp.z - pz;
            const float d2 = dx * dx + dy * dy + dz * dz;
            const float most = kMostReach + sp.r;
            if (d2 < most * most) {
                const bool solid = g_pieces[i].heavy && g_solid;
                const float reach = (solid ? kSolidReach : kGhostReach) + sp.r;
                if (d2 < reach * reach) {
                    want(i, (solid ? 0.0f : 2000.0f) + std::sqrt(d2));
                    ++g_wanted_near;
                }
            }
            for (size_t k = 0; k < paths.size(); ++k) {
                const Segment& g = paths[k];
                if (sp.x < g.lo[0] || sp.x > g.hi[0] || sp.y < g.lo[1] || sp.y > g.hi[1] || sp.z < g.lo[2] ||
                    sp.z > g.hi[2])
                    continue;
                float along = 0;
                if (distance_to_segment(ipc::Vec3{sp.x, sp.y, sp.z}, g.a, g.d, g.len2, &along) > g.margin + sp.r)
                    continue;
                if (k < projectile_paths) {
                    want(i, 1000.0f + along);
                    ++g_wanted_projectile;
                } else {
                    aim_hits.emplace_back(along, i);
                }
            }
        }
        if (aim_hits.size() > kAimMax) {
            std::nth_element(aim_hits.begin(), aim_hits.begin() + kAimMax, aim_hits.end());
            aim_hits.resize(kAimMax);
        }
        for (const auto& h : aim_hits) {
            want(h.second, 3000.0f + h.first);
            ++g_wanted_aim;
        }
        if (wanted.size() > kMaxStandIns) {
            std::nth_element(wanted.begin(), wanted.begin() + kMaxStandIns, wanted.end(),
                             [](uint32_t a, uint32_t b) { return g_pieces[a].want_key < g_pieces[b].want_key; });
            for (size_t k = kMaxStandIns; k < wanted.size(); ++k) g_pieces[wanted[k]].wanted = 0;  // not this time
            wanted.resize(kMaxStandIns);
        }
        std::sort(wanted.begin(), wanted.end(),
                  [](uint32_t a, uint32_t b) { return g_pieces[a].want_key < g_pieces[b].want_key; });
        const auto set_pose = rfg::fn<void(__cdecl*)(uint32_t, const float*, const float*)>(
            r::havok_body_set_pos_orient_by_handle);
        size_t created = 0;
        for (uint32_t i : wanted) {
            Piece& p = g_pieces[i];
            const ipc::BodyPose& b = poses[i];
            const float pos[3] = {b.pos.x, b.pos.y, b.pos.z};
            const float q[4] = {b.rot.x, b.rot.y, b.rot.z, b.rot.w};
            if (p.stand_in != kNoStandIn) {
                const float mx = pos[0] - p.si_pos[0], my = pos[1] - p.si_pos[1], mz = pos[2] - p.si_pos[2];
                const float same = std::fabs(q[0] * p.si_q[0] + q[1] * p.si_q[1] + q[2] * p.si_q[2] + q[3] * p.si_q[3]);
                if (mx * mx + my * my + mz * mz < 1e-6f && same > 0.9999999f) continue;  // where it was put
                float rot[9];
                rows_from_quat(b.rot, rot);
                set_pose(p.stand_in, pos, rot);
            } else {
                if (created >= kCreatePerFrame) {
                    p.wanted = 0;  // the next frame
                    continue;
                }
                float rot[9];
                rows_from_quat(b.rot, rot);
                create_stand_in(i, pos, rot);
                ++created;
                if (p.stand_in == kNoStandIn) continue;
            }
            memcpy(p.si_pos, pos, sizeof(pos));
            memcpy(p.si_q, q, sizeof(q));
        }
    }
    size_t keep = 0;
    for (uint32_t i : g_stand_in_slots) {
        Piece& p = g_pieces[i];
        if (p.stand_in == kNoStandIn) continue;
        if (p.inst && p.wanted == g_stand_in_pass) {
            g_stand_in_slots[keep++] = i;
            continue;
        }
        free_stand_in(p.stand_in);
        p.stand_in = kNoStandIn;
    }
    g_stand_in_slots.resize(keep);
    for (const CarriedZone& c : g_carried_zones) {
        const float* now = reinterpret_cast<const float*>(c.zone + 0x04);
        const float dx = now[0] - c.pos[0], dy = now[1] - c.pos[1], dz = now[2] - c.pos[2];
        if (dx * dx + dy * dy + dz * dz > 1e-4f && g_zone_moved_logs < 5) {
            ++g_zone_moved_logs;
            LOG("debris offload: WARNING the zone object stand-ins carry (%p) moved %.2f m, from (%.1f, %.1f, %.1f) to "
                "(%.1f, %.1f, %.1f)", static_cast<const void*>(c.zone), double(std::sqrt(dx * dx + dy * dy + dz * dz)),
                double(c.pos[0]), double(c.pos[1]), double(c.pos[2]), double(now[0]), double(now[1]), double(now[2]));
        }
    }
    if (g_stand_in_slots.empty() && g_stand_ins_to_free.empty()) {
        g_stand_in_zones.clear();
        g_carried_zones.clear();
    }
    g_stats.stand_ins = g_stand_ins_live;
}

// ---- blows on the pieces ----

// A melee blow or a bullet on a stand-in lands on the zone object it carries, whose process_melee_hit and
// process_bullet_hit (obj_zone's vtable, slots 27 and 26) do nothing. Both are wrapped: a hit within a stand-in's
// piece pushes the piece in the host the way the game pushes a general mover (object_mover::process_melee_hit: the
// blow's impulse at its point; object_mover::process_bullet_hit: -hit normal * damage * Bullet_movable_scalar at the
// hit), and wakes it if it was frozen. Before, the hammer and bullets did nothing to the pieces.
constexpr uint32_t kZoneSlotBullet = 26, kZoneSlotMelee = 27;
constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
constexpr float kHitSlack = 0.08f;  // m around a piece's bounds: Havok's convex radius around its shape
using ZoneMelee = bool(__fastcall*)(void*, void*, void*, float, const float*, const float*, const float*, uint32_t, void*);
using ZoneBullet = void(__fastcall*)(void*, void*, void*, float, const uint8_t*);
ZoneMelee o_zone_melee = nullptr;
ZoneBullet o_zone_bullet = nullptr;
uint32_t g_melee_hits = 0, g_bullet_hits = 0, g_hits_logged = 0;

// The piece with a stand-in whose bounds, in its own frame where its stand-in is, hold `point` (with `slack` around
// them); the one whose middle is nearest if several do.
uint32_t stand_in_piece_at(const float* point, float slack = kHitSlack) {
    uint32_t best = kNoSlot;
    float best_d2 = 1e30f;
    for (uint32_t i : g_stand_in_slots) {
        const Piece& p = g_pieces[i];
        if (p.stand_in == kNoStandIn || !p.inst) continue;
        float rot[9];
        rows_from_quat({p.si_q[0], p.si_q[1], p.si_q[2], p.si_q[3]}, rot);
        const float v[3] = {point[0] - p.si_pos[0], point[1] - p.si_pos[1], point[2] - p.si_pos[2]};
        float d2 = 0;
        bool inside = true;
        for (int k = 0; k < 3 && inside; ++k) {
            const float local = v[0] * rot[k * 3] + v[1] * rot[k * 3 + 1] + v[2] * rot[k * 3 + 2];
            const float off = local - p.box_c[k];
            inside = std::fabs(off) <= 0.5f * p.box_size[k] + slack;
            d2 += off * off;
        }
        if (inside && d2 < best_d2) best_d2 = d2, best = i;
    }
    return best;
}

// The most a blow speeds a piece up, as Havok caps a body's speed: a mod's melee moves push with 40,000 N*s, which
// sent a 2 kg piece off at 20 km/s.
constexpr float kBlowMaxSpeed = 40.0f;  // m/s

void push_piece(uint32_t slot, const float j[3], const float at[3], const char* what) {
    if (!(std::isfinite(j[0]) && std::isfinite(j[1]) && std::isfinite(j[2]))) return;
    const float mass = std::max(g_pieces[slot].mass, 1.0f);
    const float speed = std::sqrt(j[0] * j[0] + j[1] * j[1] + j[2] * j[2]) / mass;
    const float scale = speed > kBlowMaxSpeed ? kBlowMaxSpeed / speed : 1.0f;
    ipc::Impulse c{};
    c.h = {uint16_t(ipc::Cmd::Impulse), 0, sizeof(c)};
    c.slot = slot;
    c.mode = ipc::kExplosionImpulse;
    c.impulse = {j[0] * scale, j[1] * scale, j[2] * scale};
    c.point = {at[0], at[1], at[2]};
    if (!pxbridge::send_record(&c, sizeof(c))) return;
    if (g_hits_logged < 12) {
        ++g_hits_logged;
        const float mag = std::sqrt(j[0] * j[0] + j[1] * j[1] + j[2] * j[2]);
        LOG("debris offload: %s on piece %u (%.0f kg): impulse %.0f N*s, %.2f m/s", what, slot,
            double(g_pieces[slot].mass), double(mag), double(mag / std::max(g_pieces[slot].mass, 1.0f)));
    }
}

thread_local bool g_in_wide_blow = false;  // within a blow passed on whole (below): its stand-in hits are part of it

bool __fastcall d_zone_melee(void* self, void* edx, void* attacker, float damage, const float* impulse,
                             const float* pos, const float* c, uint32_t u, void* weapon) {
    const bool result = o_zone_melee(self, edx, attacker, damage, impulse, pos, c, u, weapon);
    if (g_on && impulse && pos && !g_in_wide_blow) {
        const uint32_t slot = stand_in_piece_at(pos);
        if (slot != kNoSlot) {
            push_piece(slot, impulse, pos, "melee blow");
            ++g_melee_hits;
        }
    }
    return result;
}

void __fastcall d_zone_bullet(void* self, void* edx, void* shooter, float damage, const uint8_t* info) {
    o_zone_bullet(self, edx, shooter, damage, info);
    if (!g_on || !info) return;
    const float* hit_pos = reinterpret_cast<const float*>(info + 0x1C);     // weapon_fire_info::hit_pos
    const float* hit_normal = reinterpret_cast<const float*>(info + 0x28);  // hit_normal
    const uint32_t slot = stand_in_piece_at(hit_pos);
    if (slot == kNoSlot) return;
    const float k = damage * rfg::global<float>(r::Bullet_movable_scalar);
    const float j[3] = {-hit_normal[0] * k, -hit_normal[1] * k, -hit_normal[2] * k};
    push_piece(slot, j, hit_pos, "bullet");
    ++g_bullet_hits;
}

// A melee blow sweeps a sphere, the move's cast_radius, along the path of its attack points, one stretch a frame
// (process_continuous_attack_point -> human_combat_process_hits), and pushes each object it hits once:
// impact_impulse * (the blow's direction + the attacker's forward vector * away_impulse_scale)
// (human_combat_unified_melee_hit). The game's moves sweep 0.4 m and reach the pieces through their stand-ins. A mod
// that enlarges them (Sledge's Sledgehammer Shockwave: up to 35 m, and 6 x 6,000 N*s) reaches far past the stand-ins,
// so a wide blow goes to the host whole (ipc::MeleeSweep): every piece the sphere sweeps gets the push once (the host
// knows the blow by its id), at most kBlowMaxSpeed, and the stand-ins' own hits are left out. A blow is its attacker's
// calls with one move, under kBlowGapMs apart; of its stretches only the first and those reaching kBlowNewReach of the
// sphere past the last one sent go, so a swing is one or two sweeps, not one a frame. Queued, as the explosions are,
// for the sim thread to send.
constexpr float kWideBlowRadius = 1.0f;  // m: a sphere wider than any of the game's own moves
constexpr uint32_t kBlowGapMs = 250;
constexpr float kBlowNewReach = 0.25f;
constexpr uint32_t kHumanMeleeMove = 0x97C;  // human::combat.cur_melee_combat_move (human_combat at +0x928, +0x54)
constexpr uint32_t kObjForward = 0x28;       // object::orient.fvec
// human_melee_combat_move_data
constexpr uint32_t kMoveCastRadius = 0x14, kMoveImpactImpulse = 0x28, kMoveAwayScale = 0x30;
struct Blow {
    const void* attacker;
    const void* move;
    uint32_t id;
    uint32_t last_ms;
    float sent_end[3];  // where the last sweep sent ended
    float a[3], b[3];   // its first stretch
    float radius;
    float impulse[3];
};
CRITICAL_SECTION g_blow_lock;
std::vector<Blow> g_blows;  // the attackers' current blows (a few)
std::vector<ipc::MeleeSweep> g_sweeps_pending;
uint32_t g_next_blow_id = 1, g_wide_blows = 0, g_sweeps_sent = 0;

// A wide blow lingers kBlowLingerMs past its last stretch. The pieces of what it shattered come out of the
// destruction a step or more later (rfg_dyn_do_collide_post, inside the next Havok steps), after the game's sphere
// cast is done, so they fell where they broke off and only the next blow threw them; a piece that appears inside the
// blow's sphere meanwhile gets the blow's push in Havok, as the objects it hit did.
constexpr uint32_t kBlowLingerMs = 1000;
uint32_t g_late_pushes = 0;

void push_new_piece_by_blow(const uint8_t* body, uint32_t handle, uint32_t now_ms) {
    const hs::Transform t = hs::body_transform(body);
    float j[3];
    bool inside = false;
    EnterCriticalSection(&g_blow_lock);
    for (const Blow& w : g_blows) {
        if (now_ms - w.last_ms > kBlowLingerMs) continue;
        const float ab[3] = {w.b[0] - w.a[0], w.b[1] - w.a[1], w.b[2] - w.a[2]};
        const float ap[3] = {t.pos[0] - w.a[0], t.pos[1] - w.a[1], t.pos[2] - w.a[2]};
        const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
        float s = len2 > 0.0f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f;
        s = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s;
        const float d[3] = {ap[0] - ab[0] * s, ap[1] - ab[1] * s, ap[2] - ab[2] * s};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > w.radius * w.radius) continue;
        memcpy(j, w.impulse, sizeof(j));
        inside = true;
        break;
    }
    LeaveCriticalSection(&g_blow_lock);
    if (!inside) return;
    const float speed = std::sqrt(j[0] * j[0] + j[1] * j[1] + j[2] * j[2]) * *reinterpret_cast<const float*>(body + kBodyInvMass);
    if (speed > kBlowMaxSpeed)
        for (float& v : j) v *= kBlowMaxSpeed / speed;
    rfg::fn<void(__cdecl*)(uint32_t, const float*)>(r::havok_body_apply_linear_impulse_by_handle)(handle, j);
    ++g_late_pushes;
}

void(__cdecl* o_combat_hits)(void*, void*, bool) = nullptr;
void __cdecl d_combat_hits(void* human, void* input, bool secondary) {
    const auto* h = static_cast<const uint8_t*>(human);
    const uint8_t* move = h ? *reinterpret_cast<const uint8_t* const*>(h + kHumanMeleeMove) : nullptr;
    const float radius = move ? *reinterpret_cast<const float*>(move + kMoveCastRadius) : 0.0f;
    if (!g_on || !input || !(radius >= kWideBlowRadius) || radius > 500.0f) {
        o_combat_hits(human, input, secondary);
        return;
    }
    // raycast_input: start at +0, end at +0xC (the attack point a frame ago and now).
    float a[3], b[3];
    memcpy(a, input, sizeof(a));
    memcpy(b, static_cast<const uint8_t*>(input) + 0xC, sizeof(b));
    g_in_wide_blow = true;
    o_combat_hits(human, input, secondary);
    g_in_wide_blow = false;

    float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (float& v : d) v = len > 1e-4f ? v / len : 0.0f;
    const float* fwd = reinterpret_cast<const float*>(h + kObjForward);
    const float away = *reinterpret_cast<const float*>(move + kMoveAwayScale);
    const float impact = *reinterpret_cast<const float*>(move + kMoveImpactImpulse);
    ipc::MeleeSweep s{};
    s.h = {uint16_t(ipc::Cmd::MeleeSweep), 0, sizeof(s)};
    s.start = {a[0], a[1], a[2]};
    s.end = {b[0], b[1], b[2]};
    s.radius = radius;
    s.max_speed = kBlowMaxSpeed;
    s.impulse = {(d[0] + fwd[0] * away) * impact, (d[1] + fwd[1] * away) * impact, (d[2] + fwd[2] * away) * impact};
    if (!(std::isfinite(s.impulse.x) && std::isfinite(s.impulse.y) && std::isfinite(s.impulse.z))) return;

    const uint32_t now_ms = rfg::global<uint32_t>(r::Frametime_total_milliseconds);
    EnterCriticalSection(&g_blow_lock);
    auto it = std::find_if(g_blows.begin(), g_blows.end(), [&](const Blow& w) { return w.attacker == human; });
    bool send = true;
    if (it == g_blows.end() || it->move != move || now_ms - it->last_ms > kBlowGapMs) {
        if (it == g_blows.end()) {
            if (g_blows.size() >= 64) g_blows.erase(g_blows.begin());
            it = g_blows.insert(g_blows.end(), Blow{});
        }
        *it = Blow{human, move, g_next_blow_id++, now_ms, {b[0], b[1], b[2]}, {a[0], a[1], a[2]}, {b[0], b[1], b[2]},
                   radius, {s.impulse.x, s.impulse.y, s.impulse.z}};
        ++g_wide_blows;
        harness::shot_series();  // Harness.ShotsAfterBlow
    } else {
        const float dx = b[0] - it->sent_end[0], dy = b[1] - it->sent_end[1], dz = b[2] - it->sent_end[2];
        send = dx * dx + dy * dy + dz * dz > (kBlowNewReach * radius) * (kBlowNewReach * radius);
        it->last_ms = now_ms;
        if (send) memcpy(it->sent_end, b, sizeof(b));
    }
    s.blow = it->id;
    if (send && g_sweeps_pending.size() < 256) g_sweeps_pending.push_back(s);
    LeaveCriticalSection(&g_blow_lock);
}

void send_melee_sweeps() {
    static std::vector<ipc::MeleeSweep> batch;
    EnterCriticalSection(&g_blow_lock);
    batch.swap(g_sweeps_pending);
    LeaveCriticalSection(&g_blow_lock);
    for (const ipc::MeleeSweep& s : batch)
        if (pxbridge::send_record(&s, sizeof(s))) ++g_sweeps_sent;
    batch.clear();
}

bool patch_zone_hits() {
    void** vt = static_cast<void**>(rfg::addr(r::obj_zone_vftable));
    if (vt[kZoneSlotMelee] != rfg::addr(r::obj_zone__process_melee_hit) ||
        vt[kZoneSlotBullet] != rfg::addr(r::object__process_bullet_hit))
        return false;
    DWORD old;
    if (!VirtualProtect(&vt[kZoneSlotBullet], 2 * sizeof(void*), PAGE_READWRITE, &old)) return false;
    o_zone_bullet = reinterpret_cast<ZoneBullet>(vt[kZoneSlotBullet]);
    o_zone_melee = reinterpret_cast<ZoneMelee>(vt[kZoneSlotMelee]);
    vt[kZoneSlotBullet] = reinterpret_cast<void*>(&d_zone_bullet);
    vt[kZoneSlotMelee] = reinterpret_cast<void*>(&d_zone_melee);
    VirtualProtect(&vt[kZoneSlotBullet], 2 * sizeof(void*), old, &old);
    return true;
}

// Test.StandInProbe (unattended runs): every 2 s, a piece with a stand-in is looked for by the game's own query (a
// ray with the default filter, as melee and bullets use, straight down through the middle of its bounds) and given a
// blow through the zone object's process_melee_hit (3 m/s sideways); 0.6 s later its pose in the host says whether
// it moved. The log tallies both.
bool g_probe = false;
int g_probe_stage = 0;
uint32_t g_probe_slot = kNoSlot;
double g_probe_at = 0;
float g_probe_pos[3] = {};
uint32_t g_probe_rays = 0, g_probe_ray_hits = 0, g_probe_blows = 0, g_probe_moved = 0;

void piece_middle(const Piece& p, float out[3]) {  // its bounds' middle, in the world, where its stand-in is
    float rot[9];
    rows_from_quat({p.si_q[0], p.si_q[1], p.si_q[2], p.si_q[3]}, rot);
    for (int k = 0; k < 3; ++k)
        out[k] = p.si_pos[k] + p.box_c[0] * rot[k] + p.box_c[1] * rot[3 + k] + p.box_c[2] * rot[6 + k];
}

// The nearest Havok body a ray from `a` to `b` hits (the default filter, or Havok's static level only): its handle
// (kNoSlot if it has none or nothing is hit), the hits and the nearest's fraction for the log.
struct RayResult {
    uint32_t handle = kNoSlot;
    int hits = 0;
    float fraction = -1.0f;
    uint32_t object = 0xFFFFFFFFu;
};
RayResult probe_ray(const float a[3], const float b[3], bool static_level = false) {
    alignas(16) uint8_t in[0x30] = {};
    rfg::fn<void(__fastcall*)(void*, void*)>(r::raycast_input__raycast_input)(in, nullptr);
    memcpy(in, a, 12);
    memcpy(in + 0xC, b, 12);
    alignas(16) uint8_t out[0x10] = {};
    rfg::fn<void(__fastcall*)(void*, void*)>(r::raycast_output__raycast_output)(out, nullptr);
    const uint32_t cast = static_level ? r::havok_raycast_against_static_level_only : r::havok_raycast;
    RayResult res;
    if (rfg::fn<bool(__cdecl*)(void*, void*)>(cast)(in, out)) {
        const uint8_t* hits = *reinterpret_cast<const uint8_t* const*>(out);
        res.hits = *reinterpret_cast<const int*>(out + 4);
        float best = 2.0f;
        for (int i = 0; hits && i < res.hits; ++i) {
            const uint8_t* h = hits + i * 0x38;
            const float fraction = *reinterpret_cast<const float*>(h + 0x24);
            if (fraction >= best) continue;
            best = res.fraction = fraction;
            res.handle = *reinterpret_cast<const uint32_t*>(h + 0x30);
            res.object = *reinterpret_cast<const uint32_t*>(h + 0x28);
        }
    }
    rfg::fn<void(__fastcall*)(void*, void*)>(r::raycast_output___raycast_output)(out, nullptr);
    return res;
}

void probe_frame() {
    const double now = session::since_start_ms();
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    if (!s) return;
    if (g_probe_stage == 1) {
        if (now < g_probe_at + 600) return;
        const Piece& p = g_pieces[g_probe_slot];
        if (p.inst) {
            const ipc::BodyPose& b = pxbridge::poses(s)[g_probe_slot];
            const float dx = b.pos.x - g_probe_pos[0], dy = b.pos.y - g_probe_pos[1], dz = b.pos.z - g_probe_pos[2];
            const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (moved > 0.05f) ++g_probe_moved;
            LOG("stand-in probe: piece %u moved %.2f m after the blow", g_probe_slot, double(moved));
        }
        g_probe_stage = 0;
        g_probe_at = now + 1400;
        return;
    }
    if (now < g_probe_at || !g_zone_obj) return;
    for (uint32_t i : g_stand_in_slots) {
        Piece& p = g_pieces[i];
        if (p.stand_in == kNoStandIn || !p.inst || p.radius < 0.2f) continue;
        float mid[3];
        piece_middle(p, mid);
        const float top[3] = {mid[0], mid[1] + 3.0f, mid[2]}, bottom[3] = {mid[0], mid[1] - 3.0f, mid[2]};
        const RayResult ray = probe_ray(top, bottom), level = probe_ray(top, bottom, true);
        ++g_probe_rays;
        // A stand-in hit names the zone object the stand-ins carry and no body handle (the game sets those for its
        // own bodies only); terrain also carries the zone object, so the hit must also lie at the piece.
        const uint32_t zone_handle = *reinterpret_cast<const uint32_t*>(g_zone_obj + kObjHandle);
        const float hit_y = top[1] - 6.0f * ray.fraction;
        const bool on_stand_in = ray.hits > 0 && ray.object == zone_handle && ray.handle == kNoSlot &&
                                 hit_y > mid[1] - p.radius - 0.1f && hit_y < mid[1] + p.radius + 0.1f;
        if (on_stand_in) ++g_probe_ray_hits;
        uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
        const float* pp = player ? reinterpret_cast<const float*>(player + 0x04) : mid;
        LOG("stand-in probe: ray (%.1f, %.1f, %.1f) down 6 m, %.1f m from the player: default filter %d hits, nearest "
            "at %.2f (handle %08X, object %08X); static level %d hits, nearest at %.2f; its stand-in %08X", double(top[0]),
            double(top[1]), double(top[2]),
            double(std::sqrt((mid[0] - pp[0]) * (mid[0] - pp[0]) + (mid[1] - pp[1]) * (mid[1] - pp[1]) +
                             (mid[2] - pp[2]) * (mid[2] - pp[2]))),
            ray.hits, double(ray.fraction), ray.handle, ray.object, level.hits, double(level.fraction), p.stand_in);
        // A blow as the hammer's: through the zone object's vtable (our wrapper, then the game's).
        const float j[3] = {3.0f * p.mass, 0.5f * p.mass, 0.0f};
        void** vt = *reinterpret_cast<void***>(g_zone_obj);
        const ZoneMelee melee = reinterpret_cast<ZoneMelee>(vt[kZoneSlotMelee]);
        const uint32_t before = g_melee_hits;
        melee(g_zone_obj, nullptr, nullptr, 10.0f, j, mid, mid, 0, nullptr);
        const bool passed_on = g_melee_hits != before;
        g_probe_blows += passed_on ? 1 : 0;
        LOG("stand-in probe: piece %u (%.0f kg, %s, %s shape): the default ray from above hits %s; the blow %s",
            i, double(p.mass), p.heavy && g_solid ? "solid" : "ghost", p.shape ? "its own" : "a box",
            on_stand_in ? "a stand-in at the piece" : ray.hits == 0 ? "nothing" : "something else",
            passed_on ? "reached it" : "did not reach it");
        const ipc::BodyPose& b = pxbridge::poses(s)[i];
        g_probe_pos[0] = b.pos.x, g_probe_pos[1] = b.pos.y, g_probe_pos[2] = b.pos.z;
        g_probe_slot = i;
        g_probe_stage = 1;
        g_probe_at = now;
        return;
    }
}

// A guard the game lacks, needed by the stand-ins. MyCharacterListener::processConstraintsCallback, where the player
// leans on a slow body (+0x3D2), reads the subtype of the body's game object (cmp byte ptr [esi+0x7F], 4) without the
// null check it makes a few lines up; a stand-in has no game object (crashed both test replays). Those 12 bytes
// (cmp; movss xmm0, [esp+0x5C]; jne) now jump to a stub that checks first: no object counts as "not subtype 4".
const rfg::CodeSite kLeanSite{r::MyCharacterListener__processConstraintsCallback, 0x3D2},
    kLeanSubtype4{r::MyCharacterListener__processConstraintsCallback, 0x3DE},
    kLeanOther{r::MyCharacterListener__processConstraintsCallback, 0x3EA};

bool patch_lean_check() {
    uint8_t* site = static_cast<uint8_t*>(rfg::addr(kLeanSite.rva()));
    static const uint8_t kOriginal[12] = {0x80, 0x7E, 0x7F, 0x04, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x5C, 0x75, 0x0C};
    if (memcmp(site, kOriginal, sizeof(kOriginal)) != 0) return false;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    uint8_t* p = stub;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto jump_to = [&](const void* target) {
        *p++ = 0xE9;
        const int32_t rel = int32_t(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(p + 4));
        memcpy(p, &rel, 4);
        p += 4;
    };
    put({0x85, 0xF6});                          // test esi, esi
    put({0x74, 0x11});                          // jz other
    put({0x80, 0x7E, 0x7F, 0x04});              // cmp byte ptr [esi+0x7F], 4
    put({0x75, 0x0B});                          // jne other
    put({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x5C});  // movss xmm0, [esp+0x5C]
    jump_to(rfg::addr(kLeanSubtype4.rva()));
    put({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x5C});  // other: movss xmm0, [esp+0x5C]
    jump_to(rfg::addr(kLeanOther.rva()));
    FlushInstructionCache(GetCurrentProcess(), stub, size_t(p - stub));
    DWORD old;
    if (!VirtualProtect(site, sizeof(kOriginal), PAGE_EXECUTE_READWRITE, &old)) return false;
    site[0] = 0xE9;
    const int32_t rel = int32_t(reinterpret_cast<intptr_t>(stub) - reinterpret_cast<intptr_t>(site + 5));
    memcpy(site + 1, &rel, 4);
    memset(site + 5, 0x90, sizeof(kOriginal) - 5);
    VirtualProtect(site, sizeof(kOriginal), old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(kOriginal));
    return true;
}

void free_all_stand_ins() {
    for (uint32_t h : g_stand_ins_to_free) free_stand_in(h);
    g_stand_ins_to_free.clear();
    for (uint32_t i : g_stand_in_slots) {
        Piece& p = g_pieces[i];
        if (p.stand_in == kNoStandIn) continue;
        free_stand_in(p.stand_in);
        p.stand_in = kNoStandIn;
    }
    g_stand_in_slots.clear();
    g_stand_in_zones.clear();
    g_carried_zones.clear();
}

void log_explosion_infos();  // below, with the explosion hook
void send_explosions();

// ---- diagnostics (Telemetry.Enabled): what stays in Havok, and what vehicles are made of ----

struct ShapeCensus : hs::Sink {
    uint32_t convexes = 0, triangles = 0, height_fields = 0, unsupported_leaves = 0;
    uint8_t first_unsupported = 0xFF;
    void triangle(const float*, const float*, const float*) override { ++triangles; }
    void convex(const float (*)[3], int, float, uint32_t) override { ++convexes; }
    void height_field(const uint8_t*, const hs::Transform&) override { ++height_fields; }
    void unsupported(uint8_t type) override {
        if (!unsupported_leaves++) first_unsupported = type;
    }
};

bool census_walk(const uint8_t* shape, ShapeCensus& c) {
    __try {
        return hs::walk(shape, hs::Transform::identity(), c);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// Every general mover the world lists, by why adopt() leaves it in Havok.
void census_general_movers() {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldGeneralMoverCount);
    if (!all || !movers || n <= 0) return;
    uint8_t* const* bodies = static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies));
    const auto is_fading = rfg::fn<bool(__cdecl*)(uint8_t*)>(r::object_mover_is_fading_for_delete);
    enum { kDestroyed, kNotBalanced, kFading, kNoInstance, kAttached, kNoBody, kKeyframed, kFixed, kBadShape, kTriangles,
           kUnsupported, kAdoptable, kReasons };
    static const char* const kNames[kReasons] = {"destroyed", "not load-balanced", "fading", "no render instance",
                                                 "attached or constrained", "no Havok body", "keyframed", "fixed",
                                                 "shape not readable", "triangle shapes", "unsupported shapes",
                                                 "adoptable"};
    uint32_t count[kReasons] = {};
    uint32_t unsupported_leaf[hs::kTypeCount + 1] = {}, unsupported_top[hs::kTypeCount + 1] = {};
    uint32_t constrained_with_shape = 0;
    for (int i = 0; i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (!obj) continue;
        if (obj[kObjFlags] & kObjDestroyed) { ++count[kDestroyed]; continue; }
        const uint32_t mflags = *reinterpret_cast<const uint32_t*>(obj + kMoverFlags);
        if (!(mflags & kMoverLoadBalanced)) { ++count[kNotBalanced]; continue; }
        if ((mflags & kMoverFading) || is_fading(obj)) { ++count[kFading]; continue; }
        if (!*reinterpret_cast<uint8_t**>(obj + kGmRenderInstance)) { ++count[kNoInstance]; continue; }
        const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
        const uint8_t* body = handle == 0xFFFFFFFFu ? nullptr : bodies[handle];
        const uint32_t* attach = reinterpret_cast<const uint32_t*>(obj + kObjAttach);
        if (attach[0] || attach[1] || attach[2]) {
            ++count[kAttached];
            if (body) ++constrained_with_shape;
            continue;
        }
        if (!body) { ++count[kNoBody]; continue; }
        const uint8_t motion = body[kBodyMotionType];
        if (motion == 4) { ++count[kKeyframed]; continue; }
        if (motion == 0 || motion == 5) { ++count[kFixed]; continue; }
        ShapeCensus c;
        const uint8_t* shape = hs::body_shape(body);
        if (!shape || !census_walk(shape, c)) { ++count[kBadShape]; continue; }
        if (c.unsupported_leaves) {
            ++count[kUnsupported];
            ++unsupported_leaf[std::min<uint8_t>(c.first_unsupported, hs::kTypeCount)];
            ++unsupported_top[std::min<uint8_t>(hs::type_of(shape), hs::kTypeCount)];
            continue;
        }
        if (c.triangles || c.height_fields) { ++count[kTriangles]; continue; }
        ++count[kAdoptable];
    }
    char text[768];
    int len = 0;
    for (int k = 0; k < kReasons && len < int(sizeof(text)) - 48; ++k)
        if (count[k]) len += snprintf(text + len, sizeof(text) - len, "%s%s %u", len ? ", " : "", kNames[k], count[k]);
    LOG("debris census: %d general movers in Havok: %s", n, len ? text : "none");
    for (int t = 0; t <= hs::kTypeCount; ++t)
        if (unsupported_leaf[t]) LOG("debris census:   unsupported leaf %s: %u", hs::type_name(uint8_t(t)), unsupported_leaf[t]);
    for (int t = 0; t <= hs::kTypeCount; ++t)
        if (unsupported_top[t]) LOG("debris census:   unsupported, top shape %s: %u", hs::type_name(uint8_t(t)), unsupported_top[t]);
    len = 0;
    for (int t = 0; t <= hs::kTypeCount && len < int(sizeof(text)) - 48; ++t)
        if (g_adopted_top_types[t])
            len += snprintf(text + len, sizeof(text) - len, "%s%s %u", len ? ", " : "", hs::type_name(uint8_t(t)),
                            g_adopted_top_types[t]);
    if (len) LOG("debris census: top shapes of the pieces taken over so far: %s", text);
}

// The Havok bodies of vehicles (object type 3; walkers are vehicles) in the simulation islands.
void census_vehicles() {
    uint8_t* hw = rfg::global<uint8_t*>(r::Havok_world);
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!hw || !player) return;
    const float* pp = reinterpret_cast<const float*>(player + 0x04);
    const auto object_of = rfg::fn<uint8_t*(__cdecl*)(const uint8_t*)>(r::object_from_world_obj);
    struct Seen {
        uint8_t* obj;
        uint32_t bodies;
        float dist;
    };
    std::vector<Seen> vehicles;
    static uint32_t censuses = 0;
    uint32_t logged = censuses++ ? 40 : 0;  // the bodies one by one in the first census only
    for (uint32_t list = 0; list < 2; ++list) {  // m_activeSimulationIslands, m_inactiveSimulationIslands
        const uint8_t* const* islands = *reinterpret_cast<uint8_t* const* const*>(hw + 0x28 + list * 0xC);
        const int ni = *reinterpret_cast<const int*>(hw + 0x2C + list * 0xC);
        for (int i = 0; islands && i < ni; ++i) {
            const uint8_t* island = islands[i];
            if (!island) continue;
            const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + 0x4C);
            const int ne = *reinterpret_cast<const int*>(island + 0x50);
            for (int e = 0; entities && e < ne; ++e) {
                const uint8_t* body = entities[e];
                uint8_t* obj = body ? object_of(body) : nullptr;
                if (!obj || int8_t(obj[kObjType]) != kObjTypeVehicle) continue;
                const hs::Transform t = hs::body_transform(body);
                const float dx = t.pos[0] - pp[0], dy = t.pos[1] - pp[1], dz = t.pos[2] - pp[2];
                const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                auto it = std::find_if(vehicles.begin(), vehicles.end(), [obj](const Seen& s) { return s.obj == obj; });
                if (it == vehicles.end()) vehicles.push_back({obj, 1, dist});
                else ++it->bodies, it->dist = std::min(it->dist, dist);
                if (dist > 150.0f || logged >= 40) continue;
                ++logged;
                ShapeCensus c;
                const uint8_t* shape = hs::body_shape(body);
                const bool read = shape && census_walk(shape, c);
                const uint32_t filter = *reinterpret_cast<const uint32_t*>(body + kBodyFilterInfo);
                const float inv_mass = *reinterpret_cast<const float*>(body + kBodyInvMass);
                LOG("vehicle census: vehicle %08X (%s island) body %p: %.1f m away, motion %u, mass %.0f kg, layer %u "
                    "group %u, shape %s: %u convex, %u triangles, %u unsupported (first %s)%s",
                    *reinterpret_cast<const uint32_t*>(obj + kObjHandle), list ? "inactive" : "active",
                    static_cast<const void*>(body), double(dist), unsigned(body[kBodyMotionType]),
                    inv_mass > 0 ? 1.0 / double(inv_mass) : 0.0, filter & 0x7F, filter >> 16,
                    shape ? hs::type_name(hs::type_of(shape)) : "none", c.convexes, c.triangles, c.unsupported_leaves,
                    c.unsupported_leaves ? hs::type_name(c.first_unsupported) : "-", read ? "" : " (not fully readable)");
            }
        }
    }
    uint32_t nearby = 0;
    for (const Seen& s : vehicles) nearby += s.dist < 150.0f ? 1 : 0;
    LOG("vehicle census: %zu vehicles with Havok bodies, %u within 150 m of the player", vehicles.size(), nearby);
}

// Everything in Havok's simulation islands (what moves), by object type, subtype, motion type and layer.
void census_islands() {
    uint8_t* hw = rfg::global<uint8_t*>(r::Havok_world);
    if (!hw) return;
    const auto object_of = rfg::fn<uint8_t*(__cdecl*)(const uint8_t*)>(r::object_from_world_obj);
    std::vector<std::pair<uint32_t, uint32_t>> counts;  // key (type, subtype, motion, layer), bodies
    uint32_t total = 0;
    for (uint32_t list = 0; list < 2; ++list) {
        const uint8_t* const* islands = *reinterpret_cast<uint8_t* const* const*>(hw + 0x28 + list * 0xC);
        const int ni = *reinterpret_cast<const int*>(hw + 0x2C + list * 0xC);
        for (int i = 0; islands && i < ni; ++i) {
            const uint8_t* island = islands[i];
            if (!island) continue;
            const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + 0x4C);
            const int ne = *reinterpret_cast<const int*>(island + 0x50);
            for (int e = 0; entities && e < ne; ++e) {
                const uint8_t* body = entities[e];
                if (!body) continue;
                const uint8_t* obj = object_of(body);
                const uint32_t key = (obj ? uint32_t(uint8_t(obj[kObjType])) << 24 | uint32_t(uint8_t(obj[kObjType + 1])) << 16
                                          : 0xFFFF0000u) |
                                     uint32_t(body[kBodyMotionType]) << 8 |
                                     (*reinterpret_cast<const uint32_t*>(body + kBodyFilterInfo) & 0x7F);
                auto it = std::find_if(counts.begin(), counts.end(), [key](const auto& c) { return c.first == key; });
                if (it == counts.end()) counts.emplace_back(key, 1);
                else ++it->second;
                ++total;
            }
        }
    }
    std::sort(counts.begin(), counts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    char text[640];
    int len = 0;
    for (size_t i = 0; i < counts.size() && i < 16 && len < int(sizeof(text)) - 48; ++i) {
        const uint32_t k = counts[i].first;
        len += snprintf(text + len, sizeof(text) - len, "%s type %d/%d motion %u layer %u: %u", i ? ";" : "",
                        int(int8_t(k >> 24)), int(int8_t(k >> 16)), (k >> 8) & 0xFF, k & 0x7F, counts[i].second);
    }
    LOG("island census: %u bodies move in Havok:%s", total, len ? text : " none");
}

// Test.InspectFrames (replays): at those frames, what the camera looks at. The game's default ray along the view
// (what it hits: an object, or a stand-in or terrain, both carrying a zone object), the pieces in PhysX within 25 m of
// that point (frozen, awake, with a stand-in), the Havok bodies there by kind, and the same ray against the static
// world mirrored in the host (answered a few frames later): something there that Havok no longer has holds pieces up.
std::vector<int32_t> g_inspect_frames;
int32_t g_last_inspected = -1;
uint32_t g_inspect_request = 0;
float g_inspect_havok_m = -1.0f;

// What a ray along the view hit, for the log: the object (type, subtype, where) and whether its Havok body is fixed.
void describe_hit(int32_t frame, const char* filter, const RayResult& ray, float dist, const float* hit) {
    int type = -1, sub = -1, motion = -1;
    float opos[3] = {};
    if (ray.hits && ray.object != 0xFFFFFFFFu) {
        const auto from_handle = rfg::fn<uint8_t*(__fastcall*)(void*, void*, uint32_t)>(r::world__object_from_handle);
        if (const uint8_t* obj = from_handle(world(), nullptr, ray.object)) {
            type = int8_t(obj[kObjType]), sub = int8_t(obj[kObjType + 1]);
            memcpy(opos, obj + 0x04, sizeof(opos));
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
            const uint8_t* body = handle == 0xFFFFFFFFu ? nullptr
                                                        : static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies))[handle];
            if (body) motion = body[kBodyMotionType];
        }
    }
    LOG("inspect frame %d: the %s ray hits %s at %.1f m, (%.1f, %.1f, %.1f): object %08X type %d/%d at (%.1f, %.1f, "
        "%.1f), its body's motion %d (5 fixed)", frame, filter, ray.hits ? "something" : "nothing", double(dist),
        double(hit[0]), double(hit[1]), double(hit[2]), ray.object, type, sub, double(opos[0]), double(opos[1]),
        double(opos[2]), motion);
}

void inspect_view(int32_t frame) {
    const uint8_t* cam = static_cast<const uint8_t*>(rfg::addr(r::Rfg_camera));
    const float* cp = reinterpret_cast<const float*>(cam + 0x2C);         // real_pos
    const float* fv = reinterpret_cast<const float*>(cam + 0x50 + 0x18);  // real_orient.fvec
    constexpr float kRange = 300.0f, kPastPlayer = 4.0f;  // the third-person camera looks over the player's shoulder
    const float start[3] = {cp[0] + fv[0] * kPastPlayer, cp[1] + fv[1] * kPastPlayer, cp[2] + fv[2] * kPastPlayer};
    const float end[3] = {cp[0] + fv[0] * kRange, cp[1] + fv[1] * kRange, cp[2] + fv[2] * kRange};
    LOG("inspect frame %d: camera (%.1f, %.1f, %.1f) looking (%.2f, %.2f, %.2f)", frame, double(cp[0]), double(cp[1]),
        double(cp[2]), double(fv[0]), double(fv[1]), double(fv[2]));
    const RayResult ray = probe_ray(start, end);
    const float dist = kPastPlayer + (ray.hits ? ray.fraction : 1.0f) * (kRange - kPastPlayer);
    const float hit[3] = {cp[0] + fv[0] * dist, cp[1] + fv[1] * dist, cp[2] + fv[2] * dist};
    describe_hit(frame, "default", ray, dist, hit);
    {
        RayResult building;
        alignas(16) uint8_t in[0x30] = {};
        rfg::fn<void(__fastcall*)(void*, void*)>(r::raycast_input__raycast_input)(in, nullptr);
        memcpy(in, start, 12);
        memcpy(in + 0xC, end, 12);
        alignas(16) uint8_t out[0x10] = {};
        rfg::fn<void(__fastcall*)(void*, void*)>(r::raycast_output__raycast_output)(out, nullptr);
        if (rfg::fn<bool(__cdecl*)(void*, void*)>(r::havok_raycast_against_destroyable_geometry)(in, out)) {
            const uint8_t* hits = *reinterpret_cast<const uint8_t* const*>(out);
            building.hits = *reinterpret_cast<const int*>(out + 4);
            float best = 2.0f;
            for (int i = 0; hits && i < building.hits; ++i) {
                const float f = *reinterpret_cast<const float*>(hits + i * 0x38 + 0x24);
                if (f < best) best = building.fraction = f, building.object = *reinterpret_cast<const uint32_t*>(hits + i * 0x38 + 0x28);
            }
        }
        rfg::fn<void(__fastcall*)(void*, void*)>(r::raycast_output___raycast_output)(out, nullptr);
        const float bdist = kPastPlayer + (building.hits ? building.fraction : 1.0f) * (kRange - kPastPlayer);
        const float bhit[3] = {cp[0] + fv[0] * bdist, cp[1] + fv[1] * bdist, cp[2] + fv[2] * bdist};
        describe_hit(frame, "destroyable-geometry", building, bdist, bhit);
    }
    // A fan of rays, 9 x 9 over 20 degrees around the view: every object they hit first, once.
    {
        const float* rv = reinterpret_cast<const float*>(cam + 0x50);         // real_orient.rvec
        const float* uv = reinterpret_cast<const float*>(cam + 0x50 + 0xC);   // real_orient.uvec
        std::vector<std::pair<uint32_t, float>> seen;  // object, distance
        for (int iy = -4; iy <= 4; ++iy)
            for (int ix = -4; ix <= 4; ++ix) {
                const float ax = float(ix) * 0.0436f, ay = float(iy) * 0.0436f;  // 2.5 degrees apart
                float d[3];
                for (int k = 0; k < 3; ++k) d[k] = fv[k] + rv[k] * ax + uv[k] * ay;
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (float& c : d) c /= len;
                const float a[3] = {cp[0] + d[0] * kPastPlayer, cp[1] + d[1] * kPastPlayer, cp[2] + d[2] * kPastPlayer};
                const float b[3] = {cp[0] + d[0] * kRange, cp[1] + d[1] * kRange, cp[2] + d[2] * kRange};
                const RayResult fan = probe_ray(a, b);
                if (!fan.hits || fan.object == 0xFFFFFFFFu) continue;
                const float fd = kPastPlayer + fan.fraction * (kRange - kPastPlayer);
                auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.first == fan.object; });
                if (it == seen.end()) seen.emplace_back(fan.object, fd);
                else it->second = std::min(it->second, fd);
            }
        const auto from_handle = rfg::fn<uint8_t*(__fastcall*)(void*, void*, uint32_t)>(r::world__object_from_handle);
        for (size_t i = 0; i < seen.size() && i < 12; ++i) {
            const uint8_t* obj = from_handle(world(), nullptr, seen[i].first);
            if (!obj) continue;
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
            const uint8_t* body = handle == 0xFFFFFFFFu ? nullptr
                                                        : static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies))[handle];
            const float* op = reinterpret_cast<const float*>(obj + 0x04);
            LOG("inspect frame %d:   the fan hits object %08X type %d/%d at (%.1f, %.1f, %.1f), first at %.1f m; its body: "
                "motion %d, %u pieces in its shape", frame, seen[i].first, int(int8_t(obj[kObjType])),
                int(int8_t(obj[kObjType + 1])), double(op[0]), double(op[1]), double(op[2]), double(seen[i].second),
                body ? int(body[kBodyMotionType]) : -1, body && shape_signature(body) ? shape_signature(body) - 1 : 0u);
        }
        if (seen.empty()) LOG("inspect frame %d:   the fan hits no object", frame);
    }
    // The pieces around that point.
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    if (s) {
        const ipc::BodyPose* poses = pxbridge::poses(s);
        const uint32_t slots = std::min<uint32_t>(s->slots, g_vehicle_first);
        uint32_t n = 0, frozen = 0, awake = 0, stand_ins = 0, above = 0;
        float lo = 1e30f, hi = -1e30f;
        for (uint32_t i = 0; i < slots; ++i) {
            if (!g_pieces[i].inst || !(poses[i].flags & ipc::kPoseAlive)) continue;
            const ipc::Vec3& p = poses[i].pos;
            const float dx = p.x - hit[0], dy = p.y - hit[1], dz = p.z - hit[2];
            if (dx * dx + dy * dy + dz * dz > 25.0f * 25.0f) continue;
            ++n;
            frozen += (poses[i].flags & ipc::kPoseFrozen) ? 1 : 0;
            awake += (poses[i].flags & ipc::kPoseAwake) ? 1 : 0;
            stand_ins += g_pieces[i].stand_in != kNoStandIn ? 1 : 0;
            above += p.y > hit[1] - 2.0f ? 1 : 0;
            lo = std::min(lo, p.y), hi = std::max(hi, p.y);
        }
        LOG("inspect frame %d: %u pieces within 25 m of that point (%u frozen, %u awake, %u with a stand-in; %u no lower "
            "than 2 m below it; heights %.1f to %.1f)", frame, n, frozen, awake, stand_ins, above, double(n ? lo : 0),
            double(n ? hi : 0));
        // The pieces in view: within 3 m of the view ray, 4 to 150 m out (whatever the ray hit).
        const ipc::Vec3 eye{cp[0], cp[1], cp[2]};
        const float d150[3] = {fv[0] * 150.0f, fv[1] * 150.0f, fv[2] * 150.0f};
        uint32_t vn = 0, vfrozen = 0, vawake = 0, vheavy = 0, vbodiless = 0;
        float vnear = 1e30f, vfar = 0, vlo = 1e30f, vhi = -1e30f, vspeed = 0, vmass = 0;
        for (uint32_t i = 0; i < slots; ++i) {
            if (!g_pieces[i].inst) continue;
            const bool alive = (poses[i].flags & ipc::kPoseAlive) != 0;
            // A piece the host has no body for stays drawn where it was taken over (its instance's position).
            const float* ip = reinterpret_cast<const float*>(g_pieces[i].inst + kInstPos);
            const ipc::Vec3 at = alive ? poses[i].pos : ipc::Vec3{ip[0], ip[1], ip[2]};
            const ipc::Vec3 rel{at.x - eye.x, at.y - eye.y, at.z - eye.z};
            float along = 0;
            const float zero[3] = {0, 0, 0};
            if (distance_to_segment(rel, zero, d150, 150.0f * 150.0f, &along) > 3.0f || along < 4.0f || along >= 150.0f)
                continue;
            if (!alive) {
                vbodiless += g_pieces[i].fresh ? 0 : 1;
                continue;
            }
            ++vn;
            vfrozen += (poses[i].flags & ipc::kPoseFrozen) ? 1 : 0;
            vawake += (poses[i].flags & ipc::kPoseAwake) ? 1 : 0;
            vheavy += g_pieces[i].heavy;
            vnear = std::min(vnear, along), vfar = std::max(vfar, along);
            vlo = std::min(vlo, poses[i].pos.y), vhi = std::max(vhi, poses[i].pos.y);
            const float mx = poses[i].pos.x - poses[i].prev_pos.x, my = poses[i].pos.y - poses[i].prev_pos.y,
                        mz = poses[i].pos.z - poses[i].prev_pos.z;
            vspeed = std::max(vspeed, std::sqrt(mx * mx + my * my + mz * mz) * 60.0f);
            vmass += g_pieces[i].mass;
        }
        LOG("inspect frame %d: %u pieces in view (within 3 m of the view ray; %.1f to %.1f m out, heights %.1f to %.1f): "
            "%u frozen, %u awake, %u asleep; %u heavy, %.1f kg mean; fastest %.2f m/s; %u more without a body in the "
            "host", frame, vn, double(vn ? vnear : 0), double(vfar), double(vn ? vlo : 0), double(vn ? vhi : 0), vfrozen,
            vawake, vn - vfrozen - vawake, vheavy, double(vn ? vmass / float(vn) : 0), double(vspeed), vbodiless);
    }
    // The Havok bodies around it: the fixed island (the static world: buildings) and the moving ones.
    uint8_t* hw = rfg::global<uint8_t*>(r::Havok_world);
    if (hw) {
        const auto object_of = rfg::fn<uint8_t*(__cdecl*)(const uint8_t*)>(r::object_from_world_obj);
        std::vector<std::pair<uint32_t, uint32_t>> counts;  // (type, subtype, motion) -> bodies
        auto visit = [&](const uint8_t* island, bool fixed) {
            if (!island) return;
            const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + 0x4C);
            const int ne = *reinterpret_cast<const int*>(island + 0x50);
            for (int e = 0; entities && e < ne; ++e) {
                const uint8_t* body = entities[e];
                if (!body) continue;
                const hs::Transform t = hs::body_transform(body);
                const float dx = t.pos[0] - hit[0], dy = t.pos[1] - hit[1], dz = t.pos[2] - hit[2];
                if (dx * dx + dy * dy + dz * dz > 40.0f * 40.0f) continue;
                const uint8_t* obj = object_of(body);
                const uint32_t key = (obj ? uint32_t(uint8_t(obj[kObjType])) << 16 | uint32_t(uint8_t(obj[kObjType + 1])) << 8
                                          : 0xFFFF00u) | body[kBodyMotionType];
                auto it = std::find_if(counts.begin(), counts.end(), [key](const auto& c) { return c.first == key; });
                if (it == counts.end()) counts.emplace_back(key, 1);
                else ++it->second;
                const uint32_t sig = shape_signature(body);
                if (sig && obj && int8_t(obj[kObjType]) == kObjTypeMover)
                    LOG("inspect frame %d:   %s building chunk %08X at (%.1f, %.1f, %.1f), %.1f m from the point: %u pieces "
                        "in its shape", frame, fixed ? "standing" : "loose", *reinterpret_cast<const uint32_t*>(obj + kObjHandle),
                        double(t.pos[0]), double(t.pos[1]), double(t.pos[2]), double(std::sqrt(dx * dx + dy * dy + dz * dz)),
                        sig - 1);
            }
        };
        visit(*reinterpret_cast<uint8_t* const*>(hw + 0x20), true);  // m_fixedIsland
        for (uint32_t list = 0; list < 2; ++list) {
            const uint8_t* const* islands = *reinterpret_cast<uint8_t* const* const*>(hw + 0x28 + list * 0xC);
            const int ni = *reinterpret_cast<const int*>(hw + 0x2C + list * 0xC);
            for (int i = 0; islands && i < ni; ++i) visit(islands[i], false);
        }
        char text[512];
        int len = 0;
        for (size_t i = 0; i < counts.size() && len < int(sizeof(text)) - 40; ++i)
            len += snprintf(text + len, sizeof(text) - len, "%s type %d/%d motion %u: %u", i ? ";" : "",
                            int(int8_t(counts[i].first >> 16)), int(int8_t(counts[i].first >> 8)), counts[i].first & 0xFF,
                            counts[i].second);
        LOG("inspect frame %d: Havok bodies within 40 m of the point:%s", frame, len ? text : " none");
    }
    // The same ray against the static world in the host.
    ipc::Ray mirror_ray{};
    mirror_ray.origin = {cp[0], cp[1], cp[2]};
    mirror_ray.direction = {fv[0], fv[1], fv[2]};
    mirror_ray.max_distance = kRange;
    g_inspect_request = 0x15C70000u | uint32_t(frame & 0xFFFF);
    g_inspect_havok_m = ray.hits ? dist : -1.0f;
    if (!pxbridge::raycast_batch(g_inspect_request, &mirror_ray, 1)) g_inspect_request = 0;
}

void inspect_frame() {
    if (g_inspect_request && pxbridge::query_done() == g_inspect_request) {
        LOG("inspect: the ray against the static world in the host hits at %.1f m (Havok's default ray: %.1f m)",
            double(pxbridge::query_results()[0]), double(g_inspect_havok_m));
        g_inspect_request = 0;
    }
    const int32_t frame = harness::replay_frame();
    if (frame < 0 || frame == g_last_inspected) return;
    if (std::find(g_inspect_frames.begin(), g_inspect_frames.end(), frame) == g_inspect_frames.end()) return;
    g_last_inspected = frame;
    inspect_view(frame);
}

// Which layers the key layers collide with, from the game's layer table (havok_game_filter: bit a * 128 + b).
void log_layer_table() {
    const uint8_t* filter = rfg::global<uint8_t*>(r::Havok_filter);
    const uint32_t* table = filter ? *reinterpret_cast<const uint32_t* const*>(filter + 0x34) : nullptr;
    if (!table) return;
    static const struct {
        int layer;
        const char* name;
    } kLayers[] = {{2, "world"}, {3, "mover"}, {4, "item"}, {5, "ragdoll"}, {6, "player"}, {7, "npc"},
                   {9, "vehicle"}, {12, "walker"}, {14, "walker hull"}, {15, "flyer"}, {16, "projectile"},
                   {19, "object collector"}, {22, "no collision"}, {25, "ray default"}, {36, "keyframed"},
                   {38, "physical object"}, {39, "rfg destroyable"}, {40, "rfg normal"}, {41, "rfg normal fast"},
                   {42, "rfg lite"},
                   {43, "rfg ultra lite"}, {44, "rfg world only"}, {45, "ray rfg destroyable"}, {46, "rfg stress"},
                   {48, "camera"}, {49, "human hit detect"}, {57, "ray vehicle wheel"}, {58, "effect rigid body"},
                   {61, "invisible barrier"}};
    for (const auto& l : kLayers) {
        char text[512];
        int len = 0;
        for (int b = 0; b < 128 && len < int(sizeof(text)) - 8; ++b) {
            const uint32_t bit = uint32_t(l.layer) * 128 + uint32_t(b);
            if (table[bit >> 5] & (1u << (bit & 31))) len += snprintf(text + len, sizeof(text) - len, " %d", b);
        }
        LOG("layer table: %2d %-20s collides with:%s", l.layer, l.name, len ? text : " nothing");
    }
}

// The game touches render instances only in the sim/render handshake, while the render thread waits: its objects'
// update_render functions run there as game render tasks (Task_list_update_render, run by game_render_setup_frame
// from gamestate_gp_handshake). Moving the pieces from the end of the sim frame instead raced the render thread's
// visibility pass, which then looped forever (rl_frustum_base::is_occluded) after a few matches; so they move in a
// render task too. The task list is walked on the sim thread only.
struct GameRenderTask {
    bool* enabled;  // null: always
    void(__cdecl* callback)(void* user, int arg);
    void* user;
    GameRenderTask* next;
    GameRenderTask* prev;
};
static_assert(sizeof(GameRenderTask) == 0x14, "game_render_task");
GameRenderTask g_render_task{};
bool g_task_registered = false;

// A long session in the open world fills even a large pool of render instances (Experiment.RenderInstances), and
// then shattering creates no pieces. Like the game's load governor with the debris it keeps, the pieces farthest
// from the player go first: below kPoolReserve free instances, up to kRecyclePerFrame of the farthest are given up
// each frame until kPoolReserve + kPoolHysteresis are free. In the render task, since it releases render instances.
// Only loose pieces: a piece in a pile draws through its pile's instance, so giving it up frees none (piles hold most
// far pieces, which went before the loose ones that hold the instances).
constexpr int32_t kPoolReserve = 2000, kPoolHysteresis = 1000;
constexpr uint32_t kRecyclePerFrame = 200;
bool g_recycling = false;
uint64_t g_recycled_total = 0;

int32_t render_pool_free() {
    return rfg::global<int32_t>(r::rl_destroyable_model_instance__ms_object_pool + 0xC);
}

void recycle_far_pieces() {
    const int32_t free = render_pool_free();
    if (!g_recycling) {
        if (free >= kPoolReserve) return;
        g_recycling = true;
        LOG("debris offload: %d render instances left: giving up the pieces farthest from the player", free);
    } else if (free >= kPoolReserve + kPoolHysteresis) {
        g_recycling = false;
        LOG("debris offload: %d render instances free again (%llu pieces given up so distant)", free, g_recycled_total);
        return;
    }
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    if (!player || !s) return;
    const float* pp = reinterpret_cast<const float*>(player + 0x04);
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint32_t slots = std::min<uint32_t>(s->slots, g_slots);
    static std::vector<std::pair<float, uint32_t>> distant;
    distant.clear();
    for (uint32_t i = 0; i < slots; ++i) {
        if (!g_pieces[i].inst || g_pieces[i].pile != kNoPile || i == g_player_slot) continue;
        const ipc::Vec3& p = poses[i].pos;
        const float dx = p.x - pp[0], dy = p.y - pp[1], dz = p.z - pp[2];
        distant.emplace_back(-(dx * dx + dy * dy + dz * dz), i);  // farthest first
    }
    const size_t n = std::min<size_t>(distant.size(), kRecyclePerFrame);
    if (!n) return;
    std::nth_element(distant.begin(), distant.begin() + (n - 1), distant.end());
    for (size_t k = 0; k < n; ++k) give_up(distant[k].second, true);
    g_recycled_total += n;
}

// A restored object (world::restore_object: destroyed and streamed in again, as an activity's building when the
// activity starts over) takes the pieces that broke off it along: in the game they would have faded out long before.
// They go at the next handshake, where render instances may be released. The handles come from KeepDebris: the
// restored objects and the loose chunks that came off them (experiments/keep_debris.cpp).
// One pass over the pieces for all of them (a save's load restores thousands of objects).
std::unordered_set<uint32_t> g_restored;  // sim thread: handles of restored objects and their loose chunks
std::unordered_set<uint32_t> g_last_restored;  // diagnostics: the handles of the last restoration
uint64_t g_restored_pieces = 0;
uint32_t g_restore_logs = 0;

void forget_restored() {
    if (g_restored.empty()) return;
    g_last_restored = g_restored;
    uint32_t n = 0;
    for (uint32_t s = 0; s < g_slots; ++s)
        if (g_pieces[s].inst && g_restored.count(g_pieces[s].original)) {
            give_up(s, true);
            ++n;
        }
    g_restored_pieces += n;
    if (n && g_restore_logs++ < 20)
        LOG("debris offload: %zu objects were restored (with their loose chunks); the %u pieces that broke off them "
            "went with them", g_restored.size(), n);
    g_restored.clear();
}

// ---- piles: frozen pieces drawn as compiled instances (Experiment.PhysXDebrisPiles) ----
// The render thread's time follows the number of pieces in view, not what they draw: every piece is an
// rl_destroyable_model_instance of one sub-instance, so each visible one is tested against every pass, walks its
// building's whole material map (m_mat_map, 18 materials on average) twice and goes through add_to_batch on its own
// (about 12,000 a frame in the heaviest part of the wcdlc7 replay, 31,000 pieces). A standing building is the other
// way round: one instance whose sub-instances are compiled into one mesh (rl_start_recompile_mesh_instance), a draw
// per material and pass. A pile turns the still pieces of one building lying in one cell (16 m) into that.
// The game's machinery: a building's sub-instances are one array in its shared base
// (rl_destroyable_model_instance_shared_base, m_base_sub_instances +0x14), each instance owning a contiguous range of it
// (m_sub_instances +0x11C, m_num_sub_instances +0x120). unclaim_sub_instances gives sub-instances back to the base and
// claim_sub_instances takes them, by their index in the building (m_sub_instance_original_index_map +0x0C, by place in
// the array); the repair path (rfg_mover::create_subpieces) claims them and update_instances places them in the
// instance, then calculate_local_aabb sizes it (as build_new_rl_instance_from_shape does); split makes a new instance
// of some of an instance's sub-instances (the same indices), in the same scene partition. Each of these deletes the
// instance's mesh and asks for a new compile: it draws batched until the new one arrives.
// Measured with a one-shot merge at frame 40000 of wcdlc7 (2 runs each, frames 44000-45775): 12.0-13.1 -> 6.7-6.8 ms a
// frame, before_kick 7.9-8.7 -> 2.8-3.0 ms, 40% fewer draws, the same address space. Drawn batched instead, piles
// saved nothing: a pile is culled as one box, and each of its pieces is a draw of its own in every pass that sees any
// part of it (twice the draws).
constexpr uint32_t kInstMatMap = 0x118, kInstSubArray = 0x11C, kInstNumSubs = 0x120, kInstRenderableSubs = 0x124,
                   kInstShared = 0x148;
constexpr uint32_t kBaseOrigIndex = 0x0C, kBaseSubArray = 0x14;
// rl_sub_instance: rl_compressed_transform (pos, pitch, bank, heading), m_instance_index, m_submesh_id, flags
// (bit 0 visible, bit 1 active, bit 31 disable_deferred).
constexpr uint32_t kSubSize = 0x20, kSubPitch = 0x0C, kSubSubmesh = 0x1A, kSubFlags = 0x1C;
constexpr uint32_t kSubDisableDeferred = 0x80000000u;
float g_pile_cell = 16.0f;  // m: Experiment.PhysXDebrisPileCellM
constexpr double kPileFrozenMs = 2000;       // a piece still this long (frozen or asleep) joins a pile
constexpr double kPileSettleMs = 1500;       // a pile takes new pieces this long after its last change at the soonest
constexpr uint32_t kPileMinNew = 4;          // frozen pieces in a cell that start a pile there
// Each join and each split moves the building's whole sub-instance array in the game (about 50-100 us for the
// largest buildings): 512 of them in one frame took up to 39 ms.
constexpr double kPileScanMs = 100;          // how often frozen pieces are looked over for piles
constexpr uint32_t kUnpilesPerFrame = 256;   // pieces leaving piles in one frame, at most
constexpr uint32_t kPileRetriesPerFrame = 8; // piles asking for their compile again in one frame, at most
// The render task adds to the frame: a scan's joins took up to 7.3 ms in one frame, 128 of them. Past these times the
// joins stop (the next frame goes on with the scan's cells, without looking the pieces over again) and piles that lost
// pieces are sized again in a later frame (until then their bounds are only too large). With more than
// kPileBacklog pieces ready, the joins take kPileJoinBudgetBehindMs a frame: a piece drawn on its own costs about
// 1 us every frame and its join 50 us once. Counted in frames instead (128 joins every 15), the piles fell behind in
// slow frames, which the pieces left on their own made slower still (Test.StressPieces: 17,000 of 69,000 pieces piled,
// 52 ms a frame).
constexpr double kPileJoinBudgetMs = 1.0, kPileJoinBudgetBehindMs = 3.0;
constexpr size_t kPileBacklog = 2000;
constexpr double kPileResizeBudgetMs = 0.5;
// A cell with fewer still pieces than kPileMinNew and no pile of its own: they join their building's pile in a
// neighbouring cell. Such pieces were nearly all the pieces left on their own at a levelled map's end (930-1,040 of
// about 31,000). Only once the first of them has been still for Experiment.PhysXDebrisPileNextDoorS (10 s; 0: never):
// joining at once kept cells next to a pile from ever getting their own, and piles spread over several cells. A cell
// with no pile next door is looked at again after kNoPileNearRetryMs.
double g_next_door_ms = 10000;
constexpr double kNoPileNearRetryMs = 3000;
// Experiment.PhysXDebrisPileMaxPieces (0: none): a full pile keeps its pieces but leaves its cell's place
// (g_pile_at), and the cell's next still pieces start another. With 16 m cells, piles near the action reached 2,100
// pieces: every change recompiled all of them, and they drew piece by piece meanwhile (310 pieces a frame against
// 120 at 8 m).
uint32_t g_pile_max = 512;
uint64_t g_piles_full = 0;
// A woken piece leaves its pile only once it has moved from where it was drawn: blasts and the player wake many pieces
// that go nowhere (the first runs split 81,000 pieces off their piles in a 245 s match and joined 107,000).
constexpr float kUnpileMove = 0.02f;         // m
constexpr float kUnpileTurnCos = 0.99996f;   // cos of half a 1 degree turn: |q1.q2| below this has turned more

struct SubInstanceUpdate {  // rl_sub_instance_update
    float m[12];            // rl_sub_instance_info::m_transform, matrix43: rvec, uvec, fvec, pos (in the instance)
    uint32_t submesh_id;
    uint32_t flags;         // bit 0 m_is_active, bit 1 m_disable_deferred
    int32_t base_index;     // the sub-instance's index in its building
};
static_assert(sizeof(SubInstanceUpdate) == 0x3C, "rl_sub_instance_update");

// The game's transforms: rows are the rotated x, y and z axes, a point goes to p * r + t.
struct M43 {
    float r[9];
    float t[3];
};

M43 mul(const M43& a, const M43& b) {  // a, then b
    M43 c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            c.r[i * 3 + j] = a.r[i * 3] * b.r[j] + a.r[i * 3 + 1] * b.r[3 + j] + a.r[i * 3 + 2] * b.r[6 + j];
    for (int j = 0; j < 3; ++j) c.t[j] = a.t[0] * b.r[j] + a.t[1] * b.r[3 + j] + a.t[2] * b.r[6 + j] + b.t[j];
    return c;
}

M43 inverse(const M43& a) {  // of a rotation and a translation
    M43 c;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) c.r[i * 3 + j] = a.r[j * 3 + i];
    for (int j = 0; j < 3; ++j) c.t[j] = -(a.t[0] * c.r[j] + a.t[1] * c.r[3 + j] + a.t[2] * c.r[6 + j]);
    return c;
}

M43 instance_matrix(const uint8_t* inst) {
    M43 m;
    memcpy(m.r, inst + kInstRot, sizeof(m.r));
    memcpy(m.t, inst + kInstPos, sizeof(m.t));
    return m;
}

M43 sub_instance_matrix(const uint8_t* sub) {  // in its instance
    M43 m;
    const float* angles = reinterpret_cast<const float*>(sub + kSubPitch);
    rfg::fn<void(__thiscall*)(float*, float, float, float)>(r::matrix__set_angles)(m.r, angles[0], angles[1],
                                                                                    angles[2]);
    memcpy(m.t, sub, sizeof(m.t));
    return m;
}

// A building's pieces in one cell of `size` m.
uint64_t merge_cell_key(const void* base, const ipc::Vec3& p, float size) {
    const auto c = [&](float v) { return uint64_t(uint32_t(int32_t(std::floor(v / size))) & 0x3FFu); };
    return uint64_t(reinterpret_cast<uintptr_t>(base)) << 30 | c(p.x) << 20 | c(p.y) << 10 | c(p.z);
}

struct RlSplitInfo {  // rl_split_info
    int32_t num_to_split;
    int16_t* to_split;   // sub-instance indices in the building
    int32_t num_to_disable;
    int16_t* to_disable;
    int32_t num_to_enable;
    int16_t* to_enable;
};
static_assert(sizeof(RlSplitInfo) == 0x18, "rl_split_info");

struct Pile {
    uint8_t* inst = nullptr;  // its render instance (null: a free entry); not one of the pieces' instances any more
    uint64_t key = 0;         // its building and cell
    uint32_t members = 0;
    double changed_ms = 0;    // pieces last joined or left
    double retried_ms = 0;    // its compile last asked for again
    double meshless_ms = 0;   // since when it has been without its mesh, drawn piece by piece (0: it has it)
    bool resize = false;      // pieces left: its bounds are to be redone
    // Its cell's last join stopped at the time budget: the rest of the cell joins in the next frames, without waiting
    // for the settle time (each part of a join would have put it off again: a backlog of 500 pieces took ~40 s).
    bool unfinished = false;
};
std::vector<Pile> g_piles;
std::vector<uint32_t> g_free_piles;
std::unordered_map<uint64_t, uint32_t> g_pile_at;  // building and cell -> pile
bool g_piles_on = false;
bool g_level_ending = false;  // the level's end gives every piece up: a pile goes at once with its last one
uint64_t g_piled_total = 0, g_unpiled_total = 0, g_piles_made = 0, g_unpile_failures = 0;
uint32_t g_piles_live = 0, g_piled_now = 0, g_piles_peak = 0, g_piled_peak = 0;

// How long piles go without their mesh after a change (each change deletes it; the compile brings a new one), and
// how many pieces they hold meanwhile: over the level, and over the 10 s between the buffer heap's log lines.
constexpr double kMeshlessBuckets[5] = {100, 250, 500, 1000, 2000};
struct MeshlessStats {
    uint64_t times = 0, pieces = 0, frames = 0, piece_frames = 0;
    double total_ms = 0, piece_ms = 0, longest_ms = 0;
    uint32_t buckets[6] = {}, peak = 0;
};
MeshlessStats g_meshless, g_meshless_window;
uint32_t g_meshless_now = 0;  // pieces in piles without their mesh, this frame
uint32_t g_waiting_for_pile = 0;  // at the last scan: still pieces ready for their cell's pile while it settles
void note_meshless(MeshlessStats& s, double ms, uint32_t members) {
    ++s.times;
    s.total_ms += ms;
    s.piece_ms += ms * members;
    s.pieces += members;
    s.longest_ms = std::max(s.longest_ms, ms);
    int b = 0;
    while (b < 5 && ms >= kMeshlessBuckets[b]) ++b;
    ++s.buckets[b];
}
void note_meshless_frame(MeshlessStats& s, uint32_t pieces) {
    ++s.frames;
    s.piece_frames += pieces;
    s.peak = std::max(s.peak, pieces);
}
void format_meshless(const MeshlessStats& s, char* out, size_t size) {
    snprintf(out, size, "%llu times, %.0f ms mean (%.0f weighted by the pieces), the longest %.0f ms (<100 ms %u, "
             "<250 %u, <500 %u, <1 s %u, <2 s %u, more %u); pieces drawn piece by piece in them: %.0f a frame, at "
             "most %u", static_cast<unsigned long long>(s.times), s.times ? s.total_ms / s.times : 0.0,
             s.pieces ? s.piece_ms / s.pieces : 0.0, s.longest_ms, s.buckets[0], s.buckets[1], s.buckets[2],
             s.buckets[3], s.buckets[4], s.buckets[5], s.frames ? double(s.piece_frames) / s.frames : 0.0, s.peak);
}

using ClaimFn = void(__thiscall*)(uint8_t*, int16_t*, int32_t);
ClaimFn claim_fn() { return rfg::fn<ClaimFn>(r::rl_destroyable_model_instance__claim_sub_instances); }
ClaimFn unclaim_fn() { return rfg::fn<ClaimFn>(r::rl_destroyable_model_instance__unclaim_sub_instances); }
void update_sub_instances(uint8_t* inst, SubInstanceUpdate* u, int32_t n) {
    rfg::fn<void(__thiscall*)(uint8_t*, SubInstanceUpdate*, int32_t)>(r::rl_destroyable_model_instance__update_instances)(
        inst, u, n);
}
void size_instance(uint8_t* inst) {
    rfg::fn<void(__thiscall*)(uint8_t*)>(r::rl_compound_mesh_instance__calculate_local_aabb)(inst);
    refresh_instance(inst);
}
// A compile under way must not see its instance's sub-instances change: the mesh compiler (compile_cache_data, on its
// own thread) counts the vertices and indices of the renderable sub-instances, sizes its buffers from that, then fills
// them, and it stops at abort checks in between (the handshake pauses it there). Resumed after a change, it fills
// buffers sized for the old sub-instances with the new ones, past their end: a heap corruption that crashed in
// rl_mesh_recompile_finalize (2026-10-27, piles). Only a request whose valid flag (rl_mesh_recompile_parameters +8)
// was cleared aborts at the next check. A new request clears the old one's, but rl_start_recompile_mesh_instance does
// nothing at all when its pool of requests is empty, which the piles' traffic can make it (claim, unclaim, split and
// update ask for a new compile after every change, through it). So before every change the piles make,
// the pending compile of the instance changed is cancelled here, its request named by m_recompile_parameters_p
// (+0x154) as long as the request still names the instance (+0).
// The game's own changes have the same hole (2026-10-28, Test.StressPieces in Wrecking Crew, 2 of 3 runs): when a
// building breaks, rfg_mover::create_subpieces swaps a dlod's merged sub-instance for its pieces (claim,
// set_sub_instance_is_active: more vertices, the pieces' inner faces), in the handshake while the compiler waits at a
// stop, and asks for a compile; the shatter bursts had emptied the pool, the building's compile went on with its old
// counts, and its vertices ran into the index buffer allocated right after them (the TLSF header of the next block:
// d_compile_cache's tlsf_free crashed on it). d_start_recompile cancels in that case too.
// The valid flag is only read at a stop where the compiler was paused, so clearing it is enough only for changes made
// in the handshake (the game's and the piles' are); a change made elsewhere would need the compiler paused first.
constexpr uint32_t kInstRecompileParams = 0x154, kParamsValid = 8;
void cancel_compile(uint8_t* inst) {
    uint8_t* params = *reinterpret_cast<uint8_t* const*>(inst + kInstRecompileParams);
    if (params && *reinterpret_cast<uint8_t* const*>(params) == inst)
        InterlockedExchange(reinterpret_cast<volatile LONG*>(params + kParamsValid), 0);
}

// compile_cache_data(rl_mesh_recompile_parameters*), on the compiler's thread: it takes the mesh's vertices and
// indices from rl_init_params::m_pBufferAllocator (Init_params +0x14C: the 76 MB TLSF heap 'RFG_VramMempoolProvider'),
// makes immutable D3D11 buffers of them (keen::graphics::initializeRenderGeometry, which copies them) and keeps them
// as long as the mesh lives, read by nothing (rl_mesh_recompile_data +0x44 indices, +0x48 vertices: only the
// destructors and rl_mesh_recompile_finalize free them, and tlsf_free takes null). When the heap has no room the
// compile fails without a word and the instance stays without a mesh. With piles the copies filled it: 46 MB for 480
// piles, and the 19 largest piles (5,300 pieces, up to 500 a pile) failed again and again and drew piece by piece
// (2026-10-27). So with piles the copies go as soon as the mesh is made. Counted: meshes made, compiles cancelled
// (their request's valid flag cleared), compiles that failed.
bool g_free_mesh_copies = false;
volatile LONG g_compiles_made = 0, g_compiles_cancelled = 0, g_compiles_failed = 0;
uint8_t* buffer_allocator() {
    return *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(rfg::addr(r::Init_params)) + 0x14C);
}
void*(__fastcall* o_compile_cache)(uint8_t*, void*, uint8_t*) = nullptr;
void* __fastcall d_compile_cache(uint8_t* inst, void* edx, uint8_t* params) {
    uint8_t* data = static_cast<uint8_t*>(o_compile_cache(inst, edx, params));
    if (data) {
        InterlockedIncrement(&g_compiles_made);
        if (g_free_mesh_copies) {
            uint8_t* a = buffer_allocator();
            using FreeFn = void(__thiscall*)(void*, void*);  // keen::MemoryAllocator::free, the third virtual
            const FreeFn free_fn = reinterpret_cast<const FreeFn*>(*reinterpret_cast<void* const*>(a))[2];
            for (const uint32_t at : {0x44u, 0x48u}) {
                void*& copy = *reinterpret_cast<void**>(data + at);
                free_fn(a, copy);
                copy = nullptr;
            }
        }
    } else if (!*reinterpret_cast<const volatile LONG*>(params + kParamsValid)) {
        InterlockedIncrement(&g_compiles_cancelled);
    } else {
        InterlockedIncrement(&g_compiles_failed);
    }
    return data;
}

// The buffer heap, for the log: its RTTI type name (a keen BaseMemoryAllocator: TLSF or zone), its name (+0x3C) and,
// for the TLSF one, its block (+0xC8) and its blocks walked (tlsf_walk_heap over TlsfAllocator::m_pPool, +0xC4) under
// the allocator's own mutex (+4), as its allocate and free take it; it keeps no count of its own.
struct HeapWalk {
    uint64_t used = 0, free = 0;
    uint32_t blocks = 0, largest_free = 0;
};
void __cdecl heap_walker(void*, size_t size, int used, void* user) {
    HeapWalk& w = *static_cast<HeapWalk*>(user);
    ++w.blocks;
    if (used) {
        w.used += size;
    } else {
        w.free += size;
        w.largest_free = std::max(w.largest_free, uint32_t(size));
    }
}
bool walk_pool(void* pool, HeapWalk& w) {
    __try {
        rfg::fn<void(__cdecl*)(void*, void(__cdecl*)(void*, size_t, int, void*), void*)>(r::tlsf_walk_heap)(
            pool, &heap_walker, &w);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}
bool heap_names(const uint8_t* a, char (&kind)[48], char (&name)[64], uint32_t& size) {
    __try {
        const uint8_t* const* vtable = *reinterpret_cast<const uint8_t* const* const*>(a);
        const uint8_t* col = vtable[-1];  // the RTTI complete object locator: its type descriptor at +0x0C
        const char* type = reinterpret_cast<const char*>(*reinterpret_cast<const uint8_t* const*>(col + 0x0C) + 8);
        strncpy_s(kind, type, _TRUNCATE);
        strncpy_s(name, reinterpret_cast<const char*>(a + 0x3C), _TRUNCATE);
        size = strstr(type, "Tlsf") ? *reinterpret_cast<const uint32_t*>(a + 0xCC) : 0;
        return true;
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

void log_buffer_heap(const char* when) {
    uint8_t* a = buffer_allocator();
    char kind[48] = "?", name[64] = "?";
    uint32_t size = 0;
    if (!a || !heap_names(a, kind, name, size)) {
        LOG("debris offload: buffer heap (%s): unreadable", when);
        return;
    }
    HeapWalk w;
    bool walked = false;
    if (size) {
        void* mutex = a + 4;
        rfg::fn<bool(__thiscall*)(void*, uint32_t)>(r::keen__Mutex__lock)(mutex, 0);
        walked = walk_pool(*reinterpret_cast<void**>(a + 0xC4), w);
        rfg::fn<void(__thiscall*)(void*)>(r::keen__Mutex__unlock)(mutex);
    }
    char use[128] = "not walked";
    if (walked)
        snprintf(use, sizeof(use), "%.1f MB used, %.1f MB free (the largest free block %.2f MB), %u blocks",
                 w.used / 1048576.0, w.free / 1048576.0, w.largest_free / 1048576.0, w.blocks);
    uint32_t meshless = 0;
    for (const Pile& pile : g_piles)
        meshless += pile.inst && !*reinterpret_cast<void* const*>(pile.inst + kInstRecompileData);
    char window[320];
    format_meshless(g_meshless_window, window, sizeof(window));
    g_meshless_window = MeshlessStats{};
    LOG("debris offload: buffer heap (%s): %s '%s' of %.1f MB: %s; compiles: %ld meshes made, %ld cancelled, %ld "
        "failed; meshes' own copies %s; %u of %u piles without a mesh now (%u pieces); since the last line, piles "
        "without their mesh: %s; %u still pieces waiting for their cell's pile to settle", when, kind, name,
        size / 1048576.0, use, g_compiles_made, g_compiles_cancelled, g_compiles_failed,
        g_free_mesh_copies ? "freed once uploaded" : "kept", meshless, g_piles_live, g_meshless_now, window,
        g_waiting_for_pile);
}

bool compile_wanted(const uint8_t* inst);
// After a pile's sub-instances changed: claim, unclaim, split and update delete the instance's mesh and ask for a new
// compile themselves; a pile with neither a mesh nor a pending request (the game's pool of requests was empty) gets
// one here (nothing happens while the pool is empty: piles_frame asks again later). Called from the render task, in
// the handshake, as the game calls it (the bool: false).
uint32_t g_pile_compiles = 0, g_pile_compile_retries = 0;
void compile_pile(uint8_t* inst) {
    if (!compile_wanted(inst)) return;
    ++g_pile_compiles;
    (o_start_recompile ? o_start_recompile
                       : rfg::fn<void(__cdecl*)(const void*, bool)>(r::rl_start_recompile_mesh_instance))(inst, false);
}

// Whether the instance has a mesh or a compile that will make one.
bool compile_wanted(const uint8_t* inst) {
    if (*reinterpret_cast<void* const*>(inst + kInstRecompileData)) return false;
    const uint8_t* params = *reinterpret_cast<uint8_t* const*>(inst + kInstRecompileParams);
    return !params || *reinterpret_cast<const uint8_t* const*>(params) != inst ||
           !*reinterpret_cast<const volatile LONG*>(params + kParamsValid);
}
void set_piece_instance(const uint8_t* inst, bool piece) {  // a piece's instance gets no compiled mesh, a pile's does
    if (!g_drop_meshes) return;
    AcquireSRWLockExclusive(&g_piece_set_lock);
    if (piece)
        g_piece_set.insert(inst);
    else
        g_piece_set.erase(inst);
    ReleaseSRWLockExclusive(&g_piece_set_lock);
}

// A piece of its own (one sub-instance, in its own instance): what it needs to leave a pile again later.
void remember_own(Piece& p) {
    const uint8_t* sub = *reinterpret_cast<uint8_t* const*>(p.inst + kInstSubArray);
    const uint8_t* base = *reinterpret_cast<uint8_t* const*>(p.inst + kInstShared);
    const uint8_t* all = *reinterpret_cast<uint8_t* const*>(base + kBaseSubArray);
    const uint16_t* orig = *reinterpret_cast<const uint16_t* const*>(base + kBaseOrigIndex);
    const M43 m = sub_instance_matrix(sub);
    memcpy(p.own_sub, m.r, sizeof(m.r));
    memcpy(p.own_sub + 9, m.t, sizeof(m.t));
    p.sub_index = orig[(sub - all) / kSubSize];
    p.submesh = *reinterpret_cast<const uint16_t*>(sub + kSubSubmesh);
    p.disable_deferred = (*reinterpret_cast<const uint32_t*>(sub + kSubFlags) & kSubDisableDeferred) != 0;
}

SubInstanceUpdate sub_update(const Piece& p, const M43& m) {
    SubInstanceUpdate u{};
    memcpy(u.m, m.r, sizeof(m.r));
    memcpy(u.m + 9, m.t, sizeof(m.t));
    u.submesh_id = p.submesh;
    u.flags = 1u | (p.disable_deferred ? 2u : 0u);
    u.base_index = p.sub_index;
    return u;
}

M43 own_matrix(const Piece& p) {
    M43 m;
    memcpy(m.r, p.own_sub, sizeof(m.r));
    memcpy(m.t, p.own_sub + 9, sizeof(m.t));
    return m;
}

void remember_piled_pose(Piece& p, const ipc::BodyPose& b) {
    p.piled_pos[0] = b.pos.x, p.piled_pos[1] = b.pos.y, p.piled_pos[2] = b.pos.z;
    p.piled_q[0] = b.rot.x, p.piled_q[1] = b.rot.y, p.piled_q[2] = b.rot.z, p.piled_q[3] = b.rot.w;
}

bool moved_from_pile(const Piece& p, const ipc::BodyPose& b) {
    const float dx = b.pos.x - p.piled_pos[0], dy = b.pos.y - p.piled_pos[1], dz = b.pos.z - p.piled_pos[2];
    if (dx * dx + dy * dy + dz * dz > kUnpileMove * kUnpileMove) return true;
    const float dot = b.rot.x * p.piled_q[0] + b.rot.y * p.piled_q[1] + b.rot.z * p.piled_q[2] + b.rot.w * p.piled_q[3];
    return std::fabs(dot) < kUnpileTurnCos;
}

void free_pile(uint32_t id) {
    Pile& pile = g_piles[id];
    const auto at = g_pile_at.find(pile.key);
    if (at != g_pile_at.end() && at->second == id) g_pile_at.erase(at);
    pile = Pile{};
    g_free_piles.push_back(id);
    --g_piles_live;
}

void pile_lost_one(uint32_t id, double now_ms) {
    Pile& pile = g_piles[id];
    --g_piled_now;
    if (--pile.members == 0) {
        release_instance(pile.inst);  // the sub-instances it still has go back to their building with it
        free_pile(id);
        return;
    }
    if (g_level_ending) return;
    pile.changed_ms = now_ms;
    pile.resize = true;
    compile_pile(pile.inst);
}

// Frozen pieces of the pile's building and cell join it: each one's sub-instance leaves its own instance (released),
// and the pile claims and places them all at once. The pile's frame is that of the piece it was made of. Each unclaim
// moves the building's whole sub-instance array (128 pieces into one pile took 6-7 ms), so past deadline_ms (session
// time; 0: none) the rest wait for the next scan: returns how many joined, at least one.
size_t join_pile(uint32_t id, const uint32_t* slots, size_t n, const ipc::BodyPose* poses, double now_ms,
                 double deadline_ms) {
    Pile& pile = g_piles[id];
    const M43 to_pile = inverse(instance_matrix(pile.inst));
    static std::vector<int16_t> ids;
    static std::vector<SubInstanceUpdate> updates;
    ids.clear();
    updates.clear();
    const ClaimFn unclaim = unclaim_fn();
    for (size_t k = 0; k < n; ++k) {
        if (k && deadline_ms > 0 && session::since_start_ms() > deadline_ms) {
            n = k;
            break;
        }
        Piece& p = g_pieces[slots[k]];
        uint8_t* own = p.inst;
        remember_own(p);
        remember_piled_pose(p, poses[slots[k]]);
        updates.push_back(sub_update(p, mul(own_matrix(p), mul(instance_matrix(own), to_pile))));
        int16_t sub = int16_t(p.sub_index);
        ids.push_back(sub);
        cancel_compile(own);
        unclaim(own, &sub, 1);  // its instance is empty now
        set_piece_instance(own, false);
        release_instance(own);
        p.inst = pile.inst;
        p.pile = id;
        p.candidate = 0;
        g_pile_candidates.erase(slots[k]);
    }
    cancel_compile(pile.inst);
    claim_fn()(pile.inst, ids.data(), int32_t(ids.size()));
    update_sub_instances(pile.inst, updates.data(), int32_t(updates.size()));
    size_instance(pile.inst);
    compile_pile(pile.inst);
    pile.members += uint32_t(n);
    pile.changed_ms = now_ms;
    g_piled_now += uint32_t(n);
    g_piled_total += n;
    return n;
}

// A pile of the frozen pieces in slots: the first one's instance becomes the pile's (its own sub-instance stays where
// it is), the others join it (as join_pile, until deadline_ms), and its compiled mesh is made in the background.
// Returns how many pieces it took.
size_t make_pile(uint64_t key, const uint32_t* slots, size_t n, const ipc::BodyPose* poses, double now_ms,
                 double deadline_ms) {
    uint32_t id;
    if (!g_free_piles.empty()) {
        id = g_free_piles.back();
        g_free_piles.pop_back();
    } else {
        id = uint32_t(g_piles.size());
        g_piles.emplace_back();
    }
    Piece& first = g_pieces[slots[0]];
    remember_own(first);
    remember_piled_pose(first, poses[slots[0]]);
    Pile& pile = g_piles[id];
    pile.inst = first.inst;
    pile.key = key;
    pile.members = 1;
    first.pile = id;
    first.candidate = 0;
    g_pile_candidates.erase(slots[0]);
    set_piece_instance(pile.inst, false);
    g_pile_at[key] = id;
    ++g_piles_live;
    ++g_piles_made;
    ++g_piled_now;
    ++g_piled_total;
    return 1 + join_pile(id, slots + 1, n - 1, poses, now_ms, deadline_ms);
}

// PhysX woke a piece in a pile: the game splits its sub-instance off into an instance of its own again (in the pile's
// frame), which gets the piece's own transform back and goes where PhysX has it.
bool unpile(uint32_t slot, const ipc::BodyPose& b, double now_ms) {
    Piece& p = g_pieces[slot];
    const uint32_t id = p.pile;
    int16_t sub = int16_t(p.sub_index);
    RlSplitInfo info{1, &sub, 0, nullptr, 0, nullptr};
    cancel_compile(g_piles[id].inst);
    uint8_t* inst =
        rfg::fn<uint8_t*(__thiscall*)(uint8_t*, RlSplitInfo*)>(r::rl_destroyable_model_instance__split)(g_piles[id].inst,
                                                                                                         &info);
    if (!inst) {
        ++g_unpile_failures;
        return false;
    }
    cancel_compile(inst);  // the one the new instance asked for: a piece is drawn batched, without a mesh
    set_piece_instance(inst, true);  // before the update, which asks for another (skipped for pieces)
    SubInstanceUpdate u = sub_update(p, own_matrix(p));
    update_sub_instances(inst, &u, 1);
    rfg::fn<void(__thiscall*)(uint8_t*)>(r::rl_compound_mesh_instance__calculate_local_aabb)(inst);
    if (g_batch) *reinterpret_cast<uint16_t*>(inst + kInstFlags) |= kInstBatchable;
    p.inst = inst;
    p.pile = kNoPile;
    p.settle = 1;
    const float pos[3] = {b.pos.x, b.pos.y, b.pos.z}, q[4] = {b.rot.x, b.rot.y, b.rot.z, b.rot.w};
    move_instance(inst, pos, q);
    pile_lost_one(id, now_ms);
    ++g_unpiled_total;
    return true;
}

// give_up() of a piece in a pile: its sub-instance goes back to its building (drawn no more), the pile stays for the
// others. The caller clears the slot.
void leave_pile(uint32_t slot) {
    Piece& p = g_pieces[slot];
    const uint32_t id = p.pile;
    p.pile = kNoPile;
    Pile& pile = g_piles[id];
    if (pile.members > 1 && !g_level_ending) {
        int16_t sub = int16_t(p.sub_index);
        cancel_compile(pile.inst);
        unclaim_fn()(pile.inst, &sub, 1);
    }
    pile_lost_one(id, session::since_start_ms());
}

// The pile of a cell's building in one of the 26 cells around it that takes pieces now (the one with the most pieces),
// or kNoPile; busy: there is one, but it changed too recently.
std::unordered_map<uint64_t, double> g_no_pile_near;  // cell -> not looked at again before this time
uint64_t g_joined_next_door = 0;
uint32_t settled_pile_near(uint64_t key, double now_ms, bool& busy) {
    const uint64_t base = key >> 30;
    const int32_t c[3] = {int32_t(key >> 20 & 0x3FF), int32_t(key >> 10 & 0x3FF), int32_t(key & 0x3FF)};
    uint32_t best = kNoPile, most = 0;
    busy = false;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dz = -1; dz <= 1; ++dz) {
                if (!dx && !dy && !dz) continue;
                const uint64_t cell = base << 30 | uint64_t((c[0] + dx) & 0x3FF) << 20 |
                                      uint64_t((c[1] + dy) & 0x3FF) << 10 | uint64_t((c[2] + dz) & 0x3FF);
                const auto at = g_pile_at.find(cell);
                if (at == g_pile_at.end()) continue;
                const Pile& pile = g_piles[at->second];
                if (g_pile_max && pile.members >= g_pile_max) continue;
                if (now_ms - pile.changed_ms < kPileSettleMs) {
                    busy = true;
                } else if (pile.members > most) {
                    most = pile.members;
                    best = at->second;
                }
            }
    return best;
}

// Render task, after the pieces moved (move_pieces lets the woken ones out of their piles): frozen pieces join piles,
// piles that lost pieces are sized again.
void piles_frame() {
    if (!g_piles_on) return;
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    if (!s) return;
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint8_t* states = pxbridge::states(s);
    const uint32_t slots = std::min<uint32_t>(std::min<uint32_t>(s->slots, g_vehicle_first), g_slots);
    const double now_ms = session::since_start_ms();
    // Every kPileScanMs the still pieces of their own are sorted into their building's cells (`ready`); the joins go on
    // frame after frame within their time budget until the cells are done, and only then is the next scan due.
    static std::unordered_map<uint64_t, std::vector<uint32_t>> ready;
    static double next_scan_ms = 0;
    static bool resume = false;  // the last frame ran out of time: this one goes on with the scan's cells
    static size_t backlog = 0;   // pieces ready at the last scan
    const bool scan = !resume && now_ms >= next_scan_ms;
    if (scan) {
        next_scan_ms = now_ms + kPileScanMs;
        if (ready.size() > 4096) ready.clear();  // the cells of past scans
        for (auto& [key, v] : ready) v.clear();
        backlog = 0;
        // The frozen pieces of their own (move_piece lists them when they freeze; the ones that moved again, joined a
        // pile or went leave the list here).
        for (auto it = g_pile_candidates.begin(); it != g_pile_candidates.end();) {
            const uint32_t i = *it;
            Piece* p = i < slots ? &g_pieces[i] : nullptr;
            const bool still = (states[i] & ipc::kPoseAlive) && !(states[i] & ipc::kPoseAwake);
            if (!p || !p->inst || !p->candidate || p->pile != kNoPile || !still) {
                if (p) p->candidate = 0;
                it = g_pile_candidates.erase(it);
                continue;
            }
            ++it;
            if (p->fresh || i == g_player_slot || !p->frozen_since_ms || now_ms - p->frozen_since_ms < kPileFrozenMs ||
                *reinterpret_cast<const int32_t*>(p->inst + kInstNumSubs) != 1)
                continue;
            ready[merge_cell_key(*reinterpret_cast<void* const*>(p->inst + kInstShared), poses[i].pos, g_pile_cell)]
                .push_back(i);
            ++backlog;
        }
        uint32_t waiting = 0;  // pieces ready for their cell's pile while it settles
        for (const auto& [key, v] : ready) {
            if (v.empty()) continue;
            const auto at = g_pile_at.find(key);
            if (at != g_pile_at.end() && !g_piles[at->second].unfinished &&
                now_ms - g_piles[at->second].changed_ms < kPileSettleMs)
                waiting += uint32_t(v.size());
        }
        g_waiting_for_pile = waiting;
        if (g_no_pile_near.size() > 16384) g_no_pile_near.clear();
    }
    if (scan || resume) {
        const bool resumed = resume;
        resume = false;
        const double deadline_ms =
            session::since_start_ms() + (backlog > kPileBacklog ? kPileJoinBudgetBehindMs : kPileJoinBudgetMs);
        // A pile's place in a cell, a full pile's next one and the next-door joins as before; a cell done (its pieces
        // joined, or nothing to do for them until the next scan) is emptied, so a frame that goes on skips it.
        for (auto& [key, v] : ready) {
            if (v.empty()) continue;
            if (session::since_start_ms() > deadline_ms) {
                resume = true;
                break;
            }
            if (resumed)  // since the scan, pieces may have woken, gone or joined
                v.erase(std::remove_if(v.begin(), v.end(),
                                       [&](uint32_t i) {
                                           const Piece& p = g_pieces[i];
                                           return i >= slots || !p.inst || !p.candidate || p.pile != kNoPile ||
                                                  !(states[i] & ipc::kPoseAlive) || (states[i] & ipc::kPoseAwake);
                                       }),
                        v.end());
            if (v.empty()) continue;
            const auto at = g_pile_at.find(key);
            const auto room = [](uint32_t members) { return g_pile_max ? size_t(g_pile_max - members) : SIZE_MAX; };
            size_t asked = v.size(), joined = 0;
            if (at != g_pile_at.end()) {
                const uint32_t id = at->second;
                if (g_pile_max && g_piles[id].members >= g_pile_max) {
                    g_pile_at.erase(at);  // full: it keeps its pieces; the cell's next ones start another pile
                    ++g_piles_full;
                    continue;
                }
                if (!g_piles[id].unfinished && now_ms - g_piles[id].changed_ms < kPileSettleMs) {
                    v.clear();
                    continue;
                }
                asked = std::min(asked, room(g_piles[id].members));
                joined = join_pile(id, v.data(), asked, poses, now_ms, deadline_ms);
                g_piles[id].unfinished = joined < v.size();
            } else if (v.size() >= kPileMinNew) {
                asked = std::min(asked, room(0));
                joined = make_pile(key, v.data(), asked, poses, now_ms, deadline_ms);
                g_piles[g_pile_at[key]].unfinished = joined < v.size();
            } else {
                uint32_t next_door = kNoPile;
                if (g_next_door_ms > 0) {
                    double still_ms = 0;  // the longest any of them has been still
                    for (const uint32_t slot : v)
                        still_ms = std::max(still_ms, now_ms - g_pieces[slot].frozen_since_ms);
                    if (still_ms >= g_next_door_ms) {
                        double& until = g_no_pile_near[key];
                        bool busy = false;
                        if (now_ms >= until) {
                            next_door = settled_pile_near(key, now_ms, busy);
                            if (next_door == kNoPile && !busy) until = now_ms + kNoPileNearRetryMs;
                        }
                    }
                }
                if (next_door == kNoPile) {
                    v.clear();
                    continue;
                }
                asked = std::min(asked, room(g_piles[next_door].members));
                joined = join_pile(next_door, v.data(), asked, poses, now_ms, deadline_ms);
                g_joined_next_door += joined;
            }
            if (joined < asked) {  // out of time on the way: the next frame goes on with the rest
                v.erase(v.begin(), v.begin() + ptrdiff_t(joined));
                resume = true;
                break;
            }
            v.clear();
        }
    }
    uint32_t retries = 0, meshless = 0;
    const int64_t sizes_start = session::qpc();
    for (uint32_t id = 0; id < g_piles.size(); ++id) {
        Pile& pile = g_piles[id];
        if (!pile.inst) continue;
        if (pile.resize && session::ticks_to_ms(session::qpc() - sizes_start) < kPileResizeBudgetMs) {
            pile.resize = false;
            size_instance(pile.inst);
        }
        if (!*reinterpret_cast<void* const*>(pile.inst + kInstRecompileData)) {
            meshless += pile.members;
            if (!pile.meshless_ms) pile.meshless_ms = now_ms;
        } else if (pile.meshless_ms) {
            note_meshless(g_meshless, now_ms - pile.meshless_ms, pile.members);
            note_meshless(g_meshless_window, now_ms - pile.meshless_ms, pile.members);
            pile.meshless_ms = 0;
        }
        // A pile left without a mesh and without a compile (the game's pool of requests was empty, or its heap had no
        // room) asks again, once it has been still for a while, and not more often than that (its own time: the
        // change time holds joins back).
        if (retries < kPileRetriesPerFrame && now_ms - pile.changed_ms >= kPileSettleMs &&
            now_ms - pile.retried_ms >= kPileSettleMs && compile_wanted(pile.inst)) {
            ++retries;
            ++g_pile_compile_retries;
            compile_pile(pile.inst);
            pile.retried_ms = now_ms;
        }
    }
    g_meshless_now = meshless;
    note_meshless_frame(g_meshless, meshless);
    note_meshless_frame(g_meshless_window, meshless);
    g_piles_peak = std::max(g_piles_peak, g_piles_live);
    g_piled_peak = std::max(g_piled_peak, g_piled_now);
}

// Telemetry.RenderRouting: the render threads tell the pieces apart with a sorted copy of their instances.
void publish_for_routing() {
    static uint32_t frame = 0;
    if (!render_routing::enabled() || frame++ % 30) return;
    std::vector<const void*> v;
    v.reserve(g_slots);
    for (const Piece& p : g_pieces)
        if (p.inst) v.push_back(p.inst);
    std::sort(v.begin(), v.end());
    render_routing::publish_pieces(std::move(v));
}

// Test.Viewpoints 2-4: the instances go 10 km down (out of every view and shadow), in the handshake.
int g_hide_request = 0;
bool g_hidden = false;
void hide_now(int what) {
    std::unordered_set<uint8_t*> done;
    auto hide = [&](uint8_t* inst) {
        if (!inst || !done.insert(inst).second) return;
        reinterpret_cast<float*>(inst + kInstPos)[1] -= 10000.0f;
        refresh_instance(inst);
    };
    for (uint32_t s = 0; s < g_slots; ++s) {
        const Piece& p = g_pieces[s];
        if (!p.inst || s == g_player_slot) continue;
        if (p.pile != kNoPile ? (what == 1 || what == 2) : (what == 1 || what == 3)) hide(p.inst);
    }
    LOG("debris offload: test: %zu render instances (%s) put out of sight", done.size(),
        what == 1 ? "all debris" : what == 2 ? "the piles" : "the pieces of their own");
    g_hidden = true;
}

void __cdecl render_task(void*, int) {
    if (!g_on) return;
    release_chunk_refs();
    if (!pxbridge::ready() || rfg::gameseq_get_state() != kGameplay) return;
    if (g_hide_request) {
        hide_now(g_hide_request);
        g_hide_request = 0;
    }
    if (g_hidden) return;  // nothing moves the hidden instances back
    const int64_t t0 = session::qpc();
    forget_restored();
    fix_piece_instances();
    const int64_t t1 = session::qpc();
    g_unpiles_left = kUnpilesPerFrame;
    g_split_ms = 0;
    move_pieces();
    const int64_t t2 = session::qpc();
    piles_frame();
    const int64_t t3 = session::qpc();
    batch_waiting();
    recycle_far_pieces();
    const int64_t t4 = session::qpc();
    const double moves = session::ticks_to_ms(t2 - t1);
    note_task_frame({std::max(0.0, moves - g_split_ms), g_split_ms, session::ticks_to_ms(t3 - t2),
                     session::ticks_to_ms(t1 - t0) + session::ticks_to_ms(t4 - t3)});
    g_stats.move_ms += float(session::ticks_to_ms(t4 - t0));
    publish_for_routing();
}

void on_frame(int game_state) {
    if (!g_on) return;
    if (!g_task_registered) {
        g_render_task.callback = &render_task;
        rfg::fn<void(__cdecl*)(GameRenderTask*)>(r::game_render_update_render_task_register)(&g_render_task);
        g_task_registered = true;
        LOG("debris offload: pieces move in a game render task (sim/render handshake)");
    }
    if (!pxbridge::ready() || game_state != kGameplay) return;
    if (!g_tweaks_logged) {
        g_tweaks_logged = true;
        log_explosion_infos();
        const float push = rfg::global<float>(r::Character_collision_push_away_mass);
        g_push_mass = g_solid_mass_kg > 0 ? float(g_solid_mass_kg) : push > 0 ? push : 50.0f;
        LOG("debris offload: the game pushes bodies lighter than %.1f kg out of the player's way (push-away dim %.2f, "
            "impulse factor %.2f, rvec factor %.2f)", double(push),
            double(rfg::global<float>(r::Character_collision_push_away_dim)),
            double(rfg::global<float>(r::Character_collision_push_away_impulse_factor)),
            double(rfg::global<float>(r::Character_collision_push_away_rvec_factor)));
        if (g_solid)
            LOG("debris offload: pieces in PhysX of %.0f kg or more are solid to the player (Havok stand-ins near "
                "the player); lighter ones are pushed by the player's capsule", double(g_push_mass));
        if (g_diag) log_layer_table();
    }
    if (g_diag) {
        static int64_t next_census = 0;
        const int64_t now = session::qpc();
        if (now >= next_census) {
            next_census = now + int64_t(10000.0 / session::ticks_to_ms(1));
            census_general_movers();
            census_vehicles();
            census_islands();
            log_busy_proxies();
            if (g_piles_on) log_buffer_heap("now");
        }
    }
    const int64_t t0 = session::qpc();
    send_explosions();
    send_melee_sweeps();
    if (world_mirror::ready()) adopt();  // not before the pieces have a world to land on
    player_proxy();
    move_vehicles();
    update_stand_ins();
    if (g_probe) probe_frame();
    if (!g_inspect_frames.empty()) inspect_frame();
    g_stats.adopt_ms += float(session::ticks_to_ms(session::qpc() - t0));
}

// The game's pool of compile requests: s_params_pool, a pool_list of 4,096 rl_mesh_recompile_parameters (+8 their
// number, +0xC the free ones). Every change to an instance's sub-instances (claim, unclaim, update, split,
// set_sub_instance_is_active) deletes its mesh and asks for a new compile here, and the new request clears the valid
// flag of the instance's pending one, which then aborts at its next stop. With no request free the game does nothing
// at all, and the pending compile goes on with the instance as it was when it counted (see cancel_compile). Shatter
// bursts empty the pool: every new piece asks for a compile, and requests come back only once the compiler is through
// them. Counted: requests made with the pool empty, and the fewest free.
constexpr uint32_t kPoolSize = 8, kPoolFree = 0xC;
uint32_t g_requests_pool_empty = 0;
int32_t g_pool_free_min = INT32_MAX;

// rl_start_recompile_mesh_instance(rl_destroyable_model_instance*, bool): no compiled meshes for the pieces; with the
// pool empty, the instance's pending compile is cancelled as a new request would.
void __cdecl d_start_recompile(const void* inst, bool in_handshake) {
    if (g_drop_meshes) {
        AcquireSRWLockShared(&g_piece_set_lock);
        const bool piece = g_piece_set.count(inst) != 0;
        ReleaseSRWLockShared(&g_piece_set_lock);
        if (piece) {
            ++g_recompiles_skipped;
            return;
        }
    }
    const int32_t free_requests = rfg::global<int32_t>(r::s_params_pool + kPoolFree);
    g_pool_free_min = std::min(g_pool_free_min, free_requests);
    if (free_requests <= 0) {
        ++g_requests_pool_empty;
        cancel_compile(static_cast<uint8_t*>(const_cast<void*>(inst)));
    }
    o_start_recompile(inst, in_handshake);
}

// ---- explosions ----

// explosion_create(explosion_info*, object* source, object* target, vector* pos, matrix* orient, vector* dir,
// weapon_info*, bool): every explosion the game makes (rockets: projectile::detonate; weapons, vehicles, items,
// scripts) is queued here and blown up in explosion_do_frame. It also kicks the host's pieces, by a velocity change
// falling off with distance. (The first version hooked explosion_apply_damage, which the PDB calls __cdecl but whose
// optimized build takes the explosion in edi and two objects in ecx/edx: the hook read garbage and nothing ever
// reached the host.) Queued here, sent at the end of the frame by the sim thread, which owns the command ring.
// The kick at the centre: 15 m/s for the thermobaric rocket (impulse 40,000; the game's own kicks on Havok debris
// top out at 15 m/s, logged below), scaled by the square root of the explosion's impulse (rocket 28,000: 12.5 m/s,
// grenade 10,000: 7.5, vehicle bomb 75,000: 20.5), 4 to 25 m/s.
constexpr float kKickAtCentre = 15.0f, kKickImpulse = 40000.0f, kKickMin = 4.0f, kKickMax = 25.0f;
// explosion_info (PDB): m_name at +0, m_radius +0x2C, m_secondary_radius +0x30, m_impulse_magnitude +0x5C,
// expanding_explosion_duration +0x64 and _delay +0x68 (ms).
constexpr uint32_t kExplRadius = 0x2C, kExplSecondaryRadius = 0x30, kExplImpulse = 0x5C, kExplExpandMs = 0x64;
struct PendingExplosion {
    float pos[3];
    float radius;
    float kick;  // m/s at the centre
};
CRITICAL_SECTION g_expl_lock;
std::vector<PendingExplosion> g_expl_pending;
uint32_t g_expl_forwarded = 0, g_expl_at_stand_ins = 0;
std::atomic<bool> g_expl_kicks{true};  // kick_pieces_for_test

void(__cdecl* o_explosion)(void*, void*, void*, float*, void*, float*, void*, bool) = nullptr;
void __cdecl d_explosion(void* info, void* source, void* target, float* pos, void* orient, float* dir, void* weapon,
                         bool flag) {
    o_explosion(info, source, target, pos, orient, dir, weapon, flag);
    if (!g_on || !pos || !info || !g_expl_kicks.load(std::memory_order_relaxed)) return;
    const auto* in = static_cast<const uint8_t*>(info);
    const float r1 = *reinterpret_cast<const float*>(in + kExplRadius);
    const float r2 = *reinterpret_cast<const float*>(in + kExplSecondaryRadius);
    float radius = std::max(r1, r2);
    if (!(radius > 0.5f) || radius > 100.0f) radius = 5.0f;  // NaN-safe
    const float impulse = *reinterpret_cast<const float*>(in + kExplImpulse);
    float kick = impulse > 0 ? kKickAtCentre * std::sqrt(impulse / kKickImpulse) : kKickAtCentre;
    kick = !(kick > kKickMin) ? kKickMin : kick > kKickMax ? kKickMax : kick;
    EnterCriticalSection(&g_expl_lock);
    if (g_expl_pending.size() < 256) g_expl_pending.push_back({{pos[0], pos[1], pos[2]}, radius, kick});
    LeaveCriticalSection(&g_expl_lock);
}

void send_explosions() {
    static std::vector<PendingExplosion> batch;
    EnterCriticalSection(&g_expl_lock);
    batch.swap(g_expl_pending);
    LeaveCriticalSection(&g_expl_lock);
    for (const PendingExplosion& p : batch) {
        // One that went off at a stand-in (within 60 cm of its piece's bounds): a projectile that struck a pile.
        if (stand_in_piece_at(p.pos, 0.6f) != kNoSlot) ++g_expl_at_stand_ins;
        ipc::Explosion e{};
        e.h = {uint16_t(ipc::Cmd::Explosion), 0, sizeof(e)};
        e.center = {p.pos[0], p.pos[1], p.pos[2]};
        e.radius = p.radius;
        e.impulse = p.kick;
        e.up = 0.3f;
        e.mode = ipc::kExplosionVelocityChange;
        if (pxbridge::send_record(&e, sizeof(e))) ++g_expl_forwarded;
    }
    batch.clear();
}

// The game's explosion definitions (Explosion_infos, Num_explosion_infos), once, to calibrate the kicks.
void log_explosion_infos() {
    const int n = rfg::global<int>(r::Num_explosion_infos);
    const auto* infos = static_cast<const uint8_t*>(rfg::addr(r::Explosion_infos));
    LOG("debris offload: %d explosion definitions (radius / secondary radius m, impulse, expanding ms):", n);
    for (int i = 0; i < n && i < 79; ++i) {  // the array holds 79 (18,960 bytes)
        const uint8_t* e = infos + size_t(i) * 0xF0;
        char name[33];
        memcpy(name, e, 32);
        name[32] = 0;
        LOG("debris offload:   %-32s %6.1f %6.1f %10.1f %6u", name, double(*reinterpret_cast<const float*>(e + kExplRadius)),
            double(*reinterpret_cast<const float*>(e + kExplSecondaryRadius)),
            double(*reinterpret_cast<const float*>(e + kExplImpulse)), *reinterpret_cast<const uint32_t*>(e + kExplExpandMs));
    }
}

// The game's own kicks for pieces still in Havok, to calibrate kKickAtCentre: the velocity change
// object_mover_process_explosion_hit gives a piece (|impulse| / mass).
double g_kick_sum = 0, g_kick_max = 0;
uint32_t g_kick_n = 0;
void(__cdecl* o_impulse)(uint32_t, const float*) = nullptr;
void __cdecl d_impulse(uint32_t handle, const float* impulse) {
    const uint32_t caller = uint32_t(reinterpret_cast<uintptr_t>(_ReturnAddress()) - rfg::base());
    if (impulse && handle != 0xFFFFFFFFu &&
        caller - r::object_mover_process_explosion_hit < rfg::size::object_mover_process_explosion_hit) {
        const uint8_t* body = static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies))[handle];
        if (body) {
            const double dv = std::sqrt(double(impulse[0]) * impulse[0] + double(impulse[1]) * impulse[1] +
                                        double(impulse[2]) * impulse[2]) *
                              double(*reinterpret_cast<const float*>(body + kBodyInvMass));
            g_kick_sum += dv;
            g_kick_max = std::max(g_kick_max, dv);
            if (++g_kick_n % 2000 == 0)
                LOG("debris offload: the game's explosion kicks so far: %u, %.2f m/s mean, %.2f max", g_kick_n,
                    g_kick_sum / g_kick_n, g_kick_max);
        }
    }
    o_impulse(handle, impulse);
}

// ---- zones and chunks streaming out ----

// world::destream_zone(world_zone*): a zone leaves (from world::process_deserialization in world::do_frame, in the
// handshake; the game serialized and destroyed its objects a few frames earlier, those then lying in it judged by
// position, world::process_deserialize_collect). Its obj_zone is destroyed here and its containers released; the
// pieces lying in it go now, as the game's debris did, and their chunk references with them, so the chunks can unload.
// Stand-ins carrying its zone object go first (they are made again next frame with the player's zone).
uint32_t g_zones_out = 0;
uint64_t g_zone_pieces_released = 0;

void release_zone(uint8_t* w, uint8_t* zone) {
    uint8_t* zone_obj = *reinterpret_cast<uint8_t**>(zone + kZoneObj);
    if (zone_obj && std::find(g_stand_in_zones.begin(), g_stand_in_zones.end(), zone_obj) != g_stand_in_zones.end())
        free_all_stand_ins();
    if (zone_obj == g_zone_obj) g_zone_obj = nullptr;
    if (*reinterpret_cast<const int32_t*>(zone + kZoneState) != kZoneLoaded) return;  // a load called off
    const auto zone_at = rfg::fn<uint8_t*(__thiscall*)(void*, const float*)>(r::world__get_world_zone_by_pos);
    uint32_t n = 0, live = 0;
    for (uint32_t s = 0; s < g_slots; ++s) {
        const Piece& p = g_pieces[s];
        if (!p.inst || s == g_player_slot) continue;
        ++live;
        if (zone_at(w, reinterpret_cast<const float*>(p.inst + kInstPos)) == zone) {
            give_up(s, true);
            ++n;
        }
    }
    ++g_zones_out;
    g_zone_pieces_released += n;
    if (n) {
        char name[65];
        memcpy(name, zone + kZoneName, 64);
        name[64] = 0;
        LOG("debris offload: zone %s streamed out, %u of the %u pieces lay in it and went with it", name, n, live);
    }
}

void(__fastcall* o_destream_zone)(void*, void*, uint8_t*) = nullptr;
void __fastcall d_destream_zone(void* w, void* edx, uint8_t* zone) {
    if (g_on && w && zone) release_zone(static_cast<uint8_t*>(w), zone);
    o_destream_zone(w, edx, zone);
}

// chunk_base_unload_internal(c_chunk_base*, bool): a chunk unloads (stream2_do_frame, end of the handshake), its
// mesh and materials released, its memory given back with its container. A piece still drawing from it holds no
// reference (its general_mover had none): it goes first, nothing to give back.
uint32_t g_chunks_out_under_pieces = 0;
uint64_t g_chunk_pieces_released = 0;

void(__cdecl* o_chunk_unload)(uint8_t*, bool) = nullptr;
void __cdecl d_chunk_unload(uint8_t* chunk, bool delete_mesh) {
    if (g_on && chunk && !g_chunk_pieces.empty()) {
        const uint32_t uid = rfg::fn<uint32_t(__cdecl*)(uint8_t*)>(r::chunk_get_base_uid)(chunk);
        if (g_chunk_pieces.count(uid)) {
            uint32_t n = 0;
            for (uint32_t s = 0; s < g_slots; ++s) {
                Piece& p = g_pieces[s];
                if (!p.inst || p.chunk_uid != uid) continue;
                if (p.srid) g_srid_pieces.erase(p.srid);  // our reference goes with the container
                p.srid = 0;
                give_up(s, true);
                ++n;
            }
            // Their stand-ins' shapes are the pieces' own, which may live in the chunk's memory: they go now.
            for (uint32_t h : g_stand_ins_to_free) free_stand_in(h);
            g_stand_ins_to_free.clear();
            ++g_chunks_out_under_pieces;
            g_chunk_pieces_released += n;
            char name[129];
            memcpy(name, chunk + 0x30, 128);  // c_chunk_base::name
            name[128] = 0;
            LOG("debris offload: chunk %s unloads under %u pieces: they go first", name, n);
        }
    }
    o_chunk_unload(chunk, delete_mesh);
}

// Pieces thrown out of their zone while they waited in Havok (d_in_original_zone), this session.
void log_zone_crossings() {
    uint64_t deleted = 0;
    for (const uint64_t n : g_zone_deleted) deleted += n;
    if (!g_zone_kept && !deleted) return;
    LOG("debris offload: general movers that left their zone this session: %llu waiting pieces kept, %llu of them gone "
        "to PhysX at once, %llu left to the game after %u frames; deleted by the game as before: %llu (not debris "
        "%llu, fading %llu, no render instance %llu, no body %llu, attached %llu, PhysX not ready %llu)", g_zone_kept,
        g_zone_kept_adopted, g_zone_left_to_game, kZoneKeepFrames, deleted, g_zone_deleted[kZoneNotDebris],
        g_zone_deleted[kZoneFading], g_zone_deleted[kZoneNoInstance], g_zone_deleted[kZoneNoBody],
        g_zone_deleted[kZoneAttached], g_zone_deleted[kZoneNotReady]);
}

void on_exit(bool) {
    if (!g_on) return;
    LOG("debris offload: %llu pieces taken over (%llu fell through the world); %u explosions sent to the host (%u went "
        "off at a stand-in); the game's explosion kicks: %u, %.2f m/s mean, %.2f max", g_total_adopted, g_fell,
        g_expl_forwarded, g_expl_at_stand_ins, g_kick_n, g_kick_n ? g_kick_sum / g_kick_n : 0.0, g_kick_max);
    if (g_world_only_went) LOG("debris offload: %llu world-only pieces went over at once", g_world_only_went);
    log_zone_crossings();
    if (g_adopted_faded)
        LOG("debris offload: %llu pieces went over faded by distance (not fully visible)", g_adopted_faded);
    if (g_wide_blows)
        LOG("debris offload: %u melee blows wider than the game's (a mod's moves) passed on to the pieces, in %u "
            "sweeps; %u pieces that came out of what they shattered got their push", g_wide_blows, g_sweeps_sent,
            g_late_pushes);
    if (g_batch)
        LOG("debris offload: batchable bit set again %u times (the game's compiled meshes clear it)", g_batch_restored);
    if (g_drop_meshes)
        LOG("debris offload: %u compiled meshes of pieces given back, %u recompiles skipped", g_meshes_dropped,
            g_recompiles_skipped);
    if (o_start_recompile && g_pool_free_min != INT32_MAX)
        LOG("debris offload: mesh compile requests: at least %d of the game's %d free at any time; %u asked with none "
            "free (the game drops those; the instance's pending compile was cancelled here)", g_pool_free_min,
            rfg::global<int32_t>(r::s_params_pool + kPoolSize), g_requests_pool_empty);
    if (g_recycled_total)
        LOG("debris offload: %llu pieces far from the player given up for render instances", g_recycled_total);
    if (g_restored_pieces)
        LOG("debris offload: %llu pieces went with the restored objects they broke off", g_restored_pieces);
    LOG("debris offload: chunk references taken over from the movers: %llu (%llu pieces came without one), %llu given "
        "back; at most %u pieces shared one chunk's reference; %u zones streamed out, %llu pieces went with them; %u "
        "chunks unloaded under pieces (%llu pieces)",
        g_refs_taken, g_refs_missing, g_refs_released, g_srid_peak, g_zones_out, g_zone_pieces_released,
        g_chunks_out_under_pieces, g_chunk_pieces_released);
    LOG("debris offload: Havok stand-ins: %u created (%u boxes, the rest the pieces' own shapes), %u at most at once, %u "
        "failed; wanted (piece-frames) near the player %u, ahead of projectiles %u, on the line of fire %u; blows "
        "passed on to pieces: %u melee, %u bullets", g_stand_ins_created, g_stand_ins_boxes, g_stand_ins_peak,
        g_stand_in_failures, g_wanted_near, g_wanted_projectile, g_wanted_aim, g_melee_hits, g_bullet_hits);
    if (g_probe)
        LOG("stand-in probe: %u pieces probed: the default ray hit a stand-in %u times; %u blows passed on, %u pieces "
            "moved", g_probe_rays, g_probe_ray_hits, g_probe_blows, g_probe_moved);
}

// object_level_shutdown() (from gamestate_gp_exit, when gameplay ends): the level's objects are freed, movers among
// them; the render instances taken over go now too, while the renderer still has them. (general_mover_level_shutdown
// looked like the place but nothing calls it.)
void(__cdecl* o_level_shutdown)() = nullptr;
// What the renderer sees of the pieces (research: the render thread's cost per piece). rl_compound_mesh_instance:
// m_mat_map (+0x118: rl_material_map, num_materials at +0x0C, is_set at +0x10), m_num_renderable_sub_instances
// (+0x124); rl_destroyable_model_instance::m_shared_data (+0x148): the building's shared base (its pieces share it).
// get_material_block_counts walks every material of the map and every renderable sub-instance, for each visible
// piece once per frame and again when it is batched. Pieces in piles are counted apart.
void log_render_stats() {
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    const ipc::BodyPose* poses = s ? pxbridge::poses(s) : nullptr;
    const uint32_t slots = s ? std::min<uint32_t>(s->slots, g_vehicle_first) : 0;
    uint32_t mats[8] = {}, subs[8] = {}, pieces = 0, frozen = 0, awake = 0, piled = 0;
    uint64_t mat_sum = 0, sub_sum = 0;
    std::unordered_map<const void*, uint32_t> per_base;
    // What merging frozen pieces would leave to draw: pieces of one building in one cell of 4, 8 or 16 m.
    constexpr float kCells[3] = {4.0f, 8.0f, 16.0f};
    std::unordered_map<uint64_t, uint32_t> groups[3];
    auto bucket = [](uint32_t v) { return v <= 1 ? 0 : v <= 2 ? 1 : v <= 4 ? 2 : v <= 8 ? 3 : v <= 16 ? 4 : v <= 32 ? 5 : v <= 64 ? 6 : 7; };
    for (uint32_t i = 0; i < g_vehicle_first && i < g_slots; ++i) {
        const uint8_t* inst = g_pieces[i].inst;
        if (!inst) continue;
        if (g_pieces[i].pile != kNoPile) {
            ++piled;
            continue;
        }
        ++pieces;
        const uint8_t* map = *reinterpret_cast<uint8_t* const*>(inst + kInstMatMap);
        const uint32_t m = map && map[0x10] ? *reinterpret_cast<const uint32_t*>(map + 0x0C) : 0;
        const uint32_t r = *reinterpret_cast<const uint32_t*>(inst + kInstRenderableSubs);
        ++mats[bucket(m)];
        ++subs[bucket(r)];
        mat_sum += m;
        sub_sum += r;
        const void* base = *reinterpret_cast<void* const*>(inst + kInstShared);
        ++per_base[base];
        if (poses && i < slots) {
            const bool is_frozen = (poses[i].flags & ipc::kPoseFrozen) != 0;
            frozen += is_frozen;
            awake += (poses[i].flags & ipc::kPoseAwake) != 0;
            if (is_frozen)
                for (int c = 0; c < 3; ++c) ++groups[c][merge_cell_key(base, poses[i].pos, kCells[c])];
        }
    }
    if (g_piles_live) {
        uint32_t compiled = 0, batched = 0, largest = 0;
        // rl_mesh_recompile_data: keen::RenderGeometry (indexCount +0x10), vertex_stride +0x2C and index_stride +0x2E
        // (ushort), num_verts +0x30, num_render_blocks +0x3C (one per material drawn: a draw per pass).
        uint64_t vbytes = 0, ibytes = 0, blocks = 0;
        uint32_t most_blocks = 0, meshless_members = 0, meshless_largest = 0, meshless_pending = 0;
        for (const Pile& pile : g_piles) {
            if (!pile.inst) continue;
            const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(pile.inst + kInstRecompileData);
            compiled += data != nullptr;
            batched += (*reinterpret_cast<const uint16_t*>(pile.inst + kInstFlags) & kInstBatchable) != 0;
            largest = std::max(largest, pile.members);
            if (data) {
                const uint32_t n = *reinterpret_cast<const uint32_t*>(data + 0x3C);
                vbytes += uint64_t(*reinterpret_cast<const uint32_t*>(data + 0x30)) *
                          *reinterpret_cast<const uint16_t*>(data + 0x2C);
                ibytes += uint64_t(*reinterpret_cast<const uint32_t*>(data + 0x10)) *
                          *reinterpret_cast<const uint16_t*>(data + 0x2E);
                blocks += n;
                most_blocks = std::max(most_blocks, n);
            } else {
                meshless_members += pile.members;
                meshless_largest = std::max(meshless_largest, pile.members);
                meshless_pending += !compile_wanted(pile.inst);
            }
        }
        LOG("debris offload: %u pieces drawn by %u piles at the end (%u with their compiled mesh, %u drawn batched while "
            "it is made; the largest pile %u pieces)", piled, g_piles_live, compiled, batched, largest);
        LOG("debris offload: the piles' meshes: %.1f MB of vertices, %.1f MB of indices, %.1f render blocks a pile (most "
            "%u); without a mesh: %u piles of %u pieces (largest %u), %u of them with a compile under way",
            vbytes / 1048576.0, ibytes / 1048576.0, compiled ? double(blocks) / compiled : 0.0, most_blocks,
            g_piles_live - compiled, meshless_members, meshless_largest, meshless_pending);
    }
    log_buffer_heap("level end");
    if (!pieces) return;
    uint32_t biggest = 0, in_big = 0;
    for (const auto& [base, n] : per_base) {
        biggest = std::max(biggest, n);
        if (n >= 100) in_big += n;
    }
    // Groups of one piece would stay as they are; the rest would each become one instance.
    char merge[3][96];
    for (int c = 0; c < 3; ++c) {
        uint32_t multi = 0, in_multi = 0, largest = 0;
        for (const auto& [key, n] : groups[c]) {
            largest = std::max(largest, n);
            if (n > 1) {
                ++multi;
                in_multi += n;
            }
        }
        snprintf(merge[c], sizeof(merge[c]), "%.0f m: %u groups of 2+ holding %u pieces (largest %u)", kCells[c], multi,
                 in_multi, largest);
    }
    LOG("debris offload: render view of %u pieces: materials in a piece's map %.1f mean (1: %u, 2: %u, 3-4: %u, 5-8: %u, "
        "9-16: %u, 17-32: %u, 33-64: %u, more: %u); renderable sub-instances %.2f mean (1: %u, 2: %u, 3-4: %u, 5-8: %u, "
        "more: %u); %zu shared bases (buildings), the biggest with %u pieces, %u pieces in bases of 100 or more; "
        "frozen %u, awake %u", pieces, double(mat_sum) / pieces, mats[0], mats[1], mats[2], mats[3], mats[4], mats[5],
        mats[6], mats[7], double(sub_sum) / pieces, subs[0], subs[1], subs[2], subs[3], subs[4] + subs[5] + subs[6] + subs[7],
        per_base.size(), biggest, in_big, frozen, awake);
    LOG("debris offload: frozen pieces of their own by building and cell: %s; %s; %s", merge[0], merge[1], merge[2]);
    if (!g_piles_on || !poses) return;
    // Why the still pieces of their own (frozen or asleep) are not in a pile, as piles_frame sees them.
    const uint8_t* states = pxbridge::states(s);
    const double now_ms = session::since_start_ms();
    std::unordered_map<uint64_t, uint32_t> still_in_cell;
    for (uint32_t i = 0; i < slots && i < g_slots; ++i) {
        const Piece& p = g_pieces[i];
        if (p.inst && p.pile == kNoPile && (states[i] & ipc::kPoseAlive) && !(states[i] & ipc::kPoseAwake))
            ++still_in_cell[merge_cell_key(*reinterpret_cast<void* const*>(p.inst + kInstShared), poses[i].pos,
                                           g_pile_cell)];
    }
    uint32_t still = 0, asleep = 0, no_candidate = 0, fresh = 0, not_yet = 0, subs_not_one = 0, pile_waits = 0,
             next_door = 0, too_few = 0, ready = 0;
    for (uint32_t i = 0; i < slots && i < g_slots; ++i) {
        const Piece& p = g_pieces[i];
        if (!p.inst || p.pile != kNoPile || !(states[i] & ipc::kPoseAlive) || (states[i] & ipc::kPoseAwake)) continue;
        ++still;
        asleep += !(states[i] & ipc::kPoseFrozen);
        const uint64_t key = merge_cell_key(*reinterpret_cast<void* const*>(p.inst + kInstShared), poses[i].pos,
                                            g_pile_cell);
        bool busy = false;
        if (!p.candidate) ++no_candidate;
        else if (p.fresh) ++fresh;
        else if (!p.frozen_since_ms || now_ms - p.frozen_since_ms < kPileFrozenMs) ++not_yet;
        else if (*reinterpret_cast<const int32_t*>(p.inst + kInstNumSubs) != 1) ++subs_not_one;
        else if (g_pile_at.count(key)) ++pile_waits;
        else if (still_in_cell[key] >= kPileMinNew) ++ready;
        else if (settled_pile_near(key, now_ms, busy) != kNoPile || busy) ++next_door;
        else ++too_few;
    }
    LOG("debris offload: %u still pieces of their own (%u asleep, the rest frozen): %u not listed as candidates, %u "
        "fresh, %u still for less than %.0f s, %u of other than one sub-instance, %u with a pile in their cell (joining "
        "it after its settle time), %u with fewer than %u in their cell and a pile of their building next door, %u "
        "with fewer and none next door, %u ready", still, asleep, no_candidate, fresh, not_yet, kPileFrozenMs / 1000.0,
        subs_not_one, pile_waits, next_door, kPileMinNew, too_few, ready);
}

void __cdecl d_level_shutdown() {
    if (g_on) {
        log_render_stats();
        free_all_stand_ins();  // while Havok's world is still there
        AcquireSRWLockExclusive(&g_piece_set_lock);
        g_piece_set.clear();
        ReleaseSRWLockExclusive(&g_piece_set_lock);
        EnterCriticalSection(&g_expl_lock);
        g_expl_pending.clear();
        LeaveCriticalSection(&g_expl_lock);
        g_zone_obj = nullptr;
        uint32_t n = 0;
        g_level_ending = true;
        for (uint32_t s = 0; s < g_slots; ++s)
            if (g_pieces[s].inst) {
                give_up(s, false);
                ++n;
            }
        g_level_ending = false;
        g_pile_candidates.clear();
        g_no_pile_near.clear();
        std::fill(g_seen_state.begin(), g_seen_state.end(), uint8_t(0));
        AcquireSRWLockExclusive(&g_fix_lock);
        g_fix_queue.clear();
        ReleaseSRWLockExclusive(&g_fix_lock);
        if (g_piles_made)
            LOG("debris offload: piles this level: %llu made, %llu pieces joined one (%llu of them from the next cell), "
                "%llu left one again when woken (%llu splits refused); %llu filled up; at most %u piles drawing %u "
                "pieces at once; %u compiles asked for, %u of them again (no request had been made)", g_piles_made,
                g_piled_total, g_joined_next_door, g_unpiled_total, g_unpile_failures, g_piles_full, g_piles_peak,
                g_piled_peak, g_pile_compiles, g_pile_compile_retries);
        g_joined_next_door = g_piles_full = 0;
        if (g_piles_made) {
            char meshless[320];
            format_meshless(g_meshless, meshless, sizeof(meshless));
            LOG("debris offload: piles without their mesh this level (after a change, until the new one): %s", meshless);
        }
        g_meshless = g_meshless_window = MeshlessStats{};
        g_meshless_now = g_waiting_for_pile = 0;
        if (g_task_frames) {
            const char* names[kTaskParts] = {"moves", "splits out of piles", "piles (joins, sizes, retries)",
                                             "the rest"};
            double slow_total = 0;
            for (const TaskPart& t : g_task) slow_total += t.in_slow;
            char parts[kTaskParts][112];
            for (int k = 0; k < kTaskParts; ++k)
                snprintf(parts[k], sizeof(parts[k]), "%s %.3f ms a frame (longest %.2f, %.0f%% of the slow frames)",
                         names[k], g_task[k].total / g_task_frames, g_task[k].longest,
                         slow_total > 0 ? 100.0 * g_task[k].in_slow / slow_total : 0.0);
            LOG("debris offload: render task this level: %llu frames, %llu of them over 1 ms; %s; %s; %s; %s; %llu "
                "moves skipped (the instance already there)", static_cast<unsigned long long>(g_task_frames),
                static_cast<unsigned long long>(g_task_slow), parts[0], parts[1], parts[2], parts[3],
                static_cast<unsigned long long>(g_moves_skipped));
        }
        g_moves_skipped = 0;
        for (TaskPart& t : g_task) t = TaskPart{};
        g_task_frames = g_task_slow = 0;
        g_pile_compiles = g_pile_compile_retries = 0;
        g_piles.clear();
        g_free_piles.clear();
        g_pile_at.clear();
        g_piles_live = g_piled_now = g_piles_peak = g_piled_peak = 0;
        g_piles_made = g_piled_total = g_unpiled_total = g_unpile_failures = 0;
        // Before world::level_shutdown destreams the zones and releases what the level's objects still hold.
        const size_t refs = g_refs_to_release.size();
        release_chunk_refs();
        g_chunk_pieces.clear();
        g_srid_pieces.clear();
        g_restored.clear();
        if (n) LOG("debris offload: level end, %u render instances released, %zu chunk references given back", n, refs);
        log_masses("level end");
        memset(g_mass_hist, 0, sizeof(g_mass_hist));
        pxbridge::reset();
        g_player_ready = false;
        g_shape_ids.clear();
        g_largest_radius = 0;
        if (g_adopt_delay_ms)
            LOG("debris offload: level end; at most %u new pieces were waiting in Havok at once; %u went over at rest "
                "(frozen), %u moving", g_waiting_peak, g_went_at_rest, g_went_moving);
        log_zone_crossings();
        g_first_seen.clear();
        g_waiting_list.clear();
        g_adopt_now.clear();
        g_waiting = g_waiting_peak = g_went_at_rest = g_went_moving = 0;
        EnterCriticalSection(&g_blow_lock);
        g_blows.clear();
        g_sweeps_pending.clear();  // sent before pxbridge::reset() would land in the next level
        LeaveCriticalSection(&g_blow_lock);
        if (g_vehicle_proxies_made || g_chunk_proxies_made)
            LOG("debris offload: level end; bodies pushing the pieces in the host: %u of vehicles, %u of loose building "
                "chunks (%u at most at once; %u shapes unreadable, %u too large)", g_vehicle_proxies_made,
                g_chunk_proxies_made, g_vehicle_proxies_peak, g_vehicle_shapes_failed, g_proxy_shapes_too_large);
        g_vehicle_proxies_made = g_chunk_proxies_made = g_vehicle_proxies_peak = g_vehicle_shapes_failed = 0;
        g_proxy_shapes_too_large = 0;
        reset_vehicles();
    }
    o_level_shutdown();
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.exp_physx <= 0 || !cfg.exp_physx_world || !cfg.exp_physx_debris) return true;
    g_slots = pxbridge::max_bodies();
    if (g_slots < kVehicleSlots + 1024) return true;
    g_player_slot = g_slots - 1;
    g_vehicle_first = g_player_slot - kVehicleSlots;  // the vehicles' proxies, below the player's capsule
    g_pieces.assign(g_slots, Piece{});
    g_free.clear();
    for (uint32_t s = g_vehicle_first; s-- > 0;) g_free.push_back(s);  // pop_back hands out 0, 1, 2, ...
    reset_vehicles();
    InitializeCriticalSection(&g_expl_lock);
    InitializeCriticalSection(&g_blow_lock);
    bool ok = hooks::create(r::explosion_create, &d_explosion, &o_explosion, "explosion_create");
    ok &= hooks::create(r::human_combat_process_hits, &d_combat_hits, &o_combat_hits, "human_combat_process_hits");
    ok &= hooks::create(r::havok_body_apply_linear_impulse_by_handle, &d_impulse, &o_impulse,
                        "havok_body_apply_linear_impulse(handle)");
    ok &= hooks::create(r::general_mover__update_render_opacity, &d_update_render_opacity, &o_update_render_opacity,
                        "general_mover::update_render_opacity");
    ok &= hooks::create(r::object_level_shutdown, &d_level_shutdown, &o_level_shutdown, "object_level_shutdown");
    ok &= hooks::create(r::world__destream_zone, &d_destream_zone, &o_destream_zone, "world::destream_zone");
    ok &= hooks::create(r::chunk_base_unload_internal, &d_chunk_unload, &o_chunk_unload, "chunk_base_unload_internal");
    if (!ok) return false;
    // The zone check before general_mover::update_pos_and_orient's destroy: call (E8) at +0x40, test al, al after it.
    const uint8_t* zone_call = static_cast<const uint8_t*>(rfg::addr(r::general_mover__update_pos_and_orient + 0x40));
    if (zone_call[0] == 0xE8 && zone_call[5] == 0x84 && zone_call[6] == 0xC0 &&
        hooks::create(r::world__object_physically_in_original_zone, &d_in_original_zone, &o_in_original_zone,
                      "world::object_physically_in_original_zone"))
        g_zone_check_return = zone_call + 5;
    else
        LOG("debris offload: WARNING the zone check in general_mover::update_pos_and_orient is not hooked: pieces thrown "
            "out of their zone while they wait in Havok are deleted, as in the game");
    g_on = true;
    g_batch = cfg.exp_physx_debris_batch != 0;
    g_drop_meshes = g_batch && cfg.exp_physx_debris_drop_meshes != 0;
    if (!hooks::create(r::rl_start_recompile_mesh_instance, &d_start_recompile, &o_start_recompile,
                       "rl_start_recompile_mesh_instance")) {
        LOG("debris offload: WARNING rl_start_recompile_mesh_instance not hooked: a mesh compile can outlive a change "
            "to its instance when the game's request pool is empty (heap corruption in big shatter bursts)");
        g_drop_meshes = false;
    }
    if (g_batch && !hooks::create(r::rl_destroyable_model_instance__set_compiled_cache_data, &d_set_compiled,
                                  &o_set_compiled, "set_compiled_cache_data"))
        LOG("debris offload: WARNING set_compiled_cache_data not hooked: pieces that get a compiled mesh later are drawn "
            "one by one");
    if ((cfg.exp_physx_debris_piles || cfg.telemetry) &&
        !hooks::create(r::rl_destroyable_model_instance__compile_cache_data, &d_compile_cache, &o_compile_cache,
                       "compile_cache_data"))
        LOG("debris offload: WARNING compile_cache_data not hooked: compiled meshes keep their copies in the game's "
            "buffer heap (large piles may find no room)");
    else
        g_free_mesh_copies = cfg.exp_physx_debris_piles != 0;
    g_solid = cfg.exp_physx_solid_debris != 0;
    g_solid_mass_kg = cfg.exp_physx_solid_mass_kg;
    g_adopt_delay_ms = uint32_t(std::max(0, cfg.exp_physx_adopt_delay_ms));
    if (g_adopt_delay_ms)
        LOG("debris offload: new pieces stay in Havok for %u ms before they go to PhysX (less with more than %u "
            "waiting)", g_adopt_delay_ms, kMaxWaiting);
    g_world_only_at_once = cfg.exp_physx_world_only_at_once;
    if (g_adopt_delay_ms && g_world_only_at_once) LOG("debris offload: world-only pieces go to PhysX at once");
    g_diag = cfg.telemetry;
    g_probe = cfg.test_stand_in_probe;
    g_piles_on = cfg.exp_physx_debris_piles != 0;
    g_pile_cell = float(std::clamp(cfg.exp_physx_debris_pile_cell_m, 2, 64));
    g_next_door_ms = 1000.0 * std::max(0, cfg.exp_physx_debris_pile_next_door_s);
    g_pile_max = uint32_t(std::max(0, cfg.exp_physx_debris_pile_max_pieces));
    if (g_piles_on)
        LOG("debris offload: piles: pieces still for %.0f s (frozen or asleep) are drawn with the other still pieces of "
            "their building in their %.0f m cell, as one compiled instance (from %u in a cell, up to %u a pile, 0: "
            "no limit; fewer join their building's pile next door after %.0f s, 0: never); a piece that moves "
            "leaves its pile again", kPileFrozenMs / 1000.0, double(g_pile_cell), kPileMinNew, g_pile_max,
            g_next_door_ms / 1000.0);
    g_stand_in_drives_zone = cfg.test_stand_in_drives_zone;
    for (size_t at = 0; at < cfg.test_inspect_frames.size();) {
        const size_t comma = cfg.test_inspect_frames.find(L',', at);
        const std::wstring item = cfg.test_inspect_frames.substr(at, comma == std::wstring::npos ? std::wstring::npos : comma - at);
        if (!item.empty()) g_inspect_frames.push_back(_wtoi(item.c_str()));
        if (comma == std::wstring::npos) break;
        at = comma + 1;
    }
    if (g_solid && !patch_lean_check()) {
        LOG("debris offload: unexpected code in MyCharacterListener::processConstraintsCallback; no solid stand-ins");
        g_solid = false;
    }
    if (!patch_zone_hits()) LOG("debris offload: obj_zone's vtable is not as expected; blows on stand-ins do nothing");
    session::add_frame_callback(&on_frame);  // adoption; the moves run in a game render task (registered there)
    session::add_flush_handler(&on_exit);
    LOG("debris offload armed: debris pieces go to the PhysX host (%u slots), the player is a kinematic capsule there, "
        "vehicles push the pieces (%u slots)%s%s", g_vehicle_first, kVehicleSlots,
        g_batch ? "; their render instances are drawn through the batched pass" : "",
        g_solid ? "; heavy pieces are solid to the player (Havok stand-ins), the capsule pushes light ones" : "");
    return true;
}

FrameStats take_frame() {
    FrameStats f = g_stats;
    f.live = uint32_t(g_vehicle_first - g_free.size());
    g_stats = {};
    return f;
}

std::vector<uint32_t> pieces_near(const float center[3], float radius) {
    std::vector<uint32_t> out;
    const ipc::SnapshotHeader* s = g_on ? pxbridge::snapshot(nullptr) : nullptr;
    if (!s) return out;
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint32_t slots = std::min<uint32_t>(s->slots, g_vehicle_first);
    for (uint32_t i = 0; i < slots; ++i) {
        if (!g_pieces[i].inst || !(poses[i].flags & ipc::kPoseAlive)) continue;
        const float dx = poses[i].pos.x - center[0], dy = poses[i].pos.y - center[1], dz = poses[i].pos.z - center[2];
        if (dx * dx + dy * dy + dz * dz < radius * radius) out.push_back(i);
    }
    return out;
}

void forget_pieces_of(const std::unordered_set<uint32_t>& object_handles) {
    if (!g_on) return;
    for (const uint32_t handle : object_handles)
        if (handle != kNoObject) g_restored.insert(handle);
}

uint32_t piece_original(uint32_t slot) {
    return g_on && slot < g_slots && g_pieces[slot].inst ? g_pieces[slot].original : kNoObject;
}

const std::unordered_set<uint32_t>& last_restored() { return g_last_restored; }

uint32_t count_pieces_of(uint32_t object_handle) {
    uint32_t n = 0;
    for (uint32_t s = 0; g_on && s < g_slots; ++s) n += g_pieces[s].inst && g_pieces[s].original == object_handle;
    return n;
}

bool piece_position(uint32_t slot, float out[3]) {
    const ipc::SnapshotHeader* s = g_on ? pxbridge::snapshot(nullptr) : nullptr;
    if (!s || slot >= g_vehicle_first || !g_pieces[slot].inst) return false;
    const ipc::BodyPose& b = pxbridge::poses(s)[slot];
    out[0] = b.pos.x, out[1] = b.pos.y, out[2] = b.pos.z;
    return true;
}

void hide_debris_for_test(int what) { g_hide_request = what; }

void kick_pieces_for_test(bool on) { g_expl_kicks.store(on, std::memory_order_relaxed); }

Census census() {
    if (!g_on) return {};
    Census c{uint32_t(g_vehicle_first - g_free.size()), g_vehicle_first, g_piles_live, g_piled_now, g_waiting_for_pile,
             uint32_t(g_pile_candidates.size()), g_piled_total, g_unpiled_total, g_recycled_total, g_task_frames, {}};
    for (int k = 0; k < kTaskParts; ++k) c.task_ms[k] = g_task[k].total;
    return c;
}

bool debris_extent(float centre[3], float& radius, float& low_y, uint32_t& pieces) {
    const ipc::SnapshotHeader* s = g_on ? pxbridge::snapshot(nullptr) : nullptr;
    if (!s) return false;
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint32_t slots = std::min<uint32_t>(std::min<uint32_t>(s->slots, g_vehicle_first), g_slots);
    double sum[3] = {};
    std::vector<float> ys;
    for (uint32_t i = 0; i < slots; ++i) {
        if (!g_pieces[i].inst || i == g_player_slot || !(poses[i].flags & ipc::kPoseAlive)) continue;
        sum[0] += poses[i].pos.x, sum[1] += poses[i].pos.y, sum[2] += poses[i].pos.z;
        ys.push_back(poses[i].pos.y);
    }
    pieces = uint32_t(ys.size());
    if (!pieces) return false;
    for (int k = 0; k < 3; ++k) centre[k] = float(sum[k] / pieces);
    std::vector<float> d;
    d.reserve(pieces);
    for (uint32_t i = 0; i < slots; ++i) {
        if (!g_pieces[i].inst || i == g_player_slot || !(poses[i].flags & ipc::kPoseAlive)) continue;
        const float dx = poses[i].pos.x - centre[0], dz = poses[i].pos.z - centre[2];
        d.push_back(std::sqrt(dx * dx + dz * dz));
    }
    std::nth_element(d.begin(), d.begin() + d.size() * 8 / 10, d.end());
    radius = d[d.size() * 8 / 10];
    std::nth_element(ys.begin(), ys.begin() + ys.size() / 10, ys.end());
    low_y = ys[ys.size() / 10];
    return true;
}

}  // namespace debris_offload
