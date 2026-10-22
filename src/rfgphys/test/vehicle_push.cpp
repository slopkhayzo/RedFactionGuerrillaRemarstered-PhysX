#include "test/vehicle_push.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "physx/debris_offload.h"

namespace vehicle_push {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// object: pos, orient (rvec, uvec, fvec), Havok handle, flags, subtype; world: all_objects.elt, type_objects[OT_VEHICLE]
// and subtype_objects[OT_SUB_MOVER_RFG] (base_array<ushort>: elt +4, num +0xC); hkpRigidBody: motion type.
constexpr uint32_t kObjPos = 0x04, kObjOrient = 0x10, kObjHavokHandle = 0x4C, kObjFlags = 0x54, kObjSubType = 0x7F;
constexpr uint8_t kObjDestroyed = 0x10;
constexpr uint32_t kWorldAllObjects = 0x64, kWorldVehicles = 0x70 + 3 * 0x10 + 4, kWorldVehicleCount = 0x70 + 3 * 0x10 + 0xC;
constexpr uint32_t kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr uint32_t kBodyMotionType = 0xE8;
constexpr uint8_t kMotionFixed = 5;
constexpr int8_t kSubTypeVehicleAuto = 4;
constexpr uint32_t kExplosionInfoSize = 0xF0;
constexpr double kStartAt = 15, kSettle = 20, kAfterDrive = 2;
constexpr float kVehicleFrom = 80.0f;                     // m from the player
constexpr float kBuildingNear = 15.0f, kBuildingFar = 60.0f;  // m from the vehicle
constexpr float kSpeed = 8.0f, kBeyond = 12.0f;           // m/s; m past the building
constexpr float kNearPath = 4.0f, kFarFrom = 8.0f, kFarTo = 20.0f, kMoved = 0.3f;  // m

enum Phase { kWait, kSettling, kDriving, kAfter, kDone };
Phase g_phase = kWait;
int64_t g_since = 0;
double g_phase_at = 0;
uint32_t g_handle = 0xFFFFFFFFu;
float g_from[3] = {}, g_to[3] = {}, g_orient[9] = {}, g_building[3] = {};
struct Tracked {
    uint32_t slot;
    float pos[3];
    bool near_path;
};
std::vector<Tracked> g_tracked;

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

float distance(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float distance_to_path(const float* p) {
    float d[3], v[3];
    for (int k = 0; k < 3; ++k) d[k] = g_to[k] - g_from[k], v[k] = p[k] - g_from[k];
    const float len2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float t = len2 > 0 ? (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / len2 : 0.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float e[3];
    for (int k = 0; k < 3; ++k) e[k] = v[k] - d[k] * t;
    return std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
}

const uint8_t* body_of(const uint8_t* obj) {
    const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
    return handle == 0xFFFFFFFFu ? nullptr : static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies))[handle];
}

// The vehicle nearest the player (a car if there is one), and the standing building nearest it.
bool pick(const float* player) {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* vehicles = *reinterpret_cast<const uint16_t* const*>(w + kWorldVehicles);
    const int nv = *reinterpret_cast<const int*>(w + kWorldVehicleCount);
    const uint8_t* best = nullptr;
    float best_d = 1e30f;
    for (int i = 0; all && vehicles && i < nv; ++i) {
        const uint8_t* obj = all[vehicles[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed) || !body_of(obj)) continue;
        const float d = distance(reinterpret_cast<const float*>(obj + kObjPos), player);
        if (d > kVehicleFrom) continue;
        const float key = d - (int8_t(obj[kObjSubType]) == kSubTypeVehicleAuto ? 1000.0f : 0.0f);  // cars first
        if (key < best_d) best_d = key, best = obj;
    }
    if (!best) {
        LOG("vehicle push: no vehicle within %.0f m of the player (%d in the world)", double(kVehicleFrom), nv);
        return false;
    }
    const float* vp = reinterpret_cast<const float*>(best + kObjPos);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int nm = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    const uint8_t* building = nullptr;
    float building_d = 1e30f;
    for (int i = 0; movers && i < nm; ++i) {
        const uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        const uint8_t* body = body_of(obj);
        if (!body || body[kBodyMotionType] != kMotionFixed) continue;
        const float d = distance(reinterpret_cast<const float*>(obj + kObjPos), vp);
        if (d >= kBuildingNear && d <= kBuildingFar && d < building_d) building_d = d, building = obj;
    }
    if (!building) {
        LOG("vehicle push: no standing building %.0f-%.0f m from the vehicle", double(kBuildingNear), double(kBuildingFar));
        return false;
    }
    g_handle = *reinterpret_cast<const uint32_t*>(best + kObjHavokHandle);
    memcpy(g_from, vp, sizeof(g_from));
    memcpy(g_orient, best + kObjOrient, sizeof(g_orient));
    memcpy(g_building, building + kObjPos, sizeof(g_building));
    // Through the building's site and kBeyond past it, at about its base.
    float dir[3] = {g_building[0] - g_from[0], 0.0f, g_building[2] - g_from[2]};
    const float len = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    dir[0] /= len, dir[2] /= len;
    g_to[0] = g_building[0] + dir[0] * kBeyond;
    g_to[1] = g_building[1] + 0.5f;
    g_to[2] = g_building[2] + dir[2] * kBeyond;
    LOG("vehicle push: vehicle at (%.1f, %.1f, %.1f), %.1f m from the player (subtype %d); building at (%.1f, %.1f, "
        "%.1f), %.1f m from it", double(vp[0]), double(vp[1]), double(vp[2]), double(distance(vp, player)),
        int(int8_t(best[kObjSubType])), double(g_building[0]), double(g_building[1]), double(g_building[2]),
        double(building_d));
    return true;
}

// Three vehicle bombs at the building, as the zone cycle test does it.
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

void on_frame(int game_state) {
    if (game_state != kGameplay || g_phase == kDone) return;
    if (!g_since) g_since = session::qpc();
    const double t = session::ticks_to_ms(session::qpc() - g_since) / 1000.0;
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return;
    switch (g_phase) {
    case kWait:
        if (t < kStartAt) return;
        if (!pick(reinterpret_cast<const float*>(player + kObjPos))) {
            g_phase = kDone;
            return;
        }
        blast();
        g_phase = kSettling;
        g_phase_at = t;
        return;
    case kSettling: {
        if (t < g_phase_at + kSettle) return;
        g_tracked.clear();
        uint32_t near_n = 0, far_n = 0;
        for (uint32_t slot : debris_offload::pieces_near(g_building, kBeyond + kFarTo)) {
            Tracked k{slot, {}, false};
            if (!debris_offload::piece_position(slot, k.pos)) continue;
            const float d = distance_to_path(k.pos);
            if (d <= kNearPath) k.near_path = true, ++near_n;
            else if (d >= kFarFrom && d <= kFarTo) ++far_n;
            else continue;
            g_tracked.push_back(k);
        }
        LOG("vehicle push: %u pieces within %.0f m of the path, %u %.0f-%.0f m from it; driving %.1f m at %.0f m/s",
            near_n, double(kNearPath), far_n, double(kFarFrom), double(kFarTo), double(distance(g_from, g_to)),
            double(kSpeed));
        g_phase = kDriving;
        g_phase_at = t;
        return;
    }
    case kDriving: {
        uint8_t* const* bodies = static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies));
        if (g_handle == 0xFFFFFFFFu || !bodies[g_handle]) {
            LOG("vehicle push: the vehicle's body is gone");
            g_phase = kDone;
            return;
        }
        const float length = distance(g_from, g_to);
        const float s = std::min(float(t - g_phase_at) * kSpeed, length) / length;
        float pos[3];
        for (int k = 0; k < 3; ++k) pos[k] = g_from[k] + (g_to[k] - g_from[k]) * s;
        float zero[3] = {0, 0, 0};
        rfg::fn<void(__cdecl*)(uint32_t, float*, float*)>(r::havok_body_set_pos_orient_by_handle)(g_handle, pos, g_orient);
        rfg::fn<void(__cdecl*)(uint32_t, float*)>(r::havok_body_set_linear_velocity_by_handle)(g_handle, zero);
        if (s >= 1.0f) {
            g_phase = kAfter;
            g_phase_at = t;
        }
        return;
    }
    case kAfter: {
        if (t < g_phase_at + kAfterDrive) return;
        uint32_t near_n = 0, near_moved = 0, far_n = 0, far_moved = 0, gone = 0;
        for (const Tracked& k : g_tracked) {
            float now[3];
            if (!debris_offload::piece_position(k.slot, now)) {
                ++gone;
                continue;
            }
            const bool moved = distance(now, k.pos) > kMoved;
            if (k.near_path) ++near_n, near_moved += moved ? 1 : 0;
            else ++far_n, far_moved += moved ? 1 : 0;
        }
        LOG("vehicle push: %u of %u pieces near the path moved more than %.1f m, %u of %u farther off (%u pieces gone); "
            "%s", near_moved, near_n, double(kMoved), far_moved, far_n, gone,
            near_n && near_moved * 2 > near_n && far_moved * 4 < far_n + 1 ? "passed" : "check");
        g_phase = kDone;
        return;
    }
    case kDone:
        return;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (!cfg.test_vehicle_push) return true;
    session::add_frame_callback(&on_frame);
    LOG("vehicle push test armed: 15 s into gameplay a building near the nearest vehicle is blown up, and 20 s later "
        "the vehicle is moved through the rubble");
    return true;
}

}  // namespace vehicle_push
