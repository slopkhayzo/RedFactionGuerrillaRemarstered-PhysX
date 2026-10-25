#include "test/visit.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"

namespace visit {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// world: all_objects.elt, subtype_objects[OT_SUB_MOVER_RFG] (elt +4, num +0xC), the territory's zones.
constexpr uint32_t kWorldAllObjects = 0x64, kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr uint32_t kWorldZoneCount = 0x271C, kWorldZones = 0x2720;
// world_zone: bmin, bmax, state (2: loaded).
constexpr uint32_t kZoneBmin = 0x00, kZoneBmax = 0x0C, kZoneState = 0x58;
constexpr int32_t kZoneLoaded = 2;
constexpr uint32_t kObjPos = 0x04, kObjOrient = 0x10, kObjFlags = 0x54;
constexpr uint8_t kObjDestroyed = 0x10;
constexpr double kStartAt = 3, kHopEvery = 8, kListAfter = 2, kGiveUpAfter = 90;
constexpr float kHopAbove = 30.0f;
constexpr float kStandRadius = 40.0f, kListRadius = 40.0f;

float g_to[2], g_face[2];
// Further stops near the first, each at its time (s of gameplay): a strict teleport there.
struct Stop {
    float to[2], face[2];
    double at;
};
std::vector<Stop> g_more;
size_t g_next_stop = 0;
constexpr double kStopTries = 5;
enum Phase { kWait, kTravel, kArrived, kBetween, kMoving, kDone };
Phase g_phase = kWait;
int64_t g_since = 0;
double g_next_at = kStartAt;
int g_hops = 0;
bool g_hanging = false;  // over the destination, landed (strictly) the frame its ground is there

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

float dist_xz(const float* a, const float* b) {
    const float dx = a[0] - b[0], dz = a[1] - b[1];
    return std::sqrt(dx * dx + dz * dz);
}

const uint8_t* loaded_zone_at(const float* x_z) {
    const float pos[3] = {x_z[0], 0.0f, x_z[1]};
    const uint8_t* z = rfg::fn<uint8_t*(__thiscall*)(void*, const float*)>(r::world__get_world_zone_by_pos)(world(), pos);
    return z && *reinterpret_cast<const int32_t*>(z + kZoneState) == kZoneLoaded ? z : nullptr;
}

// human_teleport(human*, vector* pos, matrix* orient, float radius, bool strict) puts the player on the nearest place
// to stand within the radius of pos (human_find_place_to_stand, on ground that has streamed in); with none there it
// puts the player at pos itself, hanging in the air until the ground streams in and the fall kills, unless strict:
// then the player stays put. The ground's height is not known, so places are searched from low to high, strictly.
bool stand_at(uint8_t* player, float x, float z, const float* orient_in) {
    const float* now = reinterpret_cast<const float*>(player + kObjPos);
    const float from[3] = {now[0], now[1], now[2]};
    float orient[9];
    memcpy(orient, orient_in ? orient_in : reinterpret_cast<const float*>(player + kObjOrient), sizeof(orient));
    for (float y = -40.0f; y <= 400.0f; y += 60.0f) {
        float dest[3] = {x, y, z};
        rfg::fn<void(__cdecl*)(void*, float*, float*, float, bool)>(r::human_teleport)(player, dest, orient,
                                                                                    kStandRadius, true);
        if (now[0] != from[0] || now[1] != from[1] || now[2] != from[2]) return true;  // a failed strict one moves nothing
    }
    return false;
}

// Over (x, z), hanging in the air until something moves the player: the ground of a loaded zone streams in only once
// the player is near, and the streaming follows the player (a teleport onto unloaded ground and the fall killed).
void hang_over(uint8_t* player, float x, float z, const float* orient_in) {
    const float* now = reinterpret_cast<const float*>(player + kObjPos);
    float dest[3] = {x, now[1] + kHopAbove, z};
    float orient[9];
    memcpy(orient, orient_in ? orient_in : reinterpret_cast<const float*>(player + kObjOrient), sizeof(orient));
    rfg::fn<void(__cdecl*)(void*, float*, float*, float, bool)>(r::human_teleport)(player, dest, orient, kStandRadius,
                                                                                false);
}

// A hop: to the loaded zone whose centre is nearest the destination, if that brings the player nearer.
bool hop(uint8_t* player) {
    const float* pos = reinterpret_cast<const float*>(player + kObjPos);
    const float here[2] = {pos[0], pos[2]};
    uint8_t* w = world();
    const int n = *reinterpret_cast<const int*>(w + kWorldZoneCount);
    uint8_t* const* zones = *reinterpret_cast<uint8_t* const**>(w + kWorldZones);
    float best[2] = {here[0], here[1]}, best_d = dist_xz(here, g_to) - 100.0f;
    for (int i = 0; zones && i < n; ++i) {
        const uint8_t* z = zones[i];
        if (!z || *reinterpret_cast<const int32_t*>(z + kZoneState) != kZoneLoaded) continue;
        const float* lo = reinterpret_cast<const float*>(z + kZoneBmin);
        const float* hi = reinterpret_cast<const float*>(z + kZoneBmax);
        const float c[2] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[2] + hi[2])};
        const float d = dist_xz(c, g_to);
        if (d < best_d) best_d = d, best[0] = c[0], best[1] = c[1];
    }
    if (best[0] == here[0] && best[1] == here[1]) return false;
    hang_over(player, best[0], best[1], nullptr);
    LOG("visit: hop %d to (%.0f, %.0f), %.0f m from the destination: now at (%.0f, %.0f, %.0f)", ++g_hops,
        double(best[0]), double(best[1]), double(best_d), double(pos[0]), double(pos[1]), double(pos[2]));
    return true;
}

