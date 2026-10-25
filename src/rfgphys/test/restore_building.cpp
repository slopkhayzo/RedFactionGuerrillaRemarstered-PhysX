#include "test/restore_building.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "physx/debris_offload.h"

namespace restore_building {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// object: pos, Havok handle, flags, handle, parent; object_mover: chunk_obj_uid, creator_handle; general_mover:
// destruction_uid, original_object. world: all_objects.elt, subtype_objects[OT_SUB_MOVER_RFG] and [..._GENERAL]
// (base_array<ushort>: elt, num). hkpRigidBody: motion type.
constexpr uint32_t kObjPos = 0x04, kObjHavokHandle = 0x4C, kObjFlags = 0x54, kObjHandle = 0x6C, kObjParent = 0x70;
constexpr uint32_t kObjType = 0x7E;
constexpr uint32_t kMoverChunkObjUid = 0xAC, kMoverCreator = 0xD8, kGmDestructionUid = 0x110, kGmOriginal = 0x128;
constexpr uint8_t kObjDestroyed = 0x10;
constexpr uint32_t kWorldAllObjects = 0x64, kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr uint32_t kWorldGeneralMovers = 0x3C4, kWorldGeneralMoverCount = 0x3CC;
constexpr uint32_t kBodyMotionType = 0xE8;
constexpr uint8_t kMotionFixed = 5;
constexpr uint32_t kExplosionInfoSize = 0xF0;
constexpr double kStartAt = 15, kSampleAfter = 1, kRestoreAfter = 12, kCheckAfter = 5, kNextAfter = 3;
constexpr float kNear = 10.0f, kFar = 60.0f, kDebrisRadius = 60.0f;

enum Phase { kWait, kSample, kRestore, kCheck, kDone };
Phase g_phase = kWait;
int64_t g_since = 0;
double g_phase_at = 0;
uint32_t g_handle = 0xFFFFFFFFu;
float g_building[3] = {};
uint32_t g_havok_before = 0, g_physx_before = 0;
std::unordered_set<uint32_t> g_movers_before;  // the general movers there were before the first blast
int g_cycles = 1, g_cycle = 0, g_passed = 0;
int32_t g_first_free = -1;  // render instances free in the pool after the first cycle
int g_first_movers = -1;    // general movers after the first cycle

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

float distance(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

const uint8_t* body_of(const uint8_t* obj) {
    const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
    return handle == 0xFFFFFFFFu ? nullptr : static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies))[handle];
}

uint8_t* object_from_handle(uint32_t handle) {
    return rfg::fn<uint8_t*(__thiscall*)(void*, uint32_t)>(r::world__object_from_handle)(world(), handle);
}

bool standing(uint32_t handle) {
    const uint8_t* obj = object_from_handle(handle);
    return obj && !(obj[kObjFlags] & kObjDestroyed) && body_of(obj);
}

int32_t render_instances_free() {
    return rfg::global<int32_t>(r::rl_destroyable_model_instance__ms_object_pool + 0xC);
}
int general_movers() { return *reinterpret_cast<const int*>(world() + kWorldGeneralMoverCount); }

// The biggest standing building (by its bounding box, object::get_bbox at vtable slot 1) whose middle is 10-60 m from
// the player: a demolition target rather than a fence post. Its middle, near the ground, is where the bombs go.
bool pick(const float* player) {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    using GetBbox = void(__thiscall*)(uint8_t*, float*, float*);
    uint8_t* best = nullptr;
    float best_volume = 0, best_d = 0, best_lo[3] = {}, best_hi[3] = {};
    for (int i = 0; all && movers && i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        const uint8_t* body = body_of(obj);
        if (!body || body[kBodyMotionType] != kMotionFixed) continue;
        float lo[3], hi[3];
        (*reinterpret_cast<GetBbox* const*>(obj))[1](obj, lo, hi);
        const float mid[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
        const float d = distance(mid, player);
        const float volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
        if (d >= kNear && d <= kFar && volume > best_volume) {
            best = obj, best_volume = volume, best_d = d;
            memcpy(best_lo, lo, sizeof(lo));
            memcpy(best_hi, hi, sizeof(hi));
        }
    }
    if (!best) {
        LOG("restore building: no standing building %.0f-%.0f m from the player", double(kNear), double(kFar));
        return false;
    }
    g_handle = *reinterpret_cast<const uint32_t*>(best + kObjHandle);
    g_building[0] = 0.5f * (best_lo[0] + best_hi[0]);
    g_building[1] = best_lo[1];
    g_building[2] = 0.5f * (best_lo[2] + best_hi[2]);
    LOG("restore building: building %08X, box %.0f x %.0f x %.0f m around (%.1f, %.1f, %.1f), %.1f m from the player "
        "(chunk object uid %08X)", g_handle, double(best_hi[0] - best_lo[0]), double(best_hi[1] - best_lo[1]),
        double(best_hi[2] - best_lo[2]), double(g_building[0]), double(g_building[1]), double(g_building[2]),
        double(best_d), *reinterpret_cast<const uint32_t*>(best + kMoverChunkObjUid));
    return true;
}

void remember_movers() {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldGeneralMoverCount);
    g_movers_before.clear();
    for (int i = 0; all && movers && i < n; ++i)
        if (const uint8_t* obj = all[movers[i]])
            g_movers_before.insert(*reinterpret_cast<const uint32_t*>(obj + kObjHandle));
}

