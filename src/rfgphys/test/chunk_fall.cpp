#include "test/chunk_fall.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"
#include "physx/debris_offload.h"
#include "physx/world_mirror.h"
#include "test/viewpoints.h"

namespace chunk_fall {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// object: pos, Havok handle, flags; world: all_objects.elt, subtype_objects[OT_SUB_MOVER_RFG] (elt, num);
// hkpRigidBody: motion type (5: fixed, a standing building).
constexpr uint32_t kObjPos = 0x04, kObjHavokHandle = 0x4C, kObjFlags = 0x54;
constexpr uint8_t kObjDestroyed = 0x10;
constexpr uint32_t kWorldAllObjects = 0x64, kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr uint32_t kBodyMotionType = 0xE8;
constexpr uint8_t kMotionFixed = 5;
constexpr uint32_t kExplosionInfoSize = 0xF0;
constexpr double kStartAt = 10, kWorldWait = 120;  // s of gameplay; the most it waits for the host's static world
constexpr float kReach = 90.0f, kLongReach = 300.0f;  // m from the player: the tallest, the longest
constexpr float kViewFrom = 70.0f, kViewUp = 22.0f;  // m from the building's middle, above its mid height
constexpr float kViewFromLong = 33.0f, kViewUpLong = 8.0f;  // Test.ChunkFall=3
constexpr float kBombStep = 5.0f, kBombUp = 2.0f;  // m apart over a floor, above its base
constexpr int kMaxBombs = 40, kBombsPerFrame = 4;
constexpr double kShotEvery = 0.5, kFilmS = 12.0;  // s
constexpr uint32_t kObjHandle = 0x6C;
constexpr float kFloorOut = 4.0f;        // m: the floor taken out without bombs
constexpr float kStumpGone = 0.55f;      // of the longest building's length: past its middle, so the top tips
constexpr uint32_t kCutPerFrame = 400;   // subpieces
constexpr int kCutFrames = 240;          // at most
constexpr float kNeighbourReach = 45.0f; // m

enum Phase { kWait, kBlast, kFilm, kDone };
Phase g_phase = kWait;
int g_mode = 1;  // Test.ChunkFall
int64_t g_since = 0;
double g_film_at = 0, g_next_shot = 0;
int g_shot = 0;
const uint8_t* g_info = nullptr;
std::vector<std::vector<float>> g_bombs;
size_t g_bomb_at = 0;
float g_lo[3] = {}, g_hi[3] = {};
uint32_t g_building = 0xFFFFFFFFu;
float g_building_t = 0;
struct Cut {
    float lo[3], hi[3];
};
std::vector<Cut> g_cuts;  // what goes without bombs, in this order
int g_cut_frames = 0;

uint8_t* world() { return static_cast<uint8_t*>(rfg::addr(r::World)); }

const uint8_t* vehicle_bomb() {
    const int n = rfg::global<int>(r::Num_explosion_infos);
    const auto* infos = static_cast<const uint8_t*>(rfg::addr(r::Explosion_infos));
    for (int i = 0; i < n && i < 79; ++i)
        if (_stricmp(reinterpret_cast<const char*>(infos + size_t(i) * kExplosionInfoSize), "vehicle_bomb") == 0)
            return infos + size_t(i) * kExplosionInfoSize;
    return nullptr;
}

// What stands of the buildings next to it, to see them come loose. To the end of the run, past the film.
struct Neighbour {
    uint32_t handle;
    float t, last;
};
std::vector<Neighbour> g_neighbours;
double g_neighbours_at = 0;
void watch_neighbours(double t) {
    if (t < g_neighbours_at) return;
    g_neighbours_at = t + 1.0;
    for (Neighbour& n : g_neighbours) {
        const float now = debris_offload::standing_t(n.handle);
        if (std::fabs(now - n.last) <= 0.02f * n.t) continue;
        n.last = now;
        LOG("chunk fall: %.0f s after the cut: building %08X, next to it, stands with %.0f of %.0f t", t - g_film_at, n.handle, double(now), double(n.t));
    }
}

// A standing building within reach of the player (its box), the tallest or the longest (g_mode), and what comes out of it.
bool pick(const float* player) {
    uint8_t* w = world();
    uint8_t* const* all = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    uint8_t* const* bodies = static_cast<uint8_t* const*>(rfg::addr(r::Hk_rigid_bodies));
    using GetBbox = void(__thiscall*)(uint8_t*, float*, float*);
    std::vector<uint8_t*> standing;
    float best = 0, best_height = 0;
    for (int i = 0; all && movers && i < n; ++i) {
        uint8_t* obj = all[movers[i]];
        if (!obj || (obj[kObjFlags] & kObjDestroyed)) continue;
        const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHavokHandle);
        const uint8_t* body = handle == 0xFFFFFFFFu ? nullptr : bodies[handle];
        if (!body || body[kBodyMotionType] != kMotionFixed) continue;  // not a standing building
        float lo[3], hi[3];
        (*reinterpret_cast<GetBbox* const*>(obj))[1](obj, lo, hi);
        const float height = hi[1] - lo[1];
        const float d = std::hypot(0.5f * (lo[0] + hi[0]) - player[0], 0.5f * (lo[2] + hi[2]) - player[2]);
        if (hi[0] - lo[0] > 80.0f || hi[2] - lo[2] > 80.0f) continue;  // the ground's slab
        standing.push_back(obj);
        if (d > (g_mode == 3 ? kLongReach : kReach)) continue;
        const float score = g_mode == 3 ? (height >= 10.0f ? std::max(hi[0] - lo[0], hi[2] - lo[2]) : 0.0f) : height;
        if (score > best) {
            best = score;
            best_height = height;
            memcpy(g_lo, lo, sizeof(lo));
            memcpy(g_hi, hi, sizeof(hi));
            g_building = *reinterpret_cast<const uint32_t*>(obj + kObjHandle);
        }
    }
    debris_offload::log_materials_of(standing.data(), standing.size());
    if (best <= 0) {
        LOG("chunk fall: no standing building within %.0f m of the player", double(kReach));
        return false;
    }
    g_building_t = debris_offload::standing_t(g_building);
    g_neighbours.clear();
    for (uint8_t* obj : standing) {
        const uint32_t handle = *reinterpret_cast<const uint32_t*>(obj + kObjHandle);
        const float* at = reinterpret_cast<const float*>(obj + kObjPos);
        if (handle == g_building || std::hypot(at[0] - 0.5f * (g_lo[0] + g_hi[0]), at[2] - 0.5f * (g_lo[2] + g_hi[2])) > kNeighbourReach) continue;
        const float t = debris_offload::standing_t(handle);
        g_neighbours.push_back({handle, t, t});
        LOG("chunk fall: next to it, building %08X at (%.0f, %.0f), %.0f t", handle, double(at[0]), double(at[2]), double(t));
    }
    g_bombs.clear();
    g_cuts.clear();
    const float height = g_hi[1] - g_lo[1];
    const float cut = g_mode == 1 ? g_lo[1] + kBombUp : g_lo[1] + height * (g_mode == 2 ? 0.5f : 0.35f);
    if (g_mode == 1) {
        const int nx = std::max(1, int((g_hi[0] - g_lo[0] - 2.0f) / kBombStep));
        const int nz = std::max(1, int((g_hi[2] - g_lo[2] - 2.0f) / kBombStep));
        for (int ix = 0; ix < nx; ++ix)
            for (int iz = 0; iz < nz; ++iz)
                g_bombs.push_back({g_lo[0] + 1.0f + (g_hi[0] - g_lo[0] - 2.0f) * (float(ix) + 0.5f) / float(nx), cut,
                                   g_lo[2] + 1.0f + (g_hi[2] - g_lo[2] - 2.0f) * (float(iz) + 0.5f) / float(nz)});
    } else {
        if (g_mode == 3) {  // the stump's far part first: then the floor over what is left, and the top is on one end
            const int along = g_hi[0] - g_lo[0] > g_hi[2] - g_lo[2] ? 0 : 2;
            Cut stump{{g_lo[0] - 1.0f, g_lo[1] - 1.0f, g_lo[2] - 1.0f}, {g_hi[0] + 1.0f, cut + kFloorOut, g_hi[2] + 1.0f}};
            stump.lo[along] = g_hi[along] - kStumpGone * (g_hi[along] - g_lo[along]);
            g_cuts.push_back(stump);
        }
        g_cuts.push_back({{g_lo[0] - 1.0f, cut, g_lo[2] - 1.0f}, {g_hi[0] + 1.0f, cut + kFloorOut, g_hi[2] + 1.0f}});
    }
    if (g_bombs.size() > size_t(kMaxBombs)) {  // evenly thinned
        std::vector<std::vector<float>> kept;
        for (size_t i = 0; i < size_t(kMaxBombs); ++i) kept.push_back(g_bombs[i * g_bombs.size() / size_t(kMaxBombs)]);
        g_bombs.swap(kept);
    }
    // The view: kViewFrom m from its middle toward the player, a little above its mid height.
    const float mid[3] = {0.5f * (g_lo[0] + g_hi[0]), 0.5f * (g_lo[1] + g_hi[1]), 0.5f * (g_lo[2] + g_hi[2])};
    float dx = player[0] - mid[0], dz = player[2] - mid[2];
    const float len = std::hypot(dx, dz);
    if (len < 1.0f) dx = 1.0f, dz = 0.0f;
    else dx /= len, dz /= len;
    float view_from = kViewFrom, view_up = kViewUp;
    if (g_mode == 3) {  // across its length, from the street, to see it tip
        if (g_hi[0] - g_lo[0] > g_hi[2] - g_lo[2]) dx = 0.0f, dz = dz < 0.0f ? -1.0f : 1.0f;
        else dx = dx < 0.0f ? -1.0f : 1.0f, dz = 0.0f;
        view_from = kViewFromLong;
        view_up = kViewUpLong;
    }
    const float from[3] = {mid[0] + dx * view_from, mid[1] + view_up, mid[2] + dz * view_from};
    viewpoints::hold_view(from, mid);
    LOG("chunk fall: building %08X from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f), %.1f m tall, %.0f t; %zu vehicle bombs, the "
        "cut %.1f m up; the view from (%.1f, %.1f, %.1f)", g_building, double(g_lo[0]), double(g_lo[1]), double(g_lo[2]),
        double(g_hi[0]), double(g_hi[1]), double(g_hi[2]), double(best_height), double(g_building_t), g_bombs.size(),
        double(cut - g_lo[1]), double(from[0]), double(from[1]), double(from[2]));
    return true;
}

