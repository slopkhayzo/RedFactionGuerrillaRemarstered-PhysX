#pragma once
#include <cstdint>
#include <string>

// Settings come from rfgphys.ini next to the plugin. Every key can be overridden by an environment variable
// named RFGPHYS_<SECTION>_<KEY> (upper case), which is how queue jobs configure a run.
struct Config {
    bool enabled = true;
    std::wstring output_dir;          // parent folder for per-session run folders
    int keep_sessions = 0;            // > 0: only the newest this many session folders are kept (General.KeepSessions)
    // A gameplay frame that has not ended after this many seconds while a game thread spins (in unattended runs:
    // whatever the threads do) ends the process with a hang report in the log (core/hang_watch.h); 0: off.
    int hang_s = 30;

    bool telemetry = true;
    bool contact_counters = true;     // count contact-point callbacks per frame
    bool render_census = false;       // count constant-buffer uploads and occlusion queries (telemetry/render_census.h;
                                      // costs the render thread ~0.2 ms in heavy frames)
    bool cb_redundancy = false;       // diagnostic: detect repeated constant-buffer uploads (costs render-thread time)
    bool render_routing = false;      // diagnostic: how destroyable models (debris) are drawn (physx/render_routing.h)

    bool profiler = false;            // sampling profiler (needs telemetry)
    int profiler_interval_us = 1000;
    bool profiler_gameplay_only = true;

    int exit_after_main_menu_s = 0;   // > 0: quit this many seconds after the main menu first appears
    int exit_after_gameplay_s = 0;    // > 0: quit this many seconds after gameplay first starts
    int exit_after_s = 0;             // > 0: watchdog, quit this many seconds after start-up
    int test_hang_after_s = 0;        // > 0: diagnostic, the sim thread spins forever this many seconds into
                                      // gameplay (tests the hang watch, core/hang_watch.h)
    bool test_zone_cycle = false;     // blow up buildings near the player, teleport away until their zone streams
                                      // out, and back (test/zone_cycle.h)
    bool test_fly_camera = false;     // a scripted fly camera flight with screenshots (dev/fly_camera.h)
    bool test_stand_in_probe = false; // every 2 s: a ray through a piece's stand-in and a blow on it, checked
                                      // (physx/debris_offload.cpp)
    bool test_vehicle_push = false;   // a vehicle moved through rubble in PhysX, checked (test/vehicle_push.h)
    std::wstring test_inspect_frames; // replay frames (comma-separated) at which what the camera looks at is logged
                                      // (physx/debris_offload.cpp)
    bool auto_load_save = false;      // main menu -> Load Game -> most recent save, without input
    bool block_save_writes = false;   // never write or erase save data (always on for unattended runs)