// Three vehicle bombs at the building, as the vehicle push test does it.
void blast() {
    const int n = rfg::global<int>(r::Num_explosion_infos);
    const auto* infos = static_cast<const uint8_t*>(rfg::addr(r::Explosion_infos));
    const uint8_t* info = n > 0 ? infos : nullptr;
    for (int i = 0; i < n && i < 79; ++i)
        if (_stricmp(reinterpret_cast<const char*>(infos + size_t(i) * kExplosionInfoSize), "vehicle_bomb") == 0)
            info = infos + size_t(i) * kExplosionInfoSize;
    if (!info) return;
    static const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const auto create = rfg::fn<void(__cdecl*)(const void*, void*, void*, float*, const float*, float*, void*, bool)>(
        r::explosion_create);
    for (const float off : {0.0f, 4.0f, -4.0f}) {
        float p[3] = {g_building[0] + off, g_building[1] + 2.5f, g_building[2] - off};
        create(info, nullptr, nullptr, p, identity, nullptr, nullptr, false);
    }
}

// The general_movers the first blast made: what ties them to the building (original_object or parent).
void classify_new_movers() {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldGeneralMoverCount);
    uint32_t fresh = 0, orig_it = 0, orig_other = 0, orig_none = 0, parent_it = 0, parent_other = 0, parent_none = 0;
    uint32_t with_uid = 0;
    std::unordered_map<uint32_t, uint32_t> parents;
    for (int i = 0; all && movers && i < n; ++i) {
        const uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        if (g_movers_before.count(*reinterpret_cast<const uint32_t*>(obj + kObjHandle))) continue;
        ++fresh;
        const uint32_t original = *reinterpret_cast<const uint32_t*>(obj + kGmOriginal);
        const uint32_t parent = *reinterpret_cast<const uint32_t*>(obj + kObjParent);
        ++(original == g_handle ? orig_it : original == 0xFFFFFFFFu ? orig_none : orig_other);
        ++(parent == g_handle ? parent_it : parent == 0xFFFFFFFFu ? parent_none : parent_other);
        with_uid += *reinterpret_cast<const uint32_t*>(obj + kGmDestructionUid) != 0xFFFFFFFFu;
        if (parent != 0xFFFFFFFFu && parent != g_handle) ++parents[parent];
    }
    LOG("restore building: %u general movers the blast made: original object the building %u, another %u, none %u; "
        "parent the building %u, another %u, none %u; with a destruction uid %u", fresh, orig_it, orig_other, orig_none,
        parent_it, parent_other, parent_none, with_uid);
    uint32_t top = 0xFFFFFFFFu, top_n = 0;
    for (const auto& [h, c] : parents)
        if (c > top_n) top = h, top_n = c;
    if (top_n) {
        const uint8_t* obj = object_from_handle(top);
        LOG("restore building:   the most common other parent: %08X (%u movers), object type %d", top, top_n,
            obj ? int(obj[kObjType]) : -1);
    }
}

