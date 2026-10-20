#pragma once
#include <cstdint>
#include <string>

#include "core/config.h"

// One game process = one session with its own run folder (log, telemetry, summary).
namespace session {

void init(const Config& cfg, const std::wstring& module_dir);
const Config& config();
const std::wstring& run_dir();
const std::wstring& plugin_dir();  // the folder rfgphys.asi was loaded from (the game's scripts folder)

int64_t qpc();
double ticks_to_ms(int64_t ticks);
double since_start_ms();

// Handlers run on exit_now() and on process exit to flush buffered output. `at_exit` is true when called from
// DllMain during process termination, where other threads are already gone and locks must not be waited on.
void add_flush_handler(void (*fn)(bool at_exit));

// Registered as a main-loop end-of-frame callback (core/gameloop.h): sim thread, after each frame.
void on_frame(int game_state);
// Per-frame callbacks run by on_frame (sim thread, end of frame: a safe point to call game functions).
void add_frame_callback(void (*fn)(int game_state));

void start_watchdog();
[[noreturn]] void exit_now(const char* reason);
void on_process_exit();

}  // namespace session
