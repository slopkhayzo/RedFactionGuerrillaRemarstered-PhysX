cmake_minimum_required(VERSION 3.25)
project(rfgphys VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "rfg.exe is 32-bit: configure with -A Win32")
endif()

# Static CRT: no redistributable needed, and no sharing of the game's MSVCR100 heap.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

add_library(rfgphys SHARED
    src/rfgphys/main.cpp
    src/rfgphys/core/log.cpp
    src/rfgphys/core/config.cpp
    src/rfgphys/core/build_check.cpp
    src/rfgphys/core/session.cpp
    src/rfgphys/core/gameloop.cpp
    src/rfgphys/core/contacts.cpp
    src/rfgphys/core/game_lock.cpp
    src/rfgphys/core/thread_dump.cpp
    src/rfgphys/core/hang_watch.cpp
    src/rfgphys/hooks/hooks.cpp
    src/rfgphys/game/rfg.cpp
    src/rfgphys/telemetry/telemetry.cpp
    src/rfgphys/telemetry/object_census.cpp
    src/rfgphys/telemetry/render_census.cpp
    src/rfgphys/telemetry/render_timeline.cpp
    src/rfgphys/telemetry/profiler.cpp
    src/rfgphys/test/automation.cpp
    src/rfgphys/test/zone_cycle.cpp
    src/rfgphys/experiments/debris_collision.cpp
    src/rfgphys/experiments/render_tweaks.cpp
    src/rfgphys/experiments/havok_mt.cpp
    src/rfgphys/experiments/keep_debris.cpp
    src/rfgphys/harness/harness.cpp
    src/rfgphys/harness/rec_file.cpp
    src/rfgphys/harness/checkpoint.cpp
    src/rfgphys/harness/rng_guard.cpp
    src/rfgphys/harness/screenshot.cpp
    src/rfgphys/harness/track.cpp
    src/rfgphys/physx/bridge.cpp
    src/rfgphys/physx/collision_survey.cpp
    src/rfgphys/physx/debris_offload.cpp
    src/rfgphys/physx/havok_shapes.cpp
    src/rfgphys/physx/render_routing.cpp
    src/rfgphys/physx/test_scene.cpp
    src/rfgphys/physx/world_mirror.cpp)
set_target_properties(rfgphys PROPERTIES SUFFIX ".asi" CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_include_directories(rfgphys PRIVATE src/rfgphys src/shared)
target_compile_definitions(rfgphys PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    RFGPHYS_VERSION="${PROJECT_VERSION}")
target_compile_options(rfgphys PRIVATE /W4 /permissive- /utf-8 /Zi)
target_link_options(rfgphys PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO)
target_link_libraries(rfgphys PRIVATE minhook psapi)
# Version resource (src/shared/version.rc.in).
set(RC_DESCRIPTION "rfgphys - physics mod for Red Faction Guerrilla Re-Mars-tered")
set(RC_INTERNAL_NAME "rfgphys")
set(RC_FILENAME "rfgphys.asi")
set(RC_FILE_TYPE "VFT_DLL")
configure_file(src/shared/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/rfgphys_version.rc" @ONLY)
target_sources(rfgphys PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/rfgphys_version.rc")
