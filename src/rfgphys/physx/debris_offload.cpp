#include "physx/debris_offload.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
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
SRWLOCK g_piece_set_lock = SRWLOCK_INIT;
std::unordered_set<const void*> g_piece_set;
uint32_t g_slots = 0, g_player_slot = 0;
constexpr uint32_t kNoStandIn = 0xFFFFFFFFu;
struct Piece {
    uint8_t* inst = nullptr;  // the render instance taken over; null: free slot
    uint8_t settle = 0;       // 1 while the last applied pose was an interpolated (moving) one
    uint8_t fresh = 0;        // 1 from adoption until the render task first sees the piece
    uint8_t heavy = 0;        // solid to the player (Experiment.PhysXSolidDebris)
    uint32_t filter = 0;      // its Havok body's collision filter info, for its stand-in
    uint32_t stand_in = kNoStandIn;  // Havok handle of its stand-in near the player
    uint32_t wanted = 0;      // the stand-in pass that last wanted one
    float mass = 0;
    uint8_t physical_material = 0, effect_material = 0;  // its Havok body's properties 3 and 4, for its stand-in
    float box_c[3] = {}, box_size[3] = {};  // bounds of its parts in its own frame: centre, full extents
    float radius = 0;         // of those bounds around its origin
    uint32_t srid = 0;        // the chunk reference its general_mover held (0: none), given back with the piece
    uint32_t chunk_uid = kNoChunk;  // its building's chunk
};
std::vector<Piece> g_pieces;
std::vector<uint32_t> g_free;
std::unordered_map<uint64_t, uint32_t> g_shape_ids;
uint32_t g_next_shape = 1;
bool g_player_ready = false;
FrameStats g_stats{};
uint64_t g_total_adopted = 0;
// Masses of the pieces taken over, by powers of two (bucket b: [2^(b-1), 2^b) kg; 0: under 1 kg), and how the game
// decides between pushing a body out of the player's way and standing on it (character_controller_should_push_away:
// lighter than Character_collision_push_away_mass is pushed).
uint32_t g_mass_hist[20] = {};
bool g_tweaks_logged = false;

int mass_bucket(float mass) {
    int b = 0;
    for (float m = 1.0f; mass >= m && b < 19; m *= 2.0f) ++b;
    return b;
}

void log_masses(const char* when) {
    char text[512];
    int n = 0;
    for (int b = 0; b < 20 && n < int(sizeof(text)) - 32; ++b)
        if (g_mass_hist[b])
            n += snprintf(text + n, sizeof(text) - n, " <%u kg: %u", 1u << b, g_mass_hist[b]);
    LOG("debris offload: masses of the pieces taken over (%s):%s", when, n ? text : " none");
}

// ---- shapes ----

