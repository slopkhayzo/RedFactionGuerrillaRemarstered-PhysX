#include "test/viewpoints.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"
#include "hooks/hooks.h"
#include "physx/debris_offload.h"
#include "telemetry/telemetry.h"

namespace viewpoints {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// rfg_camera (as dev/fly_camera.cpp): real_fov; render_pos and render_orient, the view drawn; whether the culling
// frustum of the game's own visibility tests is up to date.
constexpr uint32_t kCamRealFov = 0xBC, kCamRenderPos = 0x10C, kCamRenderOrient = 0x118, kCamCullingValid = 0x808;
// Each view: held kHoldS; frames from kSettleS on are measured (a cut makes the first frames unlike the rest);
// the screenshot at kShotS.
constexpr double kHoldS = 4.0, kSettleS = 1.0, kShotS = 2.5;

struct Vec {
    float x, y, z;
};
Vec operator+(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec operator-(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec operator*(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec normalized(Vec v) {
    const float len = std::sqrt(dot(v, v));
    return len > 1e-6f ? v * (1.f / len) : Vec{0, 0, 1};
}

struct View {
    std::string name;
    Vec pos, rvec, uvec, fvec;
    Vec target;
};

bool g_on = false;
int g_mode = 1;  // Test.Viewpoints: 1 as it is, 2-4 with the debris (all, piles, pieces of their own) out of sight
bool g_active = false, g_finished = false;
bool g_held = false;  // hold_view: another test's view
View g_held_view;
std::vector<View> g_views;
size_t g_view = 0;
int64_t g_view_since = 0, g_last_frame = 0;
bool g_shot_taken = false;
double g_window_from_ms = 0;
std::vector<double> g_frame_ms;
struct Result {
    double mean_ms, p50, p95, p99;
};
std::vector<Result> g_results;

void(__cdecl* o_render_setup)(void*);

uint8_t* camera() { return static_cast<uint8_t*>(rfg::addr(r::Rfg_camera)); }
template <typename T>
T& field(uint8_t* p, uint32_t offset) {
    return *reinterpret_cast<T*>(p + offset);
}

// The game's matrix rows are the view's right, up and forward vectors; `hand` is the sign of (up x forward).right in
// its own view, so a view made here has the same handedness.
View look(const std::string& name, Vec pos, Vec target, float hand) {
    View v;
    v.name = name;
    v.pos = pos;
    v.target = target;
    v.fvec = normalized(target - pos);
    v.rvec = normalized(cross(Vec{0, 1, 0}, v.fvec) * hand);
    v.uvec = normalized(cross(v.fvec, v.rvec) * hand);
    return v;
}

// `hand` for look(): the game camera's own.
float camera_hand() {
    const Vec* orient = &field<Vec>(camera(), kCamRenderOrient);
    return dot(cross(orient[1], orient[2]), orient[0]) >= 0 ? 1.f : -1.f;
}

// game_render_setup_frame hands the camera's view to the render camera, in the sim/render handshake: while a view is
// held, the view drawn is the test's (as the fly camera does it).
void __cdecl d_render_setup(void* renderer) {
    const View* held = g_held ? &g_held_view : g_active && g_view < g_views.size() ? &g_views[g_view] : nullptr;
    if (held) {
        const View& v = *held;
        uint8_t* cam = camera();
        field<Vec>(cam, kCamRenderPos) = v.pos;
        Vec* orient = &field<Vec>(cam, kCamRenderOrient);
        orient[0] = v.rvec;
        orient[1] = v.uvec;
        orient[2] = v.fvec;
        field<uint8_t>(cam, kCamCullingValid) = 0;
    }
    o_render_setup(renderer);
}

double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(double(v.size()) * p))];
}

void end_view() {
    const View& v = g_views[g_view];
    Result res{};
    if (!g_frame_ms.empty()) {
        double sum = 0;
        for (double ms : g_frame_ms) sum += ms;
        res = {sum / double(g_frame_ms.size()), pct(g_frame_ms, 0.5), pct(g_frame_ms, 0.95), pct(g_frame_ms, 0.99)};
    }
    g_results.push_back(res);
    LOG("viewpoints: view %zu (%s): camera (%.0f, %.0f, %.0f) looking at (%.0f, %.0f, %.0f); %zu frames from %.0f to "
        "%.0f ms: frame %.2f ms mean (%.0f FPS), p50 %.2f, p95 %.2f, p99 %.2f", g_view, v.name.c_str(), v.pos.x, v.pos.y,
        v.pos.z, v.target.x, v.target.y, v.target.z, g_frame_ms.size(), g_window_from_ms, session::since_start_ms(),
        res.mean_ms, res.mean_ms > 0 ? 1000.0 / res.mean_ms : 0.0, res.p50, res.p95, res.p99);
    g_frame_ms.clear();
}

void finish() {
    g_active = false;
    g_finished = true;
    telemetry::sample_draw_stacks(false);
    rfg::fn<void(__cdecl*)(bool)>(r::hud_hide)(false);
    auto mean_of = [](const char* prefix) {
        double sum = 0;
        int n = 0;
        for (size_t i = 0; i < g_views.size() && i < g_results.size(); ++i)  // views cut short have no result
            if (g_views[i].name.rfind(prefix, 0) == 0 && g_results[i].mean_ms > 0) {
                sum += g_results[i].mean_ms;
                ++n;
            }
        return n ? sum / n : 0.0;
    };
    const double high = mean_of("high"), mid = mean_of("mid"), last = g_results.empty() ? 0 : g_results[0].mean_ms;
    LOG("viewpoints: done: the replay's last view %.2f ms (%.0f FPS), from high above %.2f ms (%.0f FPS), from mid "
        "height %.2f ms (%.0f FPS), means over the views", last, last > 0 ? 1000 / last : 0.0, high,
        high > 0 ? 1000 / high : 0.0, mid, mid > 0 ? 1000 / mid : 0.0);
}

void on_frame(int game_state) {
    if (!g_active) return;
    const int64_t now = session::qpc();
    const double frame_ms = session::ticks_to_ms(now - g_last_frame);
    g_last_frame = now;
    if (game_state != kGameplay) {  // the level went away under the test
        LOG("viewpoints: gameplay ended (state %d) at view %zu", game_state, g_view);
        finish();
        return;
    }
    const double in_view = session::ticks_to_ms(now - g_view_since) / 1000.0;
    if (in_view >= kSettleS) {
        if (g_frame_ms.empty()) g_window_from_ms = session::since_start_ms();
        g_frame_ms.push_back(frame_ms);
    }
    if (!g_shot_taken && in_view >= kShotS) {
        g_shot_taken = true;
        screenshot::request(session::run_dir() + L"\\viewpoint_" + std::to_wstring(g_view) + L".png");
    }
    if (in_view < kHoldS) return;
    end_view();
    if (++g_view >= g_views.size()) {
        finish();
        return;
    }
    g_view_since = now;
    g_shot_taken = false;
}

}  // namespace