// The general_movers near the building: how many broke off it (original_object), and a few of their fields.
uint32_t havok_pieces(bool sample) {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldGeneralMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldGeneralMoverCount);
    uint32_t from_it = 0, nearby = 0, shown = 0;
    for (int i = 0; all && movers && i < n; ++i) {
        const uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        from_it += *reinterpret_cast<const uint32_t*>(obj + kGmOriginal) == g_handle;
        if (!sample || distance(reinterpret_cast<const float*>(obj + kObjPos), g_building) > kDebrisRadius) continue;
        ++nearby;
        if (shown++ < 6)
            LOG("restore building:   general mover %08X: original object %08X, destruction uid %08X, creator %08X, "
                "parent %08X, chunk object uid %08X", *reinterpret_cast<const uint32_t*>(obj + kObjHandle),
                *reinterpret_cast<const uint32_t*>(obj + kGmOriginal),
                *reinterpret_cast<const uint32_t*>(obj + kGmDestructionUid),
                *reinterpret_cast<const uint32_t*>(obj + kMoverCreator),
                *reinterpret_cast<const uint32_t*>(obj + kObjParent),
                *reinterpret_cast<const uint32_t*>(obj + kMoverChunkObjUid));
    }
    if (sample)
        LOG("restore building: %u general movers within %.0f m of the building, %u broke off it", nearby,
            double(kDebrisRadius), from_it);
    return from_it;
}

void on_frame(int game_state) {
    if (game_state != kGameplay || g_phase == kDone) return;
    if (!g_since) g_since = session::qpc();
    const double t = session::ticks_to_ms(session::qpc() - g_since) / 1000.0;
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return;
    switch (g_phase) {
    case kWait:
        if (g_cycle == 0) {
            if (t < kStartAt) return;
            if (!pick(reinterpret_cast<const float*>(player + kObjPos))) {
                g_phase = kDone;
                return;
            }
            remember_movers();
        } else {
            if (t < g_phase_at + kNextAfter) return;
            if (!standing(g_handle)) {
                LOG("restore building: cycle %d: the building %08X does not stand; stopping", g_cycle + 1, g_handle);
                g_phase = kDone;
                return;
            }
        }
        blast();
        g_phase = kSample;
        g_phase_at = t;
        return;
    case kSample:
        if (t < g_phase_at + kSampleAfter) return;
        if (g_cycle == 0) {
            havok_pieces(true);
            classify_new_movers();
        }
        g_phase = kRestore;
        return;
    case kRestore: {
        if (t < g_phase_at + kRestoreAfter) return;
        g_havok_before = havok_pieces(false);
        g_physx_before = debris_offload::count_pieces_of(g_handle);
        const bool queued =
            rfg::fn<bool(__thiscall*)(void*, uint32_t)>(r::world__restore_object)(world(), g_handle);
        LOG("restore building: cycle %d of %d: before the restoration %u pieces that broke off it in Havok, %u in "
            "PhysX; world::restore_object %s", g_cycle + 1, g_cycles, g_havok_before, g_physx_before,
            queued ? "queued it" : "refused it");
        g_phase = queued ? kCheck : kDone;
        g_phase_at = t;
        return;
    }
    case kCheck: {
        if (t < g_phase_at + kCheckAfter) return;
        const uint32_t havok = havok_pieces(false), physx = debris_offload::count_pieces_of(g_handle);
        const uint8_t* obj = object_from_handle(g_handle);
        const bool stands = standing(g_handle);
        const bool ok = g_havok_before + g_physx_before > 0 && havok + physx == 0 && stands;
        g_passed += ok;
        const int32_t free = render_instances_free();
        const int movers = general_movers();
        if (g_first_free < 0) g_first_free = free, g_first_movers = movers;
        LOG("restore building: cycle %d of %d: after it %u pieces in Havok, %u in PhysX; the building %s; %d "
            "general movers, %d render instances free; %s", g_cycle + 1, g_cycles, havok, physx,
            stands ? "stands again" : obj ? "exists without a body" : "is gone", movers, free, ok ? "passed" : "check");
        g_phase_at = t;
        if (++g_cycle < g_cycles && stands) {
            g_phase = kWait;
            return;
        }
        if (g_cycles > 1)
            LOG("restore building: %d of %d cycles passed; general movers %d after the first, %d after the last; "
                "render instances free %d after the first, %d after the last; %s", g_passed, g_cycles, g_first_movers,
                movers, g_first_free, free, g_passed == g_cycles ? "passed" : "check");
        g_phase = kDone;
        return;
    }
    case kDone:
        return;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.test_restore_building <= 0) return true;
    g_cycles = cfg.test_restore_building > 50 ? 50 : cfg.test_restore_building;
    session::add_frame_callback(&on_frame);
    LOG("restore building test armed: 15 s into gameplay the biggest standing building near is blown up, and 12 s "
        "later the game restores it; %d time%s", g_cycles, g_cycles == 1 ? "" : "s");
    return true;
}

}  // namespace restore_building