struct Parts : hs::Sink {
    std::vector<std::vector<ipc::Vec3>> parts;
    bool has_triangles = false;
    void triangle(const float*, const float*, const float*) override { has_triangles = true; }
    void convex(const float (*v)[3], int n, float, uint32_t) override {
        if (n < 4) return;
        std::vector<ipc::Vec3> p(size_t(std::min(n, 255)));
        for (size_t i = 0; i < p.size(); ++i) p[i] = {v[i][0], v[i][1], v[i][2]};
        parts.push_back(std::move(p));
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

bool send_part(uint32_t shape, uint32_t part, const std::vector<ipc::Vec3>& v, uint32_t flags = 0) {
    std::vector<uint8_t> buf((sizeof(ipc::ShapePart) + v.size() * sizeof(ipc::Vec3) + 7) & ~size_t(7));
    auto* c = reinterpret_cast<ipc::ShapePart*>(buf.data());
    c->h = {uint16_t(ipc::Cmd::ShapePart), 0, uint32_t(buf.size())};
    c->shape = shape;
    c->part = part;
    c->vertex_count = uint32_t(v.size());
    c->flags = flags;
    memcpy(c + 1, v.data(), v.size() * sizeof(ipc::Vec3));
    return pxbridge::send_record(buf.data(), uint32_t(buf.size()));
}

// The same parts (to the millimetre) with the same flags share one host shape: shards of one building repeat a lot.
uint32_t intern_shape(const Parts& p, uint32_t flags) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(flags);
    for (const auto& part : p.parts) {
        mix(part.size());
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
        if (!send_part(id, uint32_t(i), p.parts[i], flags)) return 0;  // ring full: try again later
    g_shape_ids.emplace(h, id);
    return id;
}

// ---- render instances ----

void move_instance(uint8_t* inst, const float pos[3], const float q[4]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    float* rot = reinterpret_cast<float*>(inst + kInstRot);  // rvec, uvec, fvec: the rotated x, y and z axes
    rot[0] = 1 - 2 * (y * y + z * z), rot[1] = 2 * (x * y + z * w), rot[2] = 2 * (x * z - y * w);
    rot[3] = 2 * (x * y - z * w), rot[4] = 1 - 2 * (x * x + z * z), rot[5] = 2 * (y * z + x * w);
    rot[6] = 2 * (x * z + y * w), rot[7] = 2 * (y * z - x * w), rot[8] = 1 - 2 * (x * x + y * y);
    memcpy(inst + kInstPos, pos, 12);
    // As object_mover::update_render: mark the transform dirty, update the scene partition, tell the renderer.
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
std::vector<uint32_t> g_refs_to_release;
uint64_t g_refs_taken = 0, g_refs_missing = 0, g_refs_released = 0;
bool g_stream_owner_checked = false;

void give_up(uint32_t slot, bool tell_host) {
    Piece& p = g_pieces[slot];
    if (!p.inst) return;
    if (p.stand_in != kNoStandIn) g_stand_ins_to_free.push_back(p.stand_in);
    if (g_drop_meshes) {
        AcquireSRWLockExclusive(&g_piece_set_lock);
        g_piece_set.erase(p.inst);
        ReleaseSRWLockExclusive(&g_piece_set_lock);
    }
    release_instance(p.inst);
    if (p.srid) g_refs_to_release.push_back(p.srid);
    const auto chunk = g_chunk_pieces.find(p.chunk_uid);
    if (chunk != g_chunk_pieces.end() && --chunk->second == 0) g_chunk_pieces.erase(chunk);
    p = Piece{};
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
        ++g_mass_hist[mass_bucket(mass)];
        const float* lv = reinterpret_cast<const float*>(body + kBodyLinVel);
        const float* av = reinterpret_cast<const float*>(body + kBodyAngVel);
        const uint32_t slot = g_free.back();
        if (!pxbridge::create_body(slot, shape, {t.pos[0], t.pos[1], t.pos[2]}, {q[0], q[1], q[2], q[3]},
                                   {lv[0], lv[1], lv[2]}, {av[0], av[1], av[2]}, mass, ipc::kBodyMass))
            break;  // the ring is full
        g_free.pop_back();
        // The render instance is ours now: with the pointer cleared the general_mover does not release it. Its
        // reference on its chunk becomes the piece's too (with srid cleared world::free_object keeps it): without it
        // the chunk unloaded once the building's zone streamed out, the pieces drawing from freed memory (the first
        // campaign session: stretched triangles at a distance, then a crash in rl_scene_renderer::render).
        const uint32_t srid = *reinterpret_cast<const uint32_t*>(obj + kObjSrid);
        const bool holds = srid && (obj[kObjStreamFlags] & kObjStreamHeld);
        if (holds) *reinterpret_cast<uint32_t*>(obj + kObjSrid) = 0;
        const uint32_t chunk_uid = *reinterpret_cast<const uint32_t*>(obj + kMoverChunkUid);
        *reinterpret_cast<uint8_t**>(obj + kGmRenderInstance) = nullptr;
        destroy(w, obj, false);
        Piece& p = g_pieces[slot];
        p = Piece{};
        p.inst = inst;
        p.fresh = 1;
        p.srid = holds ? srid : 0;
        p.chunk_uid = chunk_uid;
        if (chunk_uid != kNoChunk) ++g_chunk_pieces[chunk_uid];
        ++(holds ? g_refs_taken : g_refs_missing);
        if (g_drop_meshes) {
            AcquireSRWLockExclusive(&g_piece_set_lock);
            g_piece_set.insert(inst);
            ReleaseSRWLockExclusive(&g_piece_set_lock);
        }
        p.heavy = heavy ? 1 : 0;
        p.filter = *reinterpret_cast<const uint32_t*>(body + kBodyFilterInfo);
        p.mass = mass;
        p.physical_material = uint8_t(body_property(body, 3));
        p.effect_material = uint8_t(body_property(body, 4));
        bounds(parts, p);
        ++g_stats.adopted;
        ++g_total_adopted;
        --budget;
    }
}

// ---- per frame ----

// Render task (below): the sim/render handshake, the render thread waiting.
void move_pieces() {
    bool fresh = false;
    const ipc::SnapshotHeader* s = pxbridge::snapshot(&fresh);
    if (!s) return;
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint32_t slots = std::min<uint32_t>(s->slots, g_slots);
    const float period_ms = 1000.0f / pxbridge::step_hz();
    float alpha = float(session::ticks_to_ms(session::qpc() - s->publish_qpc)) / period_ms;
    alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    uint32_t drops = 0;
    const auto delete_mesh = rfg::fn<void(__cdecl*)(void*)>(r::rl_delete_mesh_recompile_data);
    for (uint32_t i = 0; i < slots; ++i) {
        Piece& p = g_pieces[i];
        if (!p.inst || i == g_player_slot) continue;
        if (g_drop_meshes && drops < kDropPerFrame) {
            void*& mesh = *reinterpret_cast<void**>(p.inst + kInstRecompileData);
            if (mesh) {
                delete_mesh(mesh);
                mesh = nullptr;
                ++drops;
                ++g_meshes_dropped;
            }
        }
        if (g_batch) {
            uint16_t& flags = *reinterpret_cast<uint16_t*>(p.inst + kInstFlags);
            if (!(flags & kInstBatchable)) {
                flags |= kInstBatchable;
                ++g_batch_restored;
            }
        }
        p.fresh = 0;
        const ipc::BodyPose& b = poses[i];
        if (!(b.flags & ipc::kPoseAlive)) continue;  // not created yet in the host
        const bool awake = (b.flags & ipc::kPoseAwake) != 0;
        if (!awake && !p.settle) continue;  // at rest and already placed there
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
        if (pos[1] < kFloorY) {
            give_up(i, true);
            continue;
        }
        move_instance(p.inst, pos, q);
        ++g_stats.moved;
    }
}

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
    ipc::KinematicTarget c{};
    c.h = {uint16_t(ipc::Cmd::KinematicTarget), 0, sizeof(c)};
    c.slot = g_player_slot;
    c.pos = centre;
    c.rot = {0, 0, 0, 1};
    pxbridge::send_record(&c, sizeof(c));
}

// ---- the player on the pile (Experiment.PhysXSolidDebris) ----

// The game's character controller (hk_custom_character_proxy) collides with Havok only. In the game a body lighter
// than Character_collision_push_away_mass is pushed out of the player's way (character_controller_should_push_away)
// and anything else is solid: the player stands on it. For the pieces in PhysX: each heavy piece near the player has
// a stand-in in Havok, a keyframed box fitted to the piece in its own frame and moved with it, so the player stands
// on heavy rubble and is blocked by it; the player's capsule in the host touches only light pieces (filtered by
// kShapeHeavy / kShapePlayer) and pushes those as before. A stand-in has no game object, so the character
// controller takes its contacts as solid ground; they carry the zone object, like terrain (zone_object below).
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
constexpr int32_t kCreateBoxAtLocal = 2;         // physics_create_info::flags
constexpr float kStandInReach = 2.5f;            // m from the player's middle to a piece's bounds
constexpr float kStandInShrink = 0.1f;           // m off each extent: Havok's convex radius sits around a box
constexpr size_t kMaxStandIns = 192;
std::vector<uint32_t> g_stand_in_slots;          // pieces with a stand-in (may repeat; stand_in is the truth)
uint32_t g_stand_in_pass = 0, g_stand_ins_live = 0;
uint32_t g_stand_ins_created = 0, g_stand_in_failures = 0, g_stand_ins_peak = 0;

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
    ci.collision_type = p.filter;
    memcpy(ci.pos, pos, sizeof(ci.pos));
    memcpy(ci.orient, rot, sizeof(ci.orient));
    for (int k = 0; k < 3; ++k) ci.size[k] = std::max(p.box_size[k] - kStandInShrink, 0.05f);
    ci.max_angular_vel = 200.0f, ci.max_linear_vel = 200.0f;
    ci.mass = p.mass > 0 ? p.mass : 100.0f, ci.friction = 0.6f, ci.restitution = 0.1f;
    ci.flags = kCreateBoxAtLocal;
    memcpy(ci.local_pos, p.box_c, sizeof(ci.local_pos));
    ci.local_orient[0] = ci.local_orient[4] = ci.local_orient[8] = 1.0f;
    ci.quality = kQualityKeyframed;
    ci.solver_deactivation = kSolverDeactivationLow;
    ci.motion_type = kMotionKeyframed;
    ci.shape_type = kHavokShapeBox;
    const uint32_t handle = rfg::fn<uint32_t(__cdecl*)(PhysicsCreateInfo*)>(r::havok_body_create)(&ci);
    if (handle == kNoStandIn) {
        ++g_stand_in_failures;
        return;
    }
    p.stand_in = handle;
    g_stand_in_slots.push_back(slot);
    if (std::find(g_stand_in_zones.begin(), g_stand_in_zones.end(), zone) == g_stand_in_zones.end())
        g_stand_in_zones.push_back(zone);
    ++g_stand_ins_created;
    g_stand_ins_peak = std::max(g_stand_ins_peak, ++g_stand_ins_live);
}

