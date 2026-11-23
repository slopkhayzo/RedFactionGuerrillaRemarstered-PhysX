// rfgphys's start (startup.h), shared by the ASI's entry (main.cpp) and games that build rfgphys in.
#include "core/startup.h"

#include <windows.h>

#include <string>
#include <vector>

#include "core/address_space.h"
#include "core/build_check.h"
#include "core/config.h"
#include "core/gameloop.h"
#include "core/hang_watch.h"
#include "core/log.h"
#include "core/session.h"
#include "dev/fly_camera.h"
#include "experiments/debris_collision.h"
#include "experiments/havok_audit.h"
#include "experiments/havok_mt.h"
#include "experiments/keep_debris.h"
#include "experiments/big_buildings.h"
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
#include "test/memory_census.h"
#include "test/restore_building.h"
#include "test/stress_pieces.h"
#include "test/chunk_fall.h"
#include "test/vehicle_push.h"
#include "test/stream_report.h"
#include "test/view_shots.h"
#include "test/viewpoints.h"
#include "test/visit.h"
#include "test/zone_cycle.h"

namespace startup {
namespace {

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

}  // namespace

void run(const std::wstring& dir) {
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
    if (rfg::layout_missing() > 0) return;  // OpenRFG64: logged by select_build
    if (!cfg.enabled) {
        LOG("disabled in config");
        return;
    }
    if (!hooks::init()) return;
    if (!gameloop::install()) {
        LOG("main-loop hook failed: plugin stays inactive");
        return;
    }
    // OpenRFG64: in OpenRFG64 only the modules converted to its layouts run (the PhysX pipeline). The game's
    // limits that keep_debris raises at run time are raised in OpenRFG64's source there (src/memtest); the
    // experiments, the record/replay harness, the tests and the telemetry are not converted.
    const bool full = !rfg::is_openrfg64();
    if (!full)
        LOG("OpenRFG64: only the converted modules run (PhysX bridge, world mirror, debris offload, render routing, hang "
            "watch, and the record/replay harness when Harness.Mode is set); experiments, tests, telemetry and keep_debris "
            "stay off");
    // OpenRFG64: the record/replay harness and the automation it drives the menus with are converted too.
    if (full || cfg.harness_mode != 0) automation::install(cfg);
    if (full) {
        experiments::install_debris_collision(cfg);
        experiments::install_render_tweaks(cfg);
        experiments::install_big_buildings(cfg);
        experiments::install_havok_mt(cfg);
        experiments::install_havok_audit(cfg);
        experiments::install_keep_debris(cfg);
        if (cfg.telemetry) telemetry::install(cfg);  // frame callbacks: telemetry first, so it measures whole frames
    }
    gameloop::add_end(&session::on_frame);
    pxbridge::install(cfg);  // frame callbacks: the bridge takes the host's snapshot before its consumers run
    if (full) {
        pxtest::install(cfg);
        collision_survey::install(cfg);
    }
    world_mirror::install(cfg);
    render_routing::install(cfg);
    debris_offload::install(cfg);  // after the bridge: it moves pieces from the snapshot the bridge takes
    if (full) {
        zone_cycle::install(cfg);
        vehicle_push::install(cfg);
        chunk_fall::install(cfg);
        restore_building::install(cfg);
        stress_pieces::install(cfg);
        memory_census::install(cfg);  // its D3D11 counters with the stress test
        visit::install(cfg);
        view_shots::install(cfg);
        stream_report::install(cfg);
        viewpoints::install(cfg);
    }
    if (full || cfg.harness_mode != 0) harness::install(cfg);
    if (full) fly_camera::install(cfg);  // never with the harness: both take over the game's input
    hang_watch::start(cfg);
    if (full) address_space::install(cfg);
    if (!hooks::enable_all()) return;
    if (full && cfg.telemetry && cfg.profiler) profiler::start(cfg);
    session::start_watchdog();
    LOG("initialization complete");
}

}  // namespace startup