// Facing the target: fvec along the ground, uvec up, rvec = uvec x fvec.
void facing(float (&orient)[9]) {
    float fx = g_face[0] - g_to[0], fz = g_face[1] - g_to[1];
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len > 0.01f) fx /= len, fz /= len;
    else fx = 0, fz = 1;
    const float m[9] = {fz, 0, -fx, 0, 1, 0, fx, 0, fz};
    memcpy(orient, m, sizeof(m));
}

void log_arrival(const uint8_t* player) {
    const float* pos = reinterpret_cast<const float*>(player + kObjPos);
    LOG("visit: at the destination (%.0f, %.0f): the player at (%.1f, %.1f, %.1f), facing (%.0f, %.0f)",
        double(g_to[0]), double(g_to[1]), double(pos[0]), double(pos[1]), double(pos[2]), double(g_face[0]),
        double(g_face[1]));
}

// The buildings (rfg_movers) within kListRadius of the destination, with their bounding boxes.
void list_buildings() {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    using GetBbox = void(__thiscall*)(uint8_t*, float*, float*);  // vtable slot 1: object::get_bbox(min, max)
    struct Found {
        float d;
        uint8_t* obj;
    };
    std::vector<Found> found;
    for (int i = 0; all && movers && i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        const float* p = reinterpret_cast<const float*>(obj + kObjPos);
        const float xz[2] = {p[0], p[2]};
        const float d = dist_xz(xz, g_to);
        if (d <= kListRadius) found.push_back({d, obj});
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.d < b.d; });
    LOG("visit: %zu buildings within %.0f m of the destination", found.size(), double(kListRadius));
    for (const Found& f : found) {
        float lo[3], hi[3];
        (*reinterpret_cast<GetBbox* const*>(f.obj))[1](f.obj, lo, hi);
        const float* p = reinterpret_cast<const float*>(f.obj + kObjPos);
        LOG("visit:   building at (%.1f, %.1f, %.1f), %.0f m away: box %.0f x %.0f x %.0f m (height %.0f to %.0f), handle "
            "%08X",
            double(p[0]), double(p[1]), double(p[2]), double(f.d), double(hi[0] - lo[0]), double(hi[1] - lo[1]),
            double(hi[2] - lo[2]), double(lo[1]), double(hi[1]), *reinterpret_cast<const uint32_t*>(f.obj + 0x6C));
    }
}

