#include "physx/test_scene.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "physx/bridge.h"

namespace pxtest {
namespace {

namespace ipc = rfgphysx;

constexpr int kGameplay = 1;          // GS_GAMEPLAY
constexpr uint32_t kBurst = 1000;     // bodies per burst
constexpr double kBurstEveryMs = 500;
constexpr double kExplosionEveryMs = 3000;
constexpr uint32_t kShapes = 8;       // 3 boxes, 5 convex rocks

int g_target = 0;
int g_crash_after_s = 0;
bool g_world_mirrored = false;  // Experiment.PhysXWorld: the bodies land on the mirrored world, not on a plane
bool g_spawned = false, g_crash_sent = false;
uint32_t g_sent = 0;
double g_start_ms = 0, g_next_burst_ms = 0, g_next_explosion_ms = 0;
ipc::Vec3 g_origin{};
std::mt19937 g_rng{20261019};
std::vector<ipc::BodyPose> g_copy;                 // the newest snapshot's records (the "upload")
struct Pose {
    ipc::Vec3 pos;
    ipc::Quat rot;
};
std::vector<Pose> g_render;                        // interpolated poses for this frame
FrameStats g_stats{};
volatile float g_sink = 0;                         // keeps the copies and interpolation from being optimized away

float uni(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g_rng); }

bool player_pos(ipc::Vec3& out) {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)();
    if (!player) return false;
    memcpy(&out, player + 0x04, sizeof(out));  // object::pos
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

void register_shapes() {
    pxbridge::shape_box(0, {0.25f, 0.25f, 0.25f});
    pxbridge::shape_box(1, {0.5f, 0.15f, 0.4f});
    pxbridge::shape_box(2, {0.9f, 0.12f, 0.12f});
    for (uint32_t s = 3; s < kShapes; ++s) {
        ipc::Vec3 v[20];
        const float sx = uni(0.2f, 0.6f), sy = uni(0.15f, 0.5f), sz = uni(0.2f, 0.6f);
        for (ipc::Vec3& p : v) p = {uni(-sx, sx), uni(-sy, sy), uni(-sz, sz)};
        pxbridge::shape_convex(s, v, 20);
    }
}

void burst() {
    const uint32_t n = std::min<uint32_t>(kBurst, uint32_t(g_target) - g_sent);
    // A 10 x 10 x 10 block of pieces 12-24 m above the ground, flying apart a little: a building section breaking.
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t x = i % 10, z = (i / 10) % 10, y = i / 100;
        const ipc::Vec3 pos{g_origin.x + (float(x) - 4.5f) * 1.3f + uni(-0.2f, 0.2f), g_origin.y + 12.0f + float(y) * 1.3f,
                            g_origin.z + (float(z) - 4.5f) * 1.3f + uni(-0.2f, 0.2f)};
        const float ax = uni(-1, 1), ay = uni(-1, 1), az = uni(-1, 1), angle = uni(0, 6.2831853f);
        const float len = std::sqrt(ax * ax + ay * ay + az * az) + 1e-6f, s = std::sin(angle * 0.5f) / len;
        const ipc::Quat rot{ax * s, ay * s, az * s, std::cos(angle * 0.5f)};
        const ipc::Vec3 vel{uni(-2, 2), uni(-1, 2), uni(-2, 2)}, spin{uni(-2, 2), uni(-2, 2), uni(-2, 2)};
        if (!pxbridge::create_body(g_sent, g_sent % kShapes, pos, rot, vel, spin, 2000.0f)) break;
        ++g_sent;
    }
}

void spawn(double now) {
    if (!player_pos(g_origin)) return;
    g_origin.y -= 1.0f;  // the plane goes just under the player's feet
    if (!g_world_mirrored) {
        pxbridge::reset();
        pxbridge::add_static_plane({0, 1, 0}, -g_origin.y);
    }
    register_shapes();
    g_spawned = true;
    g_sent = 0;
    g_start_ms = now;
    g_next_burst_ms = now;
    g_next_explosion_ms = now + double((uint32_t(g_target) + kBurst - 1) / kBurst) * kBurstEveryMs + 4000.0;
    g_copy.assign(pxbridge::max_bodies(), ipc::BodyPose{});
    g_render.assign(pxbridge::max_bodies(), Pose{});
    LOG("PhysX test: %d bodies in bursts of %u around (%.1f, %.1f, %.1f), ground plane at y %.1f", g_target, kBurst,
        g_origin.x, g_origin.y + 12.0f, g_origin.z, g_origin.y);
}