// Sim thread, end of frame (Havok is not stepping): stand-ins for the heavy pieces nearest the player.
void update_stand_ins() {
    for (uint32_t h : g_stand_ins_to_free) free_stand_in(h);
    g_stand_ins_to_free.clear();
    if (!g_solid) return;
    ++g_stand_in_pass;
    const ipc::SnapshotHeader* s = pxbridge::snapshot(nullptr);
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    static std::vector<std::pair<float, uint32_t>> candidates;
    candidates.clear();
    if (s && player) {
        const float* pp = reinterpret_cast<const float*>(player + 0x04);
        const float px = pp[0], py = pp[1] + 0.9f, pz = pp[2];
        const ipc::BodyPose* poses = pxbridge::poses(s);
        const uint32_t slots = std::min<uint32_t>(s->slots, g_slots);
        for (uint32_t i = 0; i < slots; ++i) {
            const Piece& p = g_pieces[i];
            if (!p.inst || !p.heavy || i == g_player_slot) continue;
            const ipc::BodyPose& b = poses[i];
            if (!(b.flags & ipc::kPoseAlive)) continue;
            const float dx = b.pos.x - px, dy = b.pos.y - py, dz = b.pos.z - pz;
            const float reach = kStandInReach + p.radius, d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < reach * reach) candidates.emplace_back(std::sqrt(d2) - p.radius, i);
        }
        if (candidates.size() > kMaxStandIns) {
            std::nth_element(candidates.begin(), candidates.begin() + kMaxStandIns, candidates.end());
            candidates.resize(kMaxStandIns);
        }
        const auto set_pose = rfg::fn<void(__cdecl*)(uint32_t, const float*, const float*)>(
            r::havok_body_set_pos_orient_by_handle);
        for (const auto& c : candidates) {
            const uint32_t i = c.second;
            Piece& p = g_pieces[i];
            p.wanted = g_stand_in_pass;
            const ipc::BodyPose& b = poses[i];
            const float pos[3] = {b.pos.x, b.pos.y, b.pos.z};
            float rot[9];
            rows_from_quat(b.rot, rot);
            if (p.stand_in == kNoStandIn) create_stand_in(i, pos, rot);
            else set_pose(p.stand_in, pos, rot);
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
    if (g_stand_in_slots.empty() && g_stand_ins_to_free.empty()) g_stand_in_zones.clear();
    g_stats.stand_ins = g_stand_ins_live;
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
}

void log_explosion_infos();  // below, with the explosion hook
void send_explosions();

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
        if (!g_pieces[i].inst || i == g_player_slot) continue;
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

void __cdecl render_task(void*, int) {
    if (!g_on) return;
    release_chunk_refs();
    if (!pxbridge::ready() || rfg::gameseq_get_state() != kGameplay) return;
    const int64_t t0 = session::qpc();
    move_pieces();
    recycle_far_pieces();
    g_stats.move_ms += float(session::ticks_to_ms(session::qpc() - t0));
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
    }
    const int64_t t0 = session::qpc();
    send_explosions();
    if (world_mirror::ready()) adopt();  // not before the pieces have a world to land on
    player_proxy();
    update_stand_ins();
    g_stats.adopt_ms += float(session::ticks_to_ms(session::qpc() - t0));
}

