#include "core/hang_watch.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/gameloop.h"
#include "core/log.h"
#include "core/session.h"
#include "core/thread_dump.h"
#include "telemetry/telemetry.h"

namespace hang_watch {
namespace {

constexpr int kGameplay = 1;          // GS_GAMEPLAY
constexpr double kBusyShare = 0.5;    // a thread using half a core or more over the probe is busy
constexpr DWORD kProbeMs = 1000;

double g_hang_ms = 0;
bool g_unattended = false;
int g_test_hang_after_s = 0;
int64_t g_gameplay_since = 0;

// Test.HangAfterSeconds: the sim thread spins at the end of a frame, as a stuck game thread would.
void test_hang_frame(int game_state) {
    if (game_state != kGameplay) return;
    if (!g_gameplay_since) g_gameplay_since = session::qpc();
    if (session::ticks_to_ms(session::qpc() - g_gameplay_since) < g_test_hang_after_s * 1000.0) return;
    LOG("hang watch test: the sim thread now spins forever (Test.HangAfterSeconds=%d)", g_test_hang_after_s);
    volatile uint32_t spin = 0;
    for (;;) spin = spin + 1;
}

// CPU time (100 ns units) of every thread of the process.
std::unordered_map<DWORD, uint64_t> thread_times() {
    std::unordered_map<DWORD, uint64_t> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(h, &created, &exited, &kernel, &user))
            out[te.th32ThreadID] = (uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
                                   (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime);
        CloseHandle(h);
    }
    CloseHandle(snap);
    return out;
}

struct Busy {
    DWORD tid;
    double share;  // of one core
};

// Threads of the game that used kBusyShare of a core or more over kProbeMs (this one excluded).
std::vector<Busy> busy_threads() {
    const auto before = thread_times();
    const int64_t t0 = session::qpc();
    Sleep(kProbeMs);
    const auto after = thread_times();
    const double wall_100ns = session::ticks_to_ms(session::qpc() - t0) * 10000.0;
    std::vector<Busy> out;
    for (const auto& [tid, t] : after) {
        const auto it = before.find(tid);
        if (it == before.end() || tid == GetCurrentThreadId()) continue;
        const double share = double(t - it->second) / wall_100ns;
        if (share >= kBusyShare) out.push_back({tid, share});
    }
    std::sort(out.begin(), out.end(), [](const Busy& a, const Busy& b) { return a.share > b.share; });
    return out;
}

void report(double stalled_ms, const std::vector<Busy>& busy) {
    std::string list;
    for (const Busy& b : busy) {
        char item[48];
        snprintf(item, sizeof(item), "%s%lu (%.0f%%)", list.empty() ? "" : ", ", b.tid, b.share * 100.0);
        list += item;
    }
    LOG("hang: no gameplay frame has ended for %.0f s (frame %llu); busy threads: %s", stalled_ms / 1000.0,
        gameloop::frames_ended(), list.empty() ? "none" : list.c_str());
    const DWORD sim = gameloop::sim_thread_id(), render = telemetry::render_thread_id();
    thread_dump::log_thread(sim, "sim");
    thread_dump::log_thread(render, "render");
    for (const Busy& b : busy)
        if (b.tid != sim && b.tid != render) thread_dump::log_thread(b.tid, "busy");
}

DWORD WINAPI watch_main(void*) {
    SetThreadDescription(GetCurrentThread(), L"rfgphys hang watch");
    unsigned long long seen = gameloop::frames_ended();
    int64_t since = session::qpc();
    bool told_idle = false;
    for (;;) {
        Sleep(1000);
        const unsigned long long frames = gameloop::frames_ended();
        // Menus and loading screens may legitimately hold a frame for long; only gameplay frames are watched.
        if (frames != seen || gameloop::last_state() != kGameplay) {
            seen = frames;
            since = session::qpc();
            told_idle = false;
            continue;
        }
        const double stalled = session::ticks_to_ms(session::qpc() - since);
        if (stalled < g_hang_ms) continue;
        const std::vector<Busy> busy = busy_threads();
        if (gameloop::frames_ended() != seen) continue;  // it moved on during the probe
        if (busy.empty() && !g_unattended) {
            if (!told_idle)
                LOG("hang watch: no gameplay frame for %.0f s but no thread of the game is busy (window minimised, "
                    "debugger?): waiting", stalled / 1000.0);
            told_idle = true;
            continue;
        }
        report(stalled + kProbeMs, busy);
        session::exit_now("the game hung: a gameplay frame never ended (hang report in the log)");
    }
}

}  // namespace

void start(const Config& cfg) {
    if (cfg.hang_s <= 0) return;
    g_hang_ms = cfg.hang_s * 1000.0;
    // Nobody is there to close a frozen window in a replay, a scripted run or a timed test.
    g_unattended = cfg.harness_mode == 2 || !cfg.harness_auto_start.empty() || !cfg.harness_input_script.empty() ||
                   cfg.exit_after_s > 0 || cfg.exit_after_gameplay_s > 0 || cfg.exit_after_main_menu_s > 0;
    CloseHandle(CreateThread(nullptr, 256 * 1024, &watch_main, nullptr, 0, nullptr));
    LOG("hang watch: a gameplay frame still running after %d s ends the game with a hang report (%s)", cfg.hang_s,
        g_unattended ? "unattended run: whatever its threads do" : "when a thread of the game is busy");
    if (cfg.test_hang_after_s > 0) {
        g_test_hang_after_s = cfg.test_hang_after_s;
        session::add_frame_callback(&test_hang_frame);
    }
}

}  // namespace hang_watch