uint32_t g_slices = 0;  // the game's slices of chunks since the bombs (d_collide_post)

void log_chunks(double t) {
    const debris_offload::ChunkStats c = debris_offload::chunk_stats();
    const debris_offload::Census p = debris_offload::census();
    LOG("chunk fall: %.1f s: loose chunks in the host %u (%u parts, %.0f t; the fastest %.1f m/s; Havok bodies at most "
        "%.2f m from where they were sent), %u within reach left in Havok; %u driven so far (%u too large); impacts "
        "passed to the game %u (%u too weak, the strongest %.0f kJ), subpieces they broke off %u (%.1f t), parts taken "
        "off the bodies %u, pieces of them gone over at once %u; the game's slices %u; pieces in the host %u; cracks %u, snaps %u; of buildings crushed %.0f t; the heaviest %.0f, %.0f, %.0f, %.0f, %.0f, %.0f t; still standing %.0f of %.0f t", t,
        c.driven, c.parts, double(c.mass_kg) / 1000.0, double(c.fastest), double(c.follow_max), c.in_havok, c.made,
        c.too_large, c.impacts, c.impacts_weak, double(c.energy_max_kj), c.crushed, double(c.crushed_t), c.parts_removed,
        c.pieces_at_once, g_slices, p.live, c.cracks, c.snaps, double(c.stump_t), double(c.largest_t[0]), double(c.largest_t[1]), double(c.largest_t[2]),
        double(c.largest_t[3]), double(c.largest_t[4]), double(c.largest_t[5]), double(debris_offload::standing_t(g_building)), double(g_building_t));
}