void on_frame(int game_state) {
    if (game_state != kGameplay || g_phase == kDone) return;
    if (!g_since) g_since = session::qpc();
    const double t = session::ticks_to_ms(session::qpc() - g_since) / 1000.0;
    if (t < g_next_at) return;
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return;
    switch (g_phase) {
    case kWait:
        // The game streams zones around the camera, which stays behind a teleported player: for the test the
        // streaming follows the player (world::do_frame, Stream_follows_player).
        rfg::global<uint8_t>(r::Stream_follows_player) = 1;
        g_phase = kTravel;
        [[fallthrough]];
    case kTravel: {
        if (t >= kGiveUpAfter) {
            LOG("visit: could not get to (%.0f, %.0f) in %.0f s (%d hops)", double(g_to[0]), double(g_to[1]), t,
                g_hops);
            g_phase = kDone;
            break;
        }
        float orient[9];
        facing(orient);
        if (g_hanging) {  // every frame until the ground is there
            if (!stand_at(player, g_to[0], g_to[1], orient)) break;
            log_arrival(player);
            g_phase = kArrived;
            g_next_at = t + kListAfter;
        } else if (loaded_zone_at(g_to)) {
            hang_over(player, g_to[0], g_to[1], orient);
            g_hanging = true;
            LOG("visit: over the destination, landing once its ground streams in");
        } else if (hop(player)) {
            g_next_at = t + kHopEvery;
        } else {
            LOG("visit: no loaded zone brings the player nearer to (%.0f, %.0f)", double(g_to[0]), double(g_to[1]));
            g_phase = kDone;
        }
        break;
    }
    case kArrived:
        list_buildings();
        g_phase = kBetween;
        break;
    case kBetween:
        if (g_next_stop >= g_more.size()) {
            g_phase = kDone;
            break;
        }
        if (t < g_more[g_next_stop].at) break;
        memcpy(g_to, g_more[g_next_stop].to, sizeof(g_to));
        memcpy(g_face, g_more[g_next_stop].face, sizeof(g_face));
        g_phase = kMoving;
        [[fallthrough]];
    case kMoving: {
        float orient[9];
        facing(orient);
        if (stand_at(player, g_to[0], g_to[1], orient)) {
            log_arrival(player);
        } else if (t < g_more[g_next_stop].at + kStopTries) {
            break;
        } else {
            LOG("visit: could not stand at (%.0f, %.0f)", double(g_to[0]), double(g_to[1]));
        }
        ++g_next_stop;
        g_phase = kBetween;
        break;
    }
    case kDone:
        break;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.test_visit.empty()) return true;
    // x,z,tx,tz, then |x,z,tx,tz@seconds for each further stop.
    const std::wstring& v = cfg.test_visit;
    size_t bar = v.find(L'|');
    if (swscanf_s(v.substr(0, bar).c_str(), L"%f,%f,%f,%f", &g_to[0], &g_to[1], &g_face[0], &g_face[1]) != 4) {
        LOG("visit: Test.Visit wants x,z,tx,tz[|x,z,tx,tz@s...]; got '%ls'", v.c_str());
        return false;
    }
    while (bar != std::wstring::npos) {
        const size_t next = v.find(L'|', bar + 1);
        Stop stop{};
        if (swscanf_s(v.substr(bar + 1, next == std::wstring::npos ? next : next - bar - 1).c_str(), L"%f,%f,%f,%f@%lf",
                      &stop.to[0], &stop.to[1], &stop.face[0], &stop.face[1], &stop.at) != 5) {
            LOG("visit: a further stop wants x,z,tx,tz@seconds; got '%ls'", v.c_str());
            return false;
        }
        g_more.push_back(stop);
        bar = next;
    }
    session::add_frame_callback(&on_frame);
    LOG("visit: from %.0f s into gameplay the player goes to (%.0f, %.0f), facing (%.0f, %.0f); %zu further stops",
        kStartAt, double(g_to[0]), double(g_to[1]), double(g_face[0]), double(g_face[1]), g_more.size());
    for (const Stop& s : g_more)
        LOG("visit:   at %.0f s: (%.0f, %.0f), facing (%.0f, %.0f)", s.at, double(s.to[0]), double(s.to[1]),
            double(s.face[0]), double(s.face[1]));
    return true;
}

}  // namespace visit
