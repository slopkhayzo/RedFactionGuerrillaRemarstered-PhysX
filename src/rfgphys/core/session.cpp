#include "core/session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <vector>

#include "core/log.h"
#include "game/rfg.h"

namespace session {
namespace {

Config g_cfg;
std::wstring g_run_dir, g_plugin_dir;
int64_t g_t0 = 0;
double g_ms_per_tick = 0;
std::vector<void (*)(bool)> g_flush_handlers;
std::vector<void (*)(int)> g_frame_callbacks;
std::atomic<bool> g_exiting{false};
int64_t g_main_menu_since = 0;

void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
}

// General.KeepSessions: before a new session folder is made, the oldest ones in the same parent go until keep - 1
// remain. Only folders named as this plugin names them (YYYYMMDD-HHMMSS-session) with files directly inside.
void prune_sessions(const std::wstring& parent, int keep) {
    if (keep <= 0) return;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((parent + L"\\*-session").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        bool ours = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n.size() == 23 && n[8] == L'-' &&
                    n.compare(15, 8, L"-session") == 0;
        for (size_t i = 0; ours && i < 15; ++i)
            if (i != 8 && (n[i] < L'0' || n[i] > L'9')) ours = false;
        if (ours) names.push_back(n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i + size_t(keep) <= names.size(); ++i) {  // the new session makes it `keep`
        const std::wstring dir = parent + L"\\" + names[i];
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        RemoveDirectoryW(dir.c_str());
    }
}

std::wstring timestamp_name() {
    time_t t = time(nullptr);
    tm local{};
    localtime_s(&local, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y%m%d-%H%M%S", &local);
    return buf;
}

void run_flush_handlers(bool at_exit) {
    for (auto fn : g_flush_handlers) fn(at_exit);
}

// The game can hang while it shuts down: SaveDataProviderWin32Steam::destroy signals its save thread and then joins
// it, and if the thread wakes before it is told to quit it goes back to sleep and never exits (a black window). Once
// the game state is GS_SHUTDOWN the process gets this long to exit by itself.
constexpr DWORD kShutdownGraceMs = 10000;
DWORD WINAPI shutdown_watchdog_main(LPVOID) {
    Sleep(kShutdownGraceMs);
    exit_now("the game did not finish shutting down (save-data thread stuck); ending it");
}

DWORD WINAPI watchdog_main(LPVOID) {
    Sleep(DWORD(g_cfg.exit_after_s) * 1000);
    exit_now("watchdog: ExitAfterSeconds elapsed");
}

}  // namespace

void init(const Config& cfg, const std::wstring& module_dir) {
    g_cfg = cfg;
    g_plugin_dir = module_dir;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    g_ms_per_tick = 1000.0 / double(f.QuadPart);
    g_t0 = now.QuadPart;

    wchar_t env[1024];
    DWORD n = GetEnvironmentVariableW(L"RFGPHYS_RUN_DIR", env, 1024);
    if (n > 0 && n < 1024) {
        g_run_dir.assign(env, n);
    } else {
        std::wstring parent = cfg.output_dir.empty() ? module_dir + L"\\rfgphys_runs" : cfg.output_dir;
        prune_sessions(parent, cfg.keep_sessions);
        g_run_dir = parent + L"\\" + timestamp_name() + L"-session";
    }
    make_dirs(g_run_dir);
    logx::open(g_run_dir + L"\\rfgphys.log");
}

const Config& config() { return g_cfg; }
const std::wstring& run_dir() { return g_run_dir; }
const std::wstring& plugin_dir() { return g_plugin_dir; }

int64_t qpc() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

double ticks_to_ms(int64_t ticks) { return double(ticks) * g_ms_per_tick; }
double since_start_ms() { return ticks_to_ms(qpc() - g_t0); }

void add_flush_handler(void (*fn)(bool)) { g_flush_handlers.push_back(fn); }
void add_frame_callback(void (*fn)(int)) { g_frame_callbacks.push_back(fn); }

void on_frame(int game_state) {
    static int last_state = -1000;
    if (game_state != last_state) {
        LOG("game state %d (%s) -> %d (%s) at %.1f ms", last_state, rfg::game_state_name(last_state), game_state,
            rfg::game_state_name(game_state), since_start_ms());
        last_state = game_state;
        if (game_state == 56 /* GS_SHUTDOWN */)
            CloseHandle(CreateThread(nullptr, 64 * 1024, shutdown_watchdog_main, nullptr, 0, nullptr));
    }
    if (g_cfg.exit_after_main_menu_s > 0) {
        const bool in_menu = game_state == 0 /* GS_MAINMENU */ || game_state == 48 /* GS_MAINMENU_IDLE */;
        if (in_menu && !g_main_menu_since) g_main_menu_since = qpc();
        if (g_main_menu_since && ticks_to_ms(qpc() - g_main_menu_since) >= g_cfg.exit_after_main_menu_s * 1000.0)
            exit_now("ExitAfterMainMenuSeconds elapsed");
    }
    for (auto fn : g_frame_callbacks) fn(game_state);
}

void start_watchdog() {
    if (g_cfg.exit_after_s > 0) CloseHandle(CreateThread(nullptr, 64 * 1024, watchdog_main, nullptr, 0, nullptr));
}

void exit_now(const char* reason) {
    if (!g_exiting.exchange(true)) {
        LOG("exit requested: %s", reason);
        run_flush_handlers(false);
        LOG("session end (requested exit)");
        logx::flush_at_exit();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    for (;;) Sleep(1000);
}

void on_process_exit() {
    if (g_exiting.exchange(true)) return;
    run_flush_handlers(true);
    LOG("session end (process exit)");
    logx::flush_at_exit();
}

}  // namespace session