// The step's destruction events that touch a loose chunk, as the game applies them (rfg_dyn_do_collide_post, inside
// Havok's step): rfg_collision_event records, 0x110 bytes (research/docs/08 §2). Havok's contact listener leaves its
// contact manager in the record's tail (+0xF8); the multi path, which the PhysX chunks' impacts take, leaves 0. For
// comparing the game's breaking of chunks in Havok with ours. A building is an rfg_mover too, fixed (move_type 0);
// a loose chunk moves. A slice (rfg_dyn_slice_shape) sets the frame of the last one.
constexpr uint32_t kEvSize = 0x110, kEvKeyA = 0x4C, kEvHandleA = 0x50, kEvKeyB = 0xA4, kEvHandleB = 0xA8,
                   kEvType = 0xB0, kEvDamage = 0xB4, kEvFlags = 0xC8, kEvMgr = 0xF8;
constexpr uint32_t kObjType = 0x7E;     // object type (2: mover), then its subtype (1: rfg_mover)
constexpr uint32_t kMoveType = 0xF8;    // rfg_mover::move_type: 0 RFGM_FIXED (a building), 1 RFGM_NORMAL, ...
void(__cdecl* o_collide_post)() = nullptr;
uint32_t g_post_logged = 0;

enum Mover { kNone, kBuilding, kLoose };
Mover mover(uint32_t handle) {
    if (handle == 0xFFFFFFFFu || !handle) return kNone;
    const uint8_t* obj = rfg::fn<uint8_t*(__fastcall*)(void*, void*, uint32_t)>(r::world__object_from_handle)(
        world(), nullptr, handle);
    if (!obj || int8_t(obj[kObjType]) != 2 || int8_t(obj[kObjType + 1]) != 1 || (obj[kObjFlags] & kObjDestroyed))
        return kNone;
    return *reinterpret_cast<const int*>(obj + kMoveType) == 0 ? kBuilding : kLoose;
}

