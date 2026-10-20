#pragma once
#include <cstdint>

struct Config;

// Per-frame timing of the main loop, the simulation frame, the Havok step (and its phases) and the systems around
// it, written to telemetry.csv (one row per main-loop frame), memory.csv (address space, once per second) and
// summary.json (gameplay frame-time statistics).
namespace telemetry {

bool install(const Config& cfg);

// For other modules (profiler): current main-loop frame number, last game state, and the two hot threads.
uint32_t current_frame();
int current_state();
unsigned long sim_thread_id();
unsigned long render_thread_id();

}  // namespace telemetry
