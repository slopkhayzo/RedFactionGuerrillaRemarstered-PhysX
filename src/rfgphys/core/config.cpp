#include "core/config.h"

#include <windows.h>

#include <cstdio>
#include <cwctype>

namespace {

std::wstring env_name(const wchar_t* section, const wchar_t* key) {
    std::wstring n = L"RFGPHYS_";
    for (const wchar_t* p = section; *p; ++p) n += wchar_t(towupper(*p));
    n += L'_';
    for (const wchar_t* p = key; *p; ++p) n += wchar_t(towupper(*p));
    return n;
}

bool env_value(const wchar_t* section, const wchar_t* key, std::wstring& out) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(env_name(section, key).c_str(), buf, 1024);
    if (n == 0 || n >= 1024) return false;
    out.assign(buf, n);
    return true;
}

std::wstring get_string(const std::wstring& ini, const wchar_t* section, const wchar_t* key, const wchar_t* def) {
    std::wstring v;
    if (env_value(section, key, v)) return v;
    wchar_t buf[1024];
    GetPrivateProfileStringW(section, key, def, buf, 1024, ini.c_str());
    return buf;
}

int get_int(const std::wstring& ini, const wchar_t* section, const wchar_t* key, int def) {
    std::wstring v;
    if (env_value(section, key, v)) return _wtoi(v.c_str());
    // Not GetPrivateProfileIntW: it reads negative values (HavokThreads=-1) as 0.
    wchar_t buf[64];
    if (GetPrivateProfileStringW(section, key, L"", buf, 64, ini.c_str()) == 0) return def;
    return _wtoi(buf);
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

Config load_config(const std::wstring& ini_path) {
    Config c;
    c.ini_path = ini_path;
    c.enabled = get_int(ini_path, L"General", L"Enabled", 1) != 0;
    c.output_dir = get_string(ini_path, L"General", L"OutputDir", L"");
    c.keep_sessions = get_int(ini_path, L"General", L"KeepSessions", 0);
    c.hang_s = get_int(ini_path, L"General", L"HangSeconds", 30);
    if (c.hang_s > 0 && c.hang_s < 10) c.hang_s = 10;
    c.telemetry = get_int(ini_path, L"Telemetry", L"Enabled", 1) != 0;
    c.contact_counters = get_int(ini_path, L"Telemetry", L"ContactCounters", 1) != 0;
    // Off by default: hooking ~30,000 constant-buffer uploads per heavy frame costs the render thread ~0.2 ms.
    c.render_census = get_int(ini_path, L"Telemetry", L"RenderCensus", 0) != 0;
    c.cb_redundancy = c.render_census && get_int(ini_path, L"Telemetry", L"CbRedundancy", 0) != 0;
    c.render_routing = get_int(ini_path, L"Telemetry", L"RenderRouting", 0) != 0;
    c.profiler = get_int(ini_path, L"Profiler", L"Enabled", 0) != 0;
    c.profiler_interval_us = get_int(ini_path, L"Profiler", L"IntervalUs", 1000);
    if (c.profiler_interval_us < 200) c.profiler_interval_us = 200;
    c.profiler_gameplay_only = get_int(ini_path, L"Profiler", L"GameplayOnly", 1) != 0;
    c.exit_after_main_menu_s = get_int(ini_path, L"Test", L"ExitAfterMainMenuSeconds", 0);
    c.exit_after_gameplay_s = get_int(ini_path, L"Test", L"ExitAfterGameplaySeconds", 0);
    c.exit_after_s = get_int(ini_path, L"Test", L"ExitAfterSeconds", 0);
    c.exp_unbatched_cap = get_int(ini_path, L"Experiment", L"UnbatchedCap", 0);
    c.exp_batched_cap = get_int(ini_path, L"Experiment", L"BatchedCap", 0);
    c.test_hang_after_s = get_int(ini_path, L"Test", L"HangAfterSeconds", 0);
    c.test_zone_cycle = get_int(ini_path, L"Test", L"ZoneCycle", 0) != 0;
    c.test_zone_tour = get_int(ini_path, L"Test", L"ZoneTour", 0);
    c.test_fly_camera = get_int(ini_path, L"Test", L"FlyCamera", 0) != 0;
    c.test_stand_in_probe = get_int(ini_path, L"Test", L"StandInProbe", 0) != 0;
    c.test_stand_in_drives_zone = get_int(ini_path, L"Test", L"StandInDrivesZone", 0) != 0;
    c.test_vehicle_push = get_int(ini_path, L"Test", L"VehiclePush", 0) != 0;
    c.test_restore_building = get_int(ini_path, L"Test", L"RestoreBuilding", 0);
    c.test_view_shots = get_int(ini_path, L"Test", L"ViewShots", 0);
    c.test_solver_guard = get_int(ini_path, L"Test", L"SolverGuard", 0) != 0;
    c.test_havok_audit = get_int(ini_path, L"Test", L"HavokAudit", 0);
    c.test_havok_audit_jobs = get_int(ini_path, L"Test", L"HavokAuditJobs", 0) != 0;
    c.test_havok_jobs_on_sim_thread = get_int(ini_path, L"Test", L"HavokJobsOnSimThread", 0) != 0;
    c.test_visit = get_string(ini_path, L"Test", L"Visit", L"");
    c.test_inspect_frames = get_string(ini_path, L"Test", L"InspectFrames", L"");
    c.auto_load_save = get_int(ini_path, L"Test", L"AutoLoadSave", 0) != 0;
    c.block_save_writes = get_int(ini_path, L"Test", L"BlockSaveWrites", 0) != 0;
    c.test_god_mode = get_int(ini_path, L"Test", L"GodMode", 0) != 0;
    c.test_reset_destruction = get_int(ini_path, L"Test", L"ResetDestruction", 0) != 0;
    c.test_auto_new_game = get_int(ini_path, L"Test", L"AutoNewGame", 0) != 0;
    // DebrisCollide replaced WorldOnlyDebrisCollide (which only changed layers and so had no visible effect).
    c.exp_debris_collide = get_int(ini_path, L"Experiment", L"DebrisCollide",
                                   get_int(ini_path, L"Experiment", L"WorldOnlyDebrisCollide", 0));
    if (c.exp_debris_collide < 0 || c.exp_debris_collide > 2) c.exp_debris_collide = 1;
    c.exp_solver_buffer_mb = get_int(ini_path, L"Experiment", L"SolverBufferMB", 16);
    if (c.exp_solver_buffer_mb > 256) c.exp_solver_buffer_mb = 256;
    c.exp_gpu_occlusion = get_int(ini_path, L"Experiment", L"GpuOcclusion", 1);
    c.exp_shadow_min_radius_far_cm = get_int(ini_path, L"Experiment", L"ShadowMinRadiusFarCm", 0);
    c.exp_shadow_min_radius_mid_cm = get_int(ini_path, L"Experiment", L"ShadowMinRadiusMidCm", 0);
    c.exp_shadow_min_radius_near_cm = get_int(ini_path, L"Experiment", L"ShadowMinRadiusNearCm", 0);
    c.exp_havok_threads = get_int(ini_path, L"Experiment", L"HavokThreads", 0);
    if (c.exp_havok_threads < 0) {
        // About one worker per physical core (half the logical processors) beyond the sim and render threads.
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        const int workers = int(si.dwNumberOfProcessors) / 2 - 1;
        c.exp_havok_threads = workers < 1 ? 1 : workers > 5 ? 5 : workers;
        c.exp_havok_threads_auto = true;
    }
    if (c.exp_havok_threads > 11) c.exp_havok_threads = 11;
    c.exp_havok_min_island_size = get_int(ini_path, L"Experiment", L"HavokMinIslandSize", 0);
    c.exp_keep_debris = get_int(ini_path, L"Experiment", L"KeepDebris", 0);
    c.exp_debris_pool = get_int(ini_path, L"Experiment", L"DebrisPool", 0);
    c.exp_render_instances = get_int(ini_path, L"Experiment", L"RenderInstances", 0);
    c.exp_havok_heap_mb = get_int(ini_path, L"Experiment", L"HavokHeapMB", 0);
    c.exp_zone_reserve = get_int(ini_path, L"Experiment", L"ZoneReserve", 1200);
    c.exp_physx = get_int(ini_path, L"Experiment", L"PhysX", 0);
    c.exp_physx_rate = get_int(ini_path, L"Experiment", L"PhysXRate", 60);
    c.exp_physx_max_bodies = get_int(ini_path, L"Experiment", L"PhysXMaxBodies", 65536);
    c.exp_physx_test = get_int(ini_path, L"Experiment", L"PhysXTest", 0);
    c.exp_physx_test_crash_after_s = get_int(ini_path, L"Experiment", L"PhysXTestCrashAfterS", 0);
    c.exp_collision_survey = get_int(ini_path, L"Experiment", L"CollisionSurvey", 0);
    c.exp_physx_world = get_int(ini_path, L"Experiment", L"PhysXWorld", 0);
    c.exp_physx_world_check = get_int(ini_path, L"Experiment", L"PhysXWorldCheck", 0);
    c.exp_physx_world_hf = get_int(ini_path, L"Experiment", L"PhysXWorldHf", 0);
    c.exp_physx_debris = get_int(ini_path, L"Experiment", L"PhysXDebris", 0);
    c.exp_physx_debris_batch = get_int(ini_path, L"Experiment", L"PhysXDebrisBatch", 1);
    c.exp_physx_debris_drop_meshes = get_int(ini_path, L"Experiment", L"PhysXDebrisDropMeshes", 1);
    c.exp_physx_solid_debris = get_int(ini_path, L"Experiment", L"PhysXSolidDebris", 0);
    c.exp_physx_solid_mass_kg = get_int(ini_path, L"Experiment", L"PhysXSolidMassKg", 0);
    c.exp_physx_adopt_delay_ms = get_int(ini_path, L"Experiment", L"PhysXAdoptDelayMs", 2000);    c.fly_camera = get_int(ini_path, L"FlyCamera", L"Enabled", 0) != 0;
    c.fly_camera_key = get_int(ini_path, L"FlyCamera", L"Key", VK_F10);
    if (c.fly_camera_key < 1 || c.fly_camera_key > 0xFE) c.fly_camera_key = VK_F10;
    c.fly_camera_speed = get_int(ini_path, L"FlyCamera", L"Speed", 10);
    if (c.fly_camera_speed < 1) c.fly_camera_speed = 1;
    c.fly_camera_look_speed = get_int(ini_path, L"FlyCamera", L"LookSpeed", 100);
    if (c.fly_camera_look_speed < 1) c.fly_camera_look_speed = 1;
    c.fly_camera_hide_hud = get_int(ini_path, L"FlyCamera", L"HideHud", 1) != 0;

    const std::wstring mode = get_string(ini_path, L"Harness", L"Mode", L"off");
    c.harness_mode = _wcsicmp(mode.c_str(), L"record") == 0 ? 1 : _wcsicmp(mode.c_str(), L"replay") == 0 ? 2 : 0;
    c.harness_recordings_dir = get_string(ini_path, L"Harness", L"RecordingsDir", L"");
    c.harness_recording = get_string(ini_path, L"Harness", L"Recording", L"");
    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const std::wstring gog_saves = std::wstring(local) +
        L"\\GOG.com\\Galaxy\\Applications\\51153410217180642\\Storage\\Shared\\Files\\autocloud\\save";
    c.harness_save_dir = get_string(ini_path, L"Harness", L"SaveDir", gog_saves.c_str());
    c.harness_seed = uint32_t(get_int(ini_path, L"Harness", L"Seed", int(0x5EED2026)));
    if (c.harness_seed == 0) c.harness_seed = 1;  // rand_regenerate(0) does not reseed
    c.harness_checkpoint_interval = get_int(ini_path, L"Harness", L"CheckpointInterval", 30);
    if (c.harness_checkpoint_interval < 1) c.harness_checkpoint_interval = 1;
    c.harness_virtual_clock = get_int(ini_path, L"Harness", L"VirtualClock", 1) != 0;
    c.harness_auto_start = get_string(ini_path, L"Harness", L"AutoStart", L"");
    c.harness_auto_start_from = get_string(ini_path, L"Harness", L"AutoStartFrom", L"");
    c.harness_auto_start_options = get_string(ini_path, L"Harness", L"AutoStartOptions", L"");
    c.harness_input_script = get_string(ini_path, L"Harness", L"InputScript", L"");
    c.harness_input_from = get_string(ini_path, L"Harness", L"InputFromRecording", L"");
    c.harness_max_frames = get_int(ini_path, L"Harness", L"MaxFrames", 0);
    c.harness_trace_rng = get_int(ini_path, L"Harness", L"TraceRng", 0) != 0;
    c.harness_screenshot_frames = get_string(ini_path, L"Harness", L"ScreenshotFrames", L"");
    c.harness_screenshot_every = get_int(ini_path, L"Harness", L"ScreenshotEvery", 0);
    c.harness_leave_level_at_end = get_int(ini_path, L"Harness", L"LeaveLevelAtEnd", 0);
    c.harness_replay_passes = get_int(ini_path, L"Harness", L"ReplayPasses", 1);
    c.harness_replay_real_time = get_int(ini_path, L"Harness", L"ReplayRealTime", 0) != 0;
    c.harness_dump_objects_frames = get_string(ini_path, L"Harness", L"DumpObjectsFrames", L"");
    c.harness_track = get_int(ini_path, L"Harness", L"Track", 1) != 0;
    c.harness_lock_track = c.harness_track && get_int(ini_path, L"Harness", L"LockTrack", 1) != 0;
    c.harness_track_tolerance_cm = get_int(ini_path, L"Harness", L"TrackToleranceCm", 25);
    if (c.harness_track_tolerance_cm < 1) c.harness_track_tolerance_cm = 1;
    return c;
}

std::string describe(const Config& c) {
    char buf[3072];
    static const char* const kModes[] = {"off", "record", "replay"};
    snprintf(buf, sizeof(buf),
             "enabled=%d output_dir='%s' keep_sessions=%d hang_s=%d telemetry=%d contact_counters=%d profiler=%d (interval %d us, gameplay only "
             "%d) exit_after_main_menu_s=%d exit_after_gameplay_s=%d exit_after_s=%d test_hang_after_s=%d auto_load_save=%d "
             "block_save_writes=%d exp_debris_collide=%d (solver buffer %d MB) exp_gpu_occlusion=%d "
             "exp_shadow_min_radius_cm=%d/%d/%d exp_havok_threads=%d%s (min island size %d) exp_keep_debris=%d (debris "
             "pool %d, render instances %d, Havok heap %d MB, zone reserve %d) exp_physx=%d (%d Hz, %d slots, test %d bodies, "
             "crash after %d s) exp_collision_survey=%d exp_physx_world=%d (check %d) exp_physx_debris=%d fly_camera=%d "
             "(key %d, %d m/s, look speed %d%%, hide HUD %d) harness=%s (recordings '%s', recording '%s', seed %08X, "
             "checkpoint every %d, virtual clock %d, auto start '%s', input script '%s', max frames %d, trace rng %d, "
             "screenshots '%s' every %d, track %d, lock track %d within %d cm)",
             c.enabled, narrow(c.output_dir).c_str(), c.keep_sessions, c.hang_s, c.telemetry, c.contact_counters, c.profiler,
             c.profiler_interval_us, c.profiler_gameplay_only, c.exit_after_main_menu_s, c.exit_after_gameplay_s,
             c.exit_after_s, c.test_hang_after_s, c.auto_load_save, c.block_save_writes, c.exp_debris_collide, c.exp_solver_buffer_mb,
             c.exp_gpu_occlusion, c.exp_shadow_min_radius_far_cm, c.exp_shadow_min_radius_mid_cm,
             c.exp_shadow_min_radius_near_cm, c.exp_havok_threads, c.exp_havok_threads_auto ? " (auto)" : "",
             c.exp_havok_min_island_size, c.exp_keep_debris,
             c.exp_debris_pool, c.exp_render_instances, c.exp_havok_heap_mb, c.exp_zone_reserve, c.exp_physx,
             c.exp_physx_rate, c.exp_physx_max_bodies, c.exp_physx_test, c.exp_physx_test_crash_after_s,
             c.exp_collision_survey, c.exp_physx_world, c.exp_physx_world_check, c.exp_physx_debris,
             c.fly_camera, c.fly_camera_key, c.fly_camera_speed, c.fly_camera_look_speed, c.fly_camera_hide_hud,
             kModes[c.harness_mode],
             narrow(c.harness_recordings_dir).c_str(), narrow(c.harness_recording).c_str(), c.harness_seed,
             c.harness_checkpoint_interval, c.harness_virtual_clock, narrow(c.harness_auto_start).c_str(),
             narrow(c.harness_input_script).c_str(), c.harness_max_frames, c.harness_trace_rng,
             narrow(c.harness_screenshot_frames).c_str(), c.harness_screenshot_every, c.harness_track, c.harness_lock_track,
             c.harness_track_tolerance_cm);
    return buf;
}