void __cdecl d_collide_post() {
    if ((g_phase == kBlast || g_phase == kFilm) && g_post_logged < 120) {
        const int n = std::min(rfg::global<int>(r::Num_rfg_dyn_collisions), 1024);
        const uint8_t* events = static_cast<const uint8_t*>(rfg::addr(r::RFG_dyn_collisions));
        uint32_t on_chunks = 0, from_havok = 0, simple = 0, complex = 0, other = 0, flag8 = 0, damage_max = 0;
        uint32_t with_building = 0, with_chunk = 0;
        uint64_t damage_sum = 0;
        std::unordered_set<uint32_t> keys;
        for (int i = 0; i < n; ++i) {
            const uint8_t* e = events + size_t(i) * kEvSize;
            const uint32_t ha = *reinterpret_cast<const uint32_t*>(e + kEvHandleA);
            const uint32_t hb = *reinterpret_cast<const uint32_t*>(e + kEvHandleB);
            const Mover ma = mover(ha), mb = mover(hb);
            const bool a = ma == kLoose, b = mb == kLoose;
            if (!a && !b) continue;
            ++on_chunks;
            with_building += (ma == kBuilding || mb == kBuilding) ? 1 : 0;
            with_chunk += (a && b) ? 1 : 0;
            const int type = *reinterpret_cast<const int*>(e + kEvType);
            (type == 3 ? simple : type == 4 ? complex : other)++;
            const uint32_t damage = *reinterpret_cast<const uint32_t*>(e + kEvDamage);
            damage_sum += damage;
            damage_max = std::max(damage_max, damage);
            flag8 += (e[kEvFlags] & 8) ? 1 : 0;
            from_havok += *reinterpret_cast<const uint32_t*>(e + kEvMgr) ? 1 : 0;
            keys.insert(*reinterpret_cast<const uint32_t*>(e + (a ? kEvKeyA : kEvKeyB)));
        }
        if (on_chunks) {
            ++g_post_logged;
            LOG("chunk fall: destruction events on loose chunks this step: %u of %d (%u from Havok's contacts, %u not); "
                "object simple %u, complex %u (with a building %u, two chunks %u), other %u; flag 8 on %u; damage %llu "
                "in all, %u at most; %zu subpieces",
                on_chunks, n, from_havok, on_chunks - from_havok, simple, complex, with_building, with_chunk, other,
                flag8, static_cast<unsigned long long>(damage_sum), damage_max, keys.size());
        }
    }
    const int sliced = rfg::global<int>(r::RFG_uber_heavy_destruction_slice_framecount);
    o_collide_post();
    if (rfg::global<int>(r::RFG_uber_heavy_destruction_slice_framecount) != sliced &&
        (g_phase == kBlast || g_phase == kFilm)) {
        ++g_slices;
        LOG("chunk fall: the game sliced a chunk (slice %u)", g_slices);
    }
}