// rl_start_recompile_mesh_instance(rl_destroyable_model_instance*, bool): no compiled meshes for the pieces.
void(__cdecl* o_start_recompile)(const void*, bool) = nullptr;
void __cdecl d_start_recompile(const void* inst, bool in_handshake) {
    AcquireSRWLockShared(&g_piece_set_lock);
    const bool piece = g_piece_set.count(inst) != 0;
    ReleaseSRWLockShared(&g_piece_set_lock);
    if (piece) {
        ++g_recompiles_skipped;
        return;
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
uint32_t g_expl_forwarded = 0;

void(__cdecl* o_explosion)(void*, void*, void*, float*, void*, float*, void*, bool) = nullptr;
void __cdecl d_explosion(void* info, void* source, void* target, float* pos, void* orient, float* dir, void* weapon,
                         bool flag) {
    o_explosion(info, source, target, pos, orient, dir, weapon, flag);
    if (!g_on || !pos || !info) return;
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
                p.srid = 0;
                give_up(s, true);
                ++n;
            }
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

void on_exit(bool) {
    if (!g_on) return;
    LOG("debris offload: %llu pieces taken over; %u explosions sent to the host; the game's explosion kicks: %u, "
        "%.2f m/s mean, %.2f max", g_total_adopted, g_expl_forwarded, g_kick_n, g_kick_n ? g_kick_sum / g_kick_n : 0.0,
        g_kick_max);
    if (g_batch)
        LOG("debris offload: batchable bit set again %u times (the game's compiled meshes clear it)", g_batch_restored);
    if (g_drop_meshes)
        LOG("debris offload: %u compiled meshes of pieces given back, %u recompiles skipped", g_meshes_dropped,
            g_recompiles_skipped);
    if (g_recycled_total)
        LOG("debris offload: %llu pieces far from the player given up for render instances", g_recycled_total);
    LOG("debris offload: chunk references taken over from the movers: %llu (%llu pieces came without one), %llu given "
        "back; %u zones streamed out, %llu pieces went with them; %u chunks unloaded under pieces (%llu pieces)",
        g_refs_taken, g_refs_missing, g_refs_released, g_zones_out, g_zone_pieces_released, g_chunks_out_under_pieces,
        g_chunk_pieces_released);
    if (g_solid)
        LOG("debris offload: Havok stand-ins for heavy pieces near the player: %u created, %u at most at once, %u "
            "failed", g_stand_ins_created, g_stand_ins_peak, g_stand_in_failures);
}

// object_level_shutdown() (from gamestate_gp_exit, when gameplay ends): the level's objects are freed, movers among
// them; the render instances taken over go now too, while the renderer still has them. (general_mover_level_shutdown
// looked like the place but nothing calls it.)
void(__cdecl* o_level_shutdown)() = nullptr;
void __cdecl d_level_shutdown() {
    if (g_on) {
        free_all_stand_ins();  // while Havok's world is still there
        AcquireSRWLockExclusive(&g_piece_set_lock);
        g_piece_set.clear();
        ReleaseSRWLockExclusive(&g_piece_set_lock);
        EnterCriticalSection(&g_expl_lock);
        g_expl_pending.clear();
        LeaveCriticalSection(&g_expl_lock);
        g_zone_obj = nullptr;
        uint32_t n = 0;
        for (uint32_t s = 0; s < g_slots; ++s)
            if (g_pieces[s].inst) {
                give_up(s, false);
                ++n;
            }
        // Before world::level_shutdown destreams the zones and releases what the level's objects still hold.
        const size_t refs = g_refs_to_release.size();
        release_chunk_refs();
        g_chunk_pieces.clear();
        if (n) LOG("debris offload: level end, %u render instances released, %zu chunk references given back", n, refs);
        log_masses("level end");
        memset(g_mass_hist, 0, sizeof(g_mass_hist));
        pxbridge::reset();
        g_player_ready = false;
        g_shape_ids.clear();
    }
    o_level_shutdown();
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.exp_physx <= 0 || !cfg.exp_physx_world || !cfg.exp_physx_debris) return true;
    g_slots = pxbridge::max_bodies();
    if (g_slots < 2) return true;
    g_player_slot = g_slots - 1;
    g_pieces.assign(g_slots, Piece{});
    g_free.clear();
    for (uint32_t s = g_player_slot; s-- > 0;) g_free.push_back(s);  // pop_back hands out 0, 1, 2, ...
    InitializeCriticalSection(&g_expl_lock);
    bool ok = hooks::create(r::explosion_create, &d_explosion, &o_explosion, "explosion_create");
    ok &= hooks::create(r::havok_body_apply_linear_impulse_by_handle, &d_impulse, &o_impulse,
                        "havok_body_apply_linear_impulse(handle)");
    ok &= hooks::create(r::object_level_shutdown, &d_level_shutdown, &o_level_shutdown, "object_level_shutdown");
    ok &= hooks::create(r::world__destream_zone, &d_destream_zone, &o_destream_zone, "world::destream_zone");
    ok &= hooks::create(r::chunk_base_unload_internal, &d_chunk_unload, &o_chunk_unload, "chunk_base_unload_internal");
    if (!ok) return false;
    g_on = true;
    g_batch = cfg.exp_physx_debris_batch != 0;
    g_drop_meshes = g_batch && cfg.exp_physx_debris_drop_meshes != 0;
    if (g_drop_meshes && !hooks::create(r::rl_start_recompile_mesh_instance, &d_start_recompile, &o_start_recompile,
                                        "rl_start_recompile_mesh_instance"))
        g_drop_meshes = false;
    g_solid = cfg.exp_physx_solid_debris != 0;
    g_solid_mass_kg = cfg.exp_physx_solid_mass_kg;
    if (g_solid && !patch_lean_check()) {
        LOG("debris offload: unexpected code in MyCharacterListener::processConstraintsCallback; no stand-ins");
        g_solid = false;
    }
    session::add_frame_callback(&on_frame);  // adoption; the moves run in a game render task (registered there)
    session::add_flush_handler(&on_exit);
    LOG("debris offload armed: debris pieces go to the PhysX host (%u slots), the player is a kinematic capsule there%s%s",
        g_slots - 1, g_batch ? "; their render instances are drawn through the batched pass" : "",
        g_solid ? "; heavy pieces are solid to the player (Havok stand-ins), the capsule pushes light ones" : "");
    return true;
}

FrameStats take_frame() {
    FrameStats f = g_stats;
    f.live = uint32_t(g_slots - 1 - g_free.size());
    g_stats = {};
    return f;
}

}  // namespace debris_offload