    // Experiments (change game behaviour; off by default, never applied in multiplayer).
    // Debris collides with debris (experiments/debris_collision.h): 0 off, 1 collisions only (the game's impact
    // logic is skipped for debris-debris contacts, so pieces never damage each other), 2 collisions and the game's
    // impact logic (debris damages debris and breaks it up).
    int exp_debris_collide = 0;
    // With DebrisCollide: size of Havok's island solver buffer in MB (the game uses 1 MB and deletes the debris of
    // islands that need more; colliding debris merges into such islands). 1 or less keeps the game's buffer.
    int exp_solver_buffer_mb = 16;
    // GPU occlusion queries (RL_gpu_vis_enabled): 1 the game's default, 0 off (every object counts as visible to
    // the occlusion pass; frustum and distance culling stay).
    int exp_gpu_occlusion = 1;
    // Shadow cascades: objects whose bounding sphere radius is below this (cm) cast no shadow into the far / mid /
    // near cascade (0: the game's behaviour). Small objects there are a few shadow-map texels at most.
    int exp_shadow_min_radius_far_cm = 0;
    int exp_shadow_min_radius_mid_cm = 0;
    int exp_shadow_min_radius_near_cm = 0;
    // A batched render pass draws at most 4,096 instances one by one (rl_batched_pass::m_unbatched_renderables, a
    // static array); past that, instances are not drawn that frame. > 4,096: room for this many (up to 65,536).
    int exp_unbatched_cap = 0;
    // A frame draws at most 24,576 instances batched (instanced; past that, partition_batched_instances drops whole
    // material batches), from a per-frame stream of 30,247 instances and a 1.5 MB GPU instancing buffer. > 24,576:
    // room for this many (up to 262,144).
    int exp_batched_cap = 0;
    // Havok steps the world with its multithreaded simulation on the sim thread plus this many worker threads
    // (experiments/havok_mt.h); 0 keeps the game's single-threaded step. At most 11. -1: by the processor, one worker
    // per core beyond the game's sim and render threads, 1 to 5 (5 on 8 cores / 16 threads).
    int exp_havok_threads = 0;
    bool exp_havok_threads_auto = false;  // set from HavokThreads=-1
    // With HavokThreads: hkpWorldCinfo::m_minDesiredIslandSize of the multithreaded world. Havok merges islands
    // smaller than this into "sparse" islands (fewer, larger jobs), which sleep only when all their bodies rest;
    // 0 (default) keeps islands as small as the single-threaded game has them, -1 keeps Havok's 64.
    int exp_havok_min_island_size = 0;
    // The game stops removing debris (experiments/keep_debris.h): no shatter-time deletions, fade-outs or load
    // governor deletions, in single player.
    int exp_keep_debris = 0;
    // general_mover pool size (debris pieces); 0 keeps the game's 3,072. Up to 65,000.
    int exp_debris_pool = 0;
    // Render instances for building chunks and debris pieces (rl_destroyable_model_instance); 0 keeps the game's
    // 2,100. Shattering stops creating pieces when they run out.
    int exp_render_instances = 0;
    // Havok's heap in MB; 0 (or up to 75) keeps the game's 75 MB. Colliding piles need about 15 KB per piece.
    int exp_havok_heap_mb = 0;
    // With KeepDebris: object handles kept free in each world zone (a zone has 7,280); below it the debris farthest
    // from the camera is recycled. 200 to 5,000.
    int exp_zone_reserve = 1200;
    // PhysX host process (physx/bridge.h): 0 off, 1 GPU (CPU when CUDA is unavailable), 2 CPU.
    int exp_physx = 0;
    int exp_physx_rate = 60;            // fixed steps per second
    int exp_physx_max_bodies = 65536;   // body slots (shared memory: 192 bytes per slot)
    // X0 test scene (physx/test_scene.h): bodies dropped once gameplay starts, and the seconds after which the host
    // is told to crash (0: never).
    int exp_physx_test = 0;
    int exp_physx_test_crash_after_s = 0;
    // X1 groundwork (physx/collision_survey.h): walk the static world's Havok shapes 5 s into each level and log them.
    int exp_collision_survey = 0;
    // X1 (physx/world_mirror.h): mirror Havok's static world into the PhysX host; and check it with raycasts.
    int exp_physx_world = 0;
    int exp_physx_world_check = 0;
    int exp_physx_world_hf = 0;  // diagnostic: bit 0 x-major heights, bit 1 flipped height-field split
    // X2 (physx/debris_offload.h): debris pieces leave the game for the PhysX host (needs PhysX and PhysXWorld).
    int exp_physx_debris = 0;
    int exp_physx_debris_batch = 1;  // their render instances stay batchable (drawn instanced; 0: one by one)
    // With it: the compiled meshes the game builds for every piece (unused when drawn batched) are given back.
    int exp_physx_debris_drop_meshes = 1;
    // Heavy pieces (Character_collision_push_away_mass or more) near the player get Havok stand-ins, so the player
    // stands on rubble; the player's capsule in PhysX pushes only the light ones.
    int exp_physx_solid_debris = 0;
    int exp_physx_solid_mass_kg = 0;  // with it: pieces this heavy or more are solid (0: the game's push-away mass)
    // A new piece stays in Havok this long (game ms) before it goes to PhysX: what it hits and lies on the game feels.
    int exp_physx_adopt_delay_ms = 2000;

    // Fly camera for screenshots (dev/fly_camera.h; development, off in the release settings).
    bool fly_camera = false;
    int fly_camera_key = 0x79;        // virtual-key code of the toggle (F10)
    int fly_camera_speed = 10;        // m/s
    int fly_camera_look_speed = 100;  // mouse and arrow-key turning, percent
    bool fly_camera_hide_hud = true;  // the HUD is hidden while flying

    // Record/replay harness (harness/harness.h).
    int harness_mode = 0;                 // 0 off, 1 record, 2 replay
    std::wstring harness_recordings_dir;  // record: new recordings go here
    std::wstring harness_recording;       // replay: the recording folder to play back
    std::wstring harness_save_dir;        // the game's save folder (recordings copy the loaded save from here)
    uint32_t harness_seed = 0x5EED2026;   // seeds both random generators at the start of a recording
    int harness_checkpoint_interval = 30; // frames between state checkpoints
    bool harness_virtual_clock = true;    // wall-clock reads on the sim thread follow recorded frame times
    std::wstring harness_auto_start;      // record: "wc" starts a Wrecking Crew match without input (self-tests)
    std::wstring harness_auto_start_from; // record: with AutoStart=wc, use this recording's Wrecking Crew setup
    std::wstring harness_auto_start_options;  // record: with AutoStart=wc, this setup (wrecking_crew_options as hex)
    std::wstring harness_input_script;    // record: scripted input instead of keyboard and mouse (self-tests)
    std::wstring harness_input_from;      // record: another recording's input frames instead of keyboard and mouse
    int harness_max_frames = 0;           // record: end the recording after this many frames (0: no limit)
    bool harness_trace_rng = false;       // diagnostic: log every random draw on the sim thread (rng_trace.csv)
    std::wstring harness_screenshot_frames;  // comma-separated frame numbers to capture (shot_<frame>.png)
    int harness_screenshot_every = 0;     // also capture every N frames (0: off)
    int harness_leave_level_at_end = 0;   // replay: leave the level after the last frame, then end (tests level end)
    std::wstring harness_dump_objects_frames;  // comma-separated frame numbers: objects_<frame>.csv (diagnostic)
    bool harness_track = true;            // record and replay the player track (harness/track.h); 0: no track hooks
    bool harness_lock_track = true;       // replay: lock camera, player and shots to the recording's track.bin
    int harness_track_tolerance_cm = 25;  // replay: put the player back when this far off the recorded path

    std::wstring ini_path;
};

Config load_config(const std::wstring& ini_path);
std::string describe(const Config& c);
