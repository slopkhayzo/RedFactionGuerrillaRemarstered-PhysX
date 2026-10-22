// rfgphys: Red Faction Guerrilla Re-Mars-tered physics/performance plugin, loaded by Ultimate ASI Loader.
#include <windows.h>

#include <string>
#include <vector>

#include "core/build_check.h"
#include "core/config.h"
#include "core/gameloop.h"
#include "core/hang_watch.h"
#include "core/log.h"
#include "core/session.h"
#include "dev/fly_camera.h"
#include "experiments/debris_collision.h"
#include "experiments/havok_mt.h"
#include "experiments/keep_debris.h"
#include "experiments/render_tweaks.h"
#include "game/rfg.h"
#include "harness/harness.h"
#include "hooks/hooks.h"
#include "physx/bridge.h"
#include "physx/collision_survey.h"
#include "physx/debris_offload.h"
#include "physx/render_routing.h"
#include "physx/world_mirror.h"
#include "physx/test_scene.h"
#include "telemetry/profiler.h"
#include "telemetry/telemetry.h"
#include "test/automation.h"
#include "test/vehicle_push.h"
#include "test/zone_cycle.h"

namespace {

HMODULE g_module = nullptr;

std::wstring module_dir() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(g_module, path, MAX_PATH);
    std::wstring s(path, n);
    return s.substr(0, s.find_last_of(L"\\/"));
}

void log_system_info() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    LOG("exe %ls loaded at %p, command line: %ls", exe, reinterpret_cast<void*>(rfg::base()), GetCommandLineW());

    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    int cores = 0, logical = 0;
    if (len && GetLogicalProcessorInformationEx(RelationProcessorCore,
                                                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data()),
                                                &len)) {
        for (DWORD off = 0; off < len;) {
            auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
            ++cores;
            for (WORD g = 0; g < info->Processor.GroupCount; ++g) {
                KAFFINITY m = info->Processor.GroupMask[g].Mask;
                while (m) {
                    logical += int(m & 1);
                    m >>= 1;
                }
            }
            off += info->Size;
        }
    }
    DWORD_PTR proc_mask = 0, sys_mask = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask);
    LOG("cpu: %d cores, %d logical processors, process affinity %08lX", cores, logical,
        static_cast<unsigned long>(proc_mask));
}

void init() {
    const std::wstring dir = module_dir();
    Config cfg = load_config(dir + L"\\rfgphys.ini");
    session::init(cfg, dir);
    LOG("rfgphys %s starting (plugin folder %ls)", RFGPHYS_VERSION, dir.c_str());
    LOG("config: %s", describe(cfg).c_str());
    log_system_info();

    std::string detail;
    const int build = identify_build(detail);
    LOG("build check: %s", detail.c_str());
    if (build < 0) {
        LOG("unknown rfg.exe build: plugin stays inactive");
        return;
    }
    rfg::select_build(build);  // before anything uses a game address
    if (!cfg.enabled) {
        LOG("disabled in config");
        return;
    }
    if (!hooks::init()) return;
    if (!gameloop::install()) {
        LOG("main-loop hook failed: plugin stays inactive");
        return;
    }
    automation::install(cfg);
    experiments::install_debris_collision(cfg);
    experiments::install_render_tweaks(cfg);
    experiments::install_havok_mt(cfg);
    experiments::install_keep_debris(cfg);
    if (cfg.telemetry) telemetry::install(cfg);  // frame callbacks: telemetry first, so it measures whole frames
    gameloop::add_end(&session::on_frame);
    pxbridge::install(cfg);  // frame callbacks: the bridge takes the host's snapshot before its consumers run
    pxtest::install(cfg);
    collision_survey::install(cfg);
    world_mirror::install(cfg);
    render_routing::install(cfg);
    debris_offload::install(cfg);  // after the bridge: it moves pieces from the snapshot the bridge takes
    zone_cycle::install(cfg);
    vehicle_push::install(cfg);
    harness::install(cfg);
    fly_camera::install(cfg);  // never with the harness: both take over the game's input
    hang_watch::start(cfg);
    if (!hooks::enable_all()) return;
    if (cfg.telemetry && cfg.profiler) profiler::start(cfg);
    session::start_watchdog();
    LOG("initialization complete");
}

}  // namespace

// Called by Ultimate ASI Loader after LoadLibrary, outside the loader lock, when the game's own code starts.
extern "C" __declspec(dllexport) void InitializeASI() {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) init();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        session::on_process_exit();  // process is terminating: flush what is buffered
    }
    return TRUE;
}
