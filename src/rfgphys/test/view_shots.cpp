#include "test/view_shots.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "harness/screenshot.h"

namespace view_shots {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
constexpr double kStartS = 10, kShortS = 40, kTurnEveryS = 4, kShotOffsetS = 0.5;
constexpr int kTurns = 9;
// Long runs: every kBurstEveryS a burst of kBurstShots shots kBurstGapS apart, then the turn.
constexpr double kBurstEveryS = 10, kBurstGapS = 0.5, kBurstTurnAtS = 3;
constexpr int kBurstShots = 4;
double g_length_s = kShortS;
bool g_bursts = false;
int g_burst = 0, g_burst_shots = 0;
bool g_burst_turned = false;
// Mouse movement added to the next input state in one frame: about 36 degrees of the game camera's turn (the fly
// camera test, dev/fly_camera.cpp).
constexpr float kTurnMouse = 300.f;
constexpr uint32_t kInputSize = 0x444, kInMouseMoveX = 0x330;  // cached_input_state
constexpr uint32_t kCamRealPos = 0x2C, kCamRealOrient = 0x50;   // rfg_camera: real_pos, real_orient (rvec, uvec, fvec)

// Frame-time buckets (ms), upper edges: 8.33 is 120 FPS.
constexpr double kEdges[] = {1, 2, 4, 6, 8.33, 12, 16.7, 33.3, 66.7};
uint32_t g_hist[std::size(kEdges) + 1] = {};
uint32_t g_frames = 0;
double g_sum_ms = 0, g_min_ms = 1e9, g_max_ms = 0;

int64_t g_since = 0;
int g_shots = 0, g_turns = 0;
bool g_done = false;

void log_histogram() {
    char text[512];
    int len = 0;
    for (size_t i = 0; i < std::size(g_hist); ++i) {
        const double lo = i ? kEdges[i - 1] : 0;
        if (i < std::size(kEdges))
            len += snprintf(text + len, sizeof(text) - len, " %.4g-%.4g: %u;", lo, kEdges[i], g_hist[i]);
        else
            len += snprintf(text + len, sizeof(text) - len, " over %.4g: %u", lo, g_hist[i]);
    }
    uint32_t fast = 0;
    for (size_t i = 0; i < 5; ++i) fast += g_hist[i];  // under 8.33 ms
    LOG("view shots: %u frames in %.0f s: frame time %.2f ms mean (%.0f FPS), %.2f min, %.2f max; %u frames (%.1f%%) "
        "faster than 120 FPS; ms:%s", g_frames, g_length_s, g_frames ? g_sum_ms / g_frames : 0.0,
        g_sum_ms > 0 ? 1000.0 * g_frames / g_sum_ms : 0.0, g_min_ms, g_max_ms, fast,
        g_frames ? 100.0 * fast / g_frames : 0.0, text);
}

// The game camera turns about 36 degrees with the next input poll.
void turn() {
    uint8_t* next = static_cast<uint8_t*>(rfg::addr(r::Cached_input_states)) +
                    rfg::global<uint8_t>(r::Next_cached_input_state) * kInputSize;
    *reinterpret_cast<float*>(next + kInMouseMoveX) += kTurnMouse;
}

void shot(const wchar_t* name, double t) {
    screenshot::request(session::run_dir() + name);
    const uint8_t* cam = static_cast<const uint8_t*>(rfg::addr(r::Rfg_camera));
    const float* pos = reinterpret_cast<const float*>(cam + kCamRealPos);
    const float* fvec = reinterpret_cast<const float*>(cam + kCamRealOrient + 0x18);
    LOG("view shots: %ls at %.1f s: camera (%.1f, %.1f, %.1f), facing (%.2f, %.2f, %.2f)", name + 1, t, double(pos[0]),
        double(pos[1]), double(pos[2]), double(fvec[0]), double(fvec[1]), double(fvec[2]));
}

void on_frame(int game_state) {
    if (g_done || game_state != kGameplay) return;
    const int64_t now = session::qpc();
    if (!g_since) g_since = now;
    const double t = session::ticks_to_ms(now - g_since) / 1000.0 - kStartS;
    if (t < 0) return;
    const double ms = double(rfg::global<float>(r::Frametime_real)) * 1000.0;
    if (ms > 0) {
        size_t b = 0;
        while (b < std::size(kEdges) && ms >= kEdges[b]) ++b;
        ++g_hist[b];
        ++g_frames;
        g_sum_ms += ms;
        g_min_ms = std::min(g_min_ms, ms);
        g_max_ms = std::max(g_max_ms, ms);
    }
    if (!g_bursts) {
        if (g_turns < kTurns && t >= (g_turns + 1) * kTurnEveryS) {
            turn();
            ++g_turns;
        }
        if (t >= kShotOffsetS + g_shots && t < g_length_s) {
            wchar_t name[32];
            swprintf_s(name, L"\\view_%02d.png", g_shots);
            shot(name, t);
            ++g_shots;
        }
    } else {
        const double in_burst = t - g_burst * kBurstEveryS;
        if (in_burst >= kBurstEveryS) {
            ++g_burst;
            g_burst_shots = 0;
            g_burst_turned = false;
        } else if (g_burst_shots < kBurstShots && in_burst >= kShotOffsetS + g_burst_shots * kBurstGapS &&
                   t < g_length_s) {
            wchar_t name[32];
            swprintf_s(name, L"\\view_%03d_%d.png", g_burst, g_burst_shots);
            shot(name, t);
            ++g_burst_shots;
        } else if (!g_burst_turned && in_burst >= kBurstTurnAtS) {
            turn();
            g_burst_turned = true;
        }
    }
    if (t >= g_length_s) {
        log_histogram();
        g_done = true;
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.test_view_shots <= 0) return true;
    g_bursts = cfg.test_view_shots > 1;
    g_length_s = g_bursts ? double(cfg.test_view_shots) : kShortS;
    if (!screenshot::install()) {
        LOG("view shots test: screenshots unavailable; not armed");
        return false;
    }
    session::add_frame_callback(&on_frame);
    if (g_bursts)
        LOG("view shots test armed: from %.0f s into gameplay for %.0f s, every %.0f s %d screenshots %.1f s apart, then "
            "the camera turned about 36 degrees", kStartS, g_length_s, kBurstEveryS, kBurstShots, kBurstGapS);
    else
        LOG("view shots test armed: from %.0f s into gameplay a screenshot every second for %.0f s, the camera turned "
            "about 36 degrees every %.0f s", kStartS, g_length_s, kTurnEveryS);
    return true;
}

}  // namespace view_shots