bool install(const Config& cfg) {
    const bool stress = cfg.test_stress_pieces > 0 || cfg.test_chunk_fall;  // tests that hold the view themselves
    if (!cfg.test_viewpoints && !stress) return true;
    if (cfg.harness_mode != 2 && !stress) {
        LOG("viewpoints: only in replays (Harness.Mode=replay) and with Test.StressPieces");
        return true;
    }
    bool ok = hooks::create(r::game_render_setup_frame, &d_render_setup, &o_render_setup, "game_render_setup_frame");
    ok &= screenshot::install();
    if (!ok) return false;
    if (!cfg.test_viewpoints) return true;  // the held views of Test.StressPieces only
    g_on = true;
    g_mode = cfg.test_viewpoints;
    session::add_frame_callback(&on_frame);
    LOG("viewpoints test armed: after the %s the view holds at viewpoints around the debris, %.0f s each",
        stress ? "stress test" : "recording's last frame", kHoldS);
    return true;
}

bool wanted() { return g_on && !g_finished && !g_active; }

void hold_view(const float pos[3], const float target[3]) {
    g_held_view = look("held", Vec{pos[0], pos[1], pos[2]}, Vec{target[0], target[1], target[2]}, camera_hand());
    g_held = true;
    rfg::fn<void(__cdecl*)(bool)>(r::hud_hide)(true);
}

void release_view() {
    if (!g_held) return;
    g_held = false;
    rfg::fn<void(__cdecl*)(bool)>(r::hud_hide)(false);
}

void begin() {
    uint8_t* cam = camera();
    const Vec pos = field<Vec>(cam, kCamRenderPos);
    const Vec* orient = &field<Vec>(cam, kCamRenderOrient);
    const float hand = camera_hand();
    View last;
    last.name = "the replay's last view";
    last.pos = pos;
    last.rvec = orient[0], last.uvec = orient[1], last.fvec = orient[2];
    last.target = pos + orient[2] * 10.f;
    g_views = {last};
    float centre[3], radius = 0, low_y = 0;
    uint32_t pieces = 0;
    if (debris_offload::debris_extent(centre, radius, low_y, pieces)) {
        LOG("viewpoints: %u pieces in PhysX around (%.0f, %.0f, %.0f), 80%% of them within %.0f m of it, the lowest tenth "
            "below %.1f m", pieces, centre[0], centre[1], centre[2], radius, low_y);
        const Vec target{centre[0], low_y + 2.f, centre[2]};
        const char* sides[4] = {"south", "west", "north", "east"};
        for (int pass = 0; pass < 2; ++pass)
            for (int k = 0; k < 4; ++k) {
                const float yaw = float(k) * 1.5707963f;
                const Vec dir{std::sin(yaw), 0.f, std::cos(yaw)};  // from the camera to the debris
                const float dist = pass == 0 ? radius + 30.f : 0.55f * radius + 15.f;
                const float height = pass == 0 ? 0.8f * radius + 40.f : 15.f;
                const Vec at = pass == 0 ? target : Vec{target.x, low_y + 3.f, target.z};
                const Vec from = Vec{at.x, low_y, at.z} - dir * dist + Vec{0, height, 0};
                g_views.push_back(look(std::string(pass == 0 ? "high, from the " : "mid height, from the ") + sides[k],
                                       from, at, hand));
            }
    } else {
        LOG("viewpoints: no pieces in PhysX: only the replay's last view");
    }
    if (g_mode >= 2 && g_mode <= 4) debris_offload::hide_debris_for_test(g_mode - 1);
    rfg::fn<void(__cdecl*)(bool)>(r::hud_hide)(true);
    g_view = 0;
    g_view_since = g_last_frame = session::qpc();
    g_shot_taken = false;
    g_results.clear();
    g_active = true;
    telemetry::sample_draw_stacks(true);
    LOG("viewpoints: %zu views, %.0f s each", g_views.size(), kHoldS);
}

bool finished() { return g_finished; }

}  // namespace viewpoints