ipc::Quat nlerp(const ipc::Quat& a, ipc::Quat b, float t) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = {-b.x, -b.y, -b.z, -b.w};
    ipc::Quat q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    const float inv = 1.0f / std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

void consume() {
    bool fresh = false;
    const ipc::SnapshotHeader* s = pxbridge::snapshot(&fresh);
    if (!s) return;
    const uint32_t slots = std::min<uint32_t>(s->slots, uint32_t(g_copy.size()));
    const ipc::BodyPose* src = pxbridge::poses(s);
    float sink = 0;
    if (fresh) {
        const int64_t t0 = session::qpc();
        memcpy(g_copy.data(), src, size_t(slots) * sizeof(ipc::BodyPose));
        g_stats.copy_us += float(session::ticks_to_ms(session::qpc() - t0) * 1000.0);
        sink += g_copy[slots ? slots - 1 : 0].pos.y;
    }
    // Interpolate for this frame: the snapshot shows the end of its step; render one step behind, between the
    // step's start and end, by how far the game is into the next step.
    const int64_t t0 = session::qpc();
    const float period_ms = 1000.0f / pxbridge::step_hz();
    float alpha = float(session::ticks_to_ms(t0 - s->publish_qpc)) / period_ms;
    alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    uint32_t live = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        const ipc::BodyPose& p = g_copy[i];
        if (!(p.flags & ipc::kPoseAlive)) continue;
        ++live;
        Pose& r = g_render[i];
        r.pos = {p.prev_pos.x + (p.pos.x - p.prev_pos.x) * alpha, p.prev_pos.y + (p.pos.y - p.prev_pos.y) * alpha,
                 p.prev_pos.z + (p.pos.z - p.prev_pos.z) * alpha};
        r.rot = (p.flags & ipc::kPoseAwake) ? nlerp(p.prev_rot, p.rot, alpha) : p.rot;
        sink += r.pos.y;
    }
    g_stats.interp_us += float(session::ticks_to_ms(session::qpc() - t0) * 1000.0);
    g_stats.live = live;
    g_sink = sink;
}

void on_frame(int game_state) {
    const double now = session::since_start_ms();
    if (game_state != kGameplay) {
        // Paused or in a menu: the scene stays. Once the level is gone (no player), it is emptied for the next one.
        if (g_spawned && pxbridge::ready() && !rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
            for (uint32_t s = 0; s < g_sent; ++s) pxbridge::destroy_body(s);
            g_spawned = false;
            LOG("PhysX test: the level is gone, scene reset");
        }
        return;
    }
    if (!pxbridge::ready()) return;
    if (!g_spawned) {
        spawn(now);
        if (!g_spawned) return;
    }
    if (g_sent < uint32_t(g_target) && now >= g_next_burst_ms) {
        burst();
        g_next_burst_ms += kBurstEveryMs;
    }
    if (now >= g_next_explosion_ms) {
        // At the pile's base, off-centre so the pile is thrown sideways rather than straight up.
        pxbridge::explosion({g_origin.x + uni(-3, 3), g_origin.y + 1.0f, g_origin.z + uni(-3, 3)}, 10.0f, 5000.0f, 0.5f);
        g_next_explosion_ms += kExplosionEveryMs;
    }
    if (g_crash_after_s > 0 && !g_crash_sent && now - g_start_ms >= g_crash_after_s * 1000.0) {
        g_crash_sent = true;
        LOG("PhysX test: asking the host to crash (Experiment.PhysXTestCrashAfterS=%d)", g_crash_after_s);
        pxbridge::test_crash();
    }
    consume();
    g_stats.spawned = g_sent;
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.exp_physx <= 0 || cfg.exp_physx_test <= 0) return true;
    g_target = cfg.exp_physx_test > int(pxbridge::max_bodies()) ? int(pxbridge::max_bodies()) : cfg.exp_physx_test;
    g_crash_after_s = cfg.exp_physx_test_crash_after_s;
    g_world_mirrored = cfg.exp_physx_world != 0;
    session::add_frame_callback(&on_frame);
    LOG("PhysX test scene armed: %d bodies once gameplay starts", g_target);
    return true;
}

FrameStats take_frame() {
    FrameStats f = g_stats;
    g_stats.copy_us = 0;
    g_stats.interp_us = 0;
    return f;
}

}  // namespace pxtest
