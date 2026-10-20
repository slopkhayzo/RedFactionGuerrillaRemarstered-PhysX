#include "test/zone_cycle.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"

namespace zone_cycle {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// world: all_objects.elt, subtype_objects[OT_SUB_MOVER_RFG] (base_array<ushort>: elt +4, num +0xC), the territory's
// zones.
constexpr uint32_t kWorldAllObjects = 0x64, kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr uint32_t kWorldZoneCount = 0x271C, kWorldZones = 0x2720;
// world_zone: bmin, bmax, name, state (2: loaded).
constexpr uint32_t kZoneBmin = 0x00, kZoneBmax = 0x0C, kZoneName = 0x18, kZoneState = 0x58;
constexpr int32_t kZoneLoaded = 2;
constexpr uint32_t kObjPos = 0x04, kObjOrient = 0x10, kObjFlags = 0x54;
constexpr uint8_t kObjDestroyed = 0x10;
constexpr uint32_t kExplosionInfoSize = 0xF0;
constexpr size_t kMaxTargets = 6;
constexpr float kTargetNear = 25.0f, kTargetFar = 150.0f;
// World zones are 511 m squares, streamed within 510 m of the player (a box): a zone goes once the player is more than
// 510 m beyond its edge. Hops of 450-800 m, each to a loaded zone (ground to stand on), take the player that far.
constexpr float kHopMin = 450.0f, kHopMax = 800.0f;
constexpr size_t kExplosionsPerFrame = 4;

// From 20 s into gameplay: blow up the buildings near the player (if any break, more pieces), then every 15 s hop away
// until the start's zone has streamed out, wait, then hop home until the start's zone is loaded again and stand at the
// start; blow up what is near once more. At most kMaxHops each way.
enum Phase { kWait, kAway, kGone, kHome, kBack, kDone };
constexpr double kFirstAt = 20, kHopEvery = 15, kGoneFor = 15, kBackFor = 20;
constexpr int kMaxHops = 4;

struct Point {
    float p[3];
};
int64_t g_since = 0;
Phase g_phase = kWait;
double g_phase_at = 0;
int g_phase_hops = 0;
bool g_logged_after = false;
// A teleport to ground not loaded yet leaves the player hanging there (find_place_to_stand finds no terrain); once the
// zones around have streamed in, a second teleport to the same spot puts the player on the ground.
constexpr double kLandAfter = 8;
double g_land_at = -1;
Point g_land_pos{};
std::vector<Point> g_targets, g_queue;
Point g_start{};
int g_hops = 0;
const uint8_t* g_info = nullptr;

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

uint8_t* zone_at(const float* pos) {
    return rfg::fn<uint8_t*(__thiscall*)(void*, const float*)>(r::world__get_world_zone_by_pos)(world(), pos);
}

void zone_name(const uint8_t* zone, char (&out)[65]) {
    out[0] = 0;
    if (!zone) return;
    memcpy(out, zone + kZoneName, 64);
    out[64] = 0;
}

int loaded_zones() {
    uint8_t* w = world();
    const int n = *reinterpret_cast<const int*>(w + kWorldZoneCount);
    uint8_t* const* zones = *reinterpret_cast<uint8_t* const**>(w + kWorldZones);
    int loaded = 0;
    for (int i = 0; zones && i < n; ++i)
        if (zones[i] && *reinterpret_cast<const int32_t*>(zones[i] + kZoneState) == kZoneLoaded) ++loaded;
    return loaded;
}

float dist_xz(const float* a, const float* b) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// The strongest explosion the game defines for scripts: the vehicle bomb (10 m, impulse 75,000).
const uint8_t* explosion_info() {
    const int n = rfg::global<int>(r::Num_explosion_infos);
    const auto* infos = static_cast<const uint8_t*>(rfg::addr(r::Explosion_infos));
    for (int i = 0; i < n && i < 79; ++i)
        if (_stricmp(reinterpret_cast<const char*>(infos + size_t(i) * kExplosionInfoSize), "vehicle_bomb") == 0)
            return infos + size_t(i) * kExplosionInfoSize;
    return n > 0 ? infos : nullptr;
}

// Buildings (rfg_movers) near the player, nearest first.
void pick_near(const float* player) {
    g_targets.clear();
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    std::vector<std::pair<float, Point>> found;
    for (int i = 0; all && movers && i < n; ++i) {
        const uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        const float* p = reinterpret_cast<const float*>(obj + kObjPos);
        const float d = dist_xz(p, player);
        if (d >= kTargetNear && d <= kTargetFar) found.push_back({d, {{p[0], p[1], p[2]}}});
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    char name[65];
    zone_name(zone_at(player), name);
    LOG("zone cycle: the player is in zone %s (%d zones loaded); %zu of %d buildings lie %.0f-%.0f m away", name,
        loaded_zones(), found.size(), n, double(kTargetNear), double(kTargetFar));
    for (size_t i = 0; i < found.size() && g_targets.size() < kMaxTargets; ++i) g_targets.push_back(found[i].second);
}

void blast() {
    for (const Point& t : g_targets)
        for (const float off : {0.0f, 4.0f, -4.0f}) g_queue.push_back({{t.p[0] + off, t.p[1] + 2.5f, t.p[2] - off}});
    LOG("zone cycle: %zu explosions at %zu buildings", g_targets.size() * 3, g_targets.size());
}

void explode_some() {
    if (g_queue.empty() || !g_info) return;
    static const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    // explosion_create(info, source, target, pos, orient, dir, weapon_info, bool), as activity raids demolish
    // buildings (no source, target, direction or weapon).
    const auto create = rfg::fn<void(__cdecl*)(const void*, void*, void*, float*, const float*, float*, void*, bool)>(
        r::explosion_create);
    for (size_t k = 0; k < kExplosionsPerFrame && !g_queue.empty(); ++k) {
        Point p = g_queue.back();
        g_queue.pop_back();
        create(g_info, nullptr, nullptr, p.p, identity, nullptr, nullptr, false);
    }
}

// human_teleport(human*, vector* pos, matrix* orient, float radius, bool force): the player stands at the nearest
// free place within the radius (human_find_place_to_stand, on the terrain's height), as scripted teleports do.
bool teleport(uint8_t* player, const Point& to) {
    Point from{{}};
    memcpy(from.p, player + kObjPos, sizeof(from.p));
    float orient[9];
    memcpy(orient, player + kObjOrient, sizeof(orient));
    Point dest = to;
    rfg::fn<void(__cdecl*)(void*, float*, float*, float, bool)>(r::human_teleport)(player, dest.p, orient, 40.0f, false);
    const float* now = reinterpret_cast<const float*>(player + kObjPos);
    const float moved = dist_xz(now, from.p);
    char name[65];
    zone_name(zone_at(now), name);
    LOG("zone cycle: teleport to (%.0f, %.0f): %s, now at (%.0f, %.0f, %.0f) in zone %s, %.0f m from the start",
        double(to.p[0]), double(to.p[2]), moved > 50.0f ? "done" : "FAILED (no place to stand)", double(now[0]),
        double(now[1]), double(now[2]), name, double(dist_xz(now, g_start.p)));
    return moved > 50.0f;
}

// A hop: to the centre of a loaded zone 450-800 m from the player (only a loaded zone has ground to stand on), away
// from the start as far as possible, or home: straight to the start once its zone is loaded, else the loaded zone
// centre nearest to it.
void hop(uint8_t* player, bool home, double t) {
    const float* pos = reinterpret_cast<const float*>(player + kObjPos);
    // The game streams zones around the camera, which stays behind while a teleported player hangs over ground not
    // loaded yet: for the test the streaming follows the player (world::do_frame, Stream_follows_player).
    rfg::global<uint8_t>(r::Stream_follows_player) = 1;
    const uint8_t* start_zone = zone_at(g_start.p);
    if (home && start_zone && *reinterpret_cast<const int32_t*>(start_zone + kZoneState) == kZoneLoaded) {
        LOG("zone cycle: hop %d (home): %d zones loaded, the start's zone among them", ++g_hops, loaded_zones());
        teleport(player, g_start);
        return;
    }
    uint8_t* w = world();
    const int n = *reinterpret_cast<const int*>(w + kWorldZoneCount);
    uint8_t* const* zones = *reinterpret_cast<uint8_t* const**>(w + kWorldZones);
    std::vector<std::pair<float, Point>> candidates;
    for (int i = 0; zones && i < n; ++i) {
        const uint8_t* z = zones[i];
        if (!z || *reinterpret_cast<const int32_t*>(z + kZoneState) != kZoneLoaded) continue;
        const float* lo = reinterpret_cast<const float*>(z + kZoneBmin);
        const float* hi = reinterpret_cast<const float*>(z + kZoneBmax);
        const Point c{{0.5f * (lo[0] + hi[0]), pos[1] + 30.0f, 0.5f * (lo[2] + hi[2])}};
        const float d = dist_xz(c.p, pos), from_start = dist_xz(c.p, g_start.p);
        if (d >= kHopMin && d <= kHopMax) candidates.push_back({home ? from_start : -from_start, c});
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    LOG("zone cycle: hop %d (%s): %d zones loaded, %zu loaded zone centres %.0f-%.0f m away", ++g_hops,
        home ? "towards the start" : "away", loaded_zones(), candidates.size(), double(kHopMin), double(kHopMax));
    for (size_t i = 0; i < candidates.size() && i < 4; ++i)
        if (teleport(player, candidates[i].second)) {
            g_land_pos = candidates[i].second;
            g_land_at = t + kLandAfter;
            return;
        }
}

// What the zone streaming sees: world::do_frame streams around the player when Stream_follows_player is set, else
// around Rfg_camera (+0x2C); it skips streaming while saving state data (save_load_info byte 1, bit 4).
constexpr uint32_t kWorldDeserializeState = 0x24E0, kWorldSlFlags = 0x265C, kWorldStreamPos = 0x2710;
constexpr uint32_t kCameraPos = 0x2C;

void log_streaming(const char* when, const float* player) {
    uint8_t* w = world();
    const float* sp = reinterpret_cast<const float*>(w + kWorldStreamPos);
    const float* cam = reinterpret_cast<const float*>(static_cast<uint8_t*>(rfg::addr(r::Rfg_camera)) + kCameraPos);
    const uint8_t* start = zone_at(g_start.p);
    LOG("zone cycle: %s: player (%.0f, %.0f, %.0f), camera (%.0f, %.0f, %.0f), stream position (%.0f, %.0f); start zone "
        "state %d; deserialize state %d, save flags %08X, stream follows player %d; %d zones loaded",
        when, double(player[0]), double(player[1]), double(player[2]), double(cam[0]), double(cam[1]), double(cam[2]),
        double(sp[0]), double(sp[2]), start ? *reinterpret_cast<const int32_t*>(start + kZoneState) : -1,
        *reinterpret_cast<const int32_t*>(w + kWorldDeserializeState), *reinterpret_cast<const uint32_t*>(w + kWorldSlFlags),
        rfg::global<uint8_t>(r::Stream_follows_player), loaded_zones());
}

bool start_zone_loaded() {
    const uint8_t* z = zone_at(g_start.p);
    return z && *reinterpret_cast<const int32_t*>(z + kZoneState) == kZoneLoaded;
}

void enter(Phase phase, double t) {
    g_phase = phase;
    g_phase_at = t;
    g_phase_hops = 0;
}

void on_frame(int game_state) {
    if (game_state != kGameplay || g_phase == kDone) return;
    if (!g_since) g_since = session::qpc();
    explode_some();
    const double t = session::ticks_to_ms(session::qpc() - g_since) / 1000.0;
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
    if (!player) return;
    const float* pos = reinterpret_cast<const float*>(player + kObjPos);
    const double in_phase = t - g_phase_at;
    if (g_land_at >= 0 && t >= g_land_at) {
        g_land_at = -1;
        const float y = pos[1];
        Point dest = g_land_pos;
        float orient[9];
        memcpy(orient, player + kObjOrient, sizeof(orient));
        rfg::fn<void(__cdecl*)(void*, float*, float*, float, bool)>(r::human_teleport)(player, dest.p, orient, 40.0f,
                                                                                    true);
        LOG("zone cycle: landing: height %.0f -> %.0f m", double(y), double(pos[1]));
        log_streaming("after landing", pos);
    }
    switch (g_phase) {
    case kWait:
        if (t < kFirstAt) return;
        if (!g_info) g_info = explosion_info();
        memcpy(g_start.p, pos, sizeof(g_start.p));
        pick_near(pos);
        blast();
        enter(kAway, t);
        break;
    case kAway:
        if (g_phase_hops > 0 && !g_logged_after && in_phase >= kHopEvery * g_phase_hops + 5) {
            g_logged_after = true;
            log_streaming("5 s after the hop", pos);
        }
        if (!start_zone_loaded()) {
            LOG("zone cycle: the start's zone has streamed out (%.0f m away, %d hops)", double(dist_xz(pos, g_start.p)),
                g_phase_hops);
            enter(kGone, t);
        } else if (in_phase >= kHopEvery * (g_phase_hops + 1)) {
            if (g_phase_hops++ >= kMaxHops) {
                LOG("zone cycle: the start's zone is still loaded after %d hops; going home", kMaxHops);
                enter(kHome, t);
            } else {
                log_streaming("before the hop", pos);
                hop(player, false, t);
                g_logged_after = false;
            }
        }
        break;
    case kGone:
        if (in_phase >= kGoneFor) enter(kHome, t - kHopEvery);  // the first hop home at once
        break;
    case kHome:
        if (in_phase >= kHopEvery * (g_phase_hops + 1)) {
            if (g_phase_hops++ >= kMaxHops) {
                LOG("zone cycle: could not get home in %d hops", kMaxHops);
                enter(kBack, t);
            } else {
                const bool direct = start_zone_loaded();
                hop(player, true, t);
                if (direct) enter(kBack, t);
            }
        }
        break;
    case kBack:
        if (in_phase >= kBackFor) {
            pick_near(pos);
            blast();
            LOG("zone cycle: done, %d zones loaded", loaded_zones());
            rfg::global<uint8_t>(r::Stream_follows_player) = 0;
            enter(kDone, t);
        }
        break;
    case kDone:
        break;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (!cfg.test_zone_cycle) return true;
    session::add_frame_callback(&on_frame);
    LOG("zone cycle test armed: from 20 s into gameplay the player hops away until the start's zone streams out, then "
        "back; buildings near the start are blown up before and after");
    return true;
}

}  // namespace zone_cycle