void on_frame(int game_state) {
    if (game_state != kGameplay) {
        if (g_phase != kWait && g_phase != kDone) viewpoints::release_view();
        if (g_phase != kWait) g_phase = kDone;
        g_neighbours.clear();
        return;
    }
    if (!g_since) g_since = session::qpc();
    const double t = session::ticks_to_ms(session::qpc() - g_since) / 1000.0;
    if (g_phase == kBlast || g_phase == kFilm || g_phase == kDone) watch_neighbours(t);
    switch (g_phase) {
    case kWait: {
        if (t < kStartAt) return;
        const bool world = world_mirror::flushed_scans() == UINT32_MAX || world_mirror::ready();
        if (!world && t < kStartAt + kWorldWait) return;
        uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(r::player_get_local_player)();
        if (!player) return;
        g_info = vehicle_bomb();
        if (!g_info || !pick(reinterpret_cast<const float*>(player + kObjPos))) {
            if (!g_info) LOG("chunk fall: no vehicle_bomb explosion");
            g_phase = kDone;
            return;
        }
        g_bomb_at = 0;
        g_phase = kBlast;
        g_film_at = t;
        g_next_shot = t;
        g_shot = 0;
        return;
    }
    case kBlast:
    case kFilm: {
        if (g_phase == kBlast) {
            static const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            const auto create = rfg::fn<void(__cdecl*)(const void*, void*, void*, float*, const float*, float*, void*,
                                                       bool)>(r::explosion_create);
            for (int k = 0; k < kBombsPerFrame && g_bomb_at < g_bombs.size(); ++k)
                create(g_info, nullptr, nullptr, g_bombs[g_bomb_at++].data(), identity, nullptr, nullptr, false);
            uint32_t cut = 0;  // one at a time: the next waits for this one to be gone
            for (const Cut& c : g_cuts)
                if ((cut = debris_offload::crush_in_box(g_building, c.lo, c.hi, kCutPerFrame))) break;
            if (cut && ++g_cut_frames >= kCutFrames) LOG("chunk fall: %u subpieces would not go", cut);
            if (g_bomb_at >= g_bombs.size() && (!cut || g_cut_frames >= kCutFrames)) g_phase = kFilm;
            debris_offload::trace_chunks_from_now_on();
        }
        if (t >= g_next_shot) {
            g_next_shot += kShotEvery;
            wchar_t name[32];
            swprintf_s(name, L"\\chunk_fall_%02d.png", g_shot++);
            screenshot::request(session::run_dir() + name);
            log_chunks(t - g_film_at);
        }
        if (t - g_film_at >= kFilmS) {
            viewpoints::release_view();
            LOG("chunk fall: done");
            g_phase = kDone;
        }
        return;
    }
    case kDone:
        return;
    }
}

}  // namespace

namespace {
void(__cdecl* o_stress_do_frame)() = nullptr;
void __cdecl d_stress_do_frame() {}  // Test.ChunkFallNoStress: the game's stress evaluation does not run
}  // namespace

bool install(const Config& cfg) {
    if (!cfg.test_chunk_fall) return true;
    g_mode = cfg.test_chunk_fall;
    session::add_frame_callback(&on_frame);
    if (cfg.test_chunk_fall_no_stress) {
        if (hooks::create(r::stress_evaluation_do_frame, &d_stress_do_frame, &o_stress_do_frame, "stress_evaluation_do_frame"))
            LOG("chunk fall: the game's stress evaluation is off");
        else
            LOG("chunk fall: WARNING stress_evaluation_do_frame not hooked: the stress evaluation stays on");
    }
    if (cfg.telemetry)  // the telemetry times the same function
        LOG("chunk fall: with Telemetry.Enabled the destruction events are not logged");
    else if (!hooks::create(r::rfg_dyn_do_collide_post, &d_collide_post, &o_collide_post, "rfg_dyn_do_collide_post"))
        LOG("chunk fall: WARNING rfg_dyn_do_collide_post not hooked: the destruction events are not logged");
    LOG("chunk fall test armed (%d): once the host has the static world, a building within %.0f m is cut by vehicle bombs, "
        "filmed from %.0f m every %.1f s for %.0f s", g_mode, double(kReach), double(kViewFrom), kShotEvery, kFilmS);
    return true;
}

}  // namespace chunk_fall
