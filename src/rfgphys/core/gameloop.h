#pragma once

// The game's main-loop frame on the simulation thread (rfg_do_frame): one hook shared by every module that needs
// per-frame work, plus control over the frame time the main loop computes.
namespace gameloop {

bool install();

// Callbacks on the sim thread, run in registration order: before each frame, and after it with the game state the
// frame ended in. The end of a frame is a safe point to call game functions.
void add_begin(void (*fn)());
void add_end(void (*fn)(int game_state));

// Frame time of the main loop. rfg_do_frame calls frametime_calculate(false, 0, fixed_rate = 0); `fixed_rate`
// (optional) returns the rate to pass instead: 0 keeps the game's real-time behaviour (and its frame limiter),
// r > 0 makes the frame exactly (float)(1.0 / r) seconds long. `after` runs right after frametime_calculate.
void set_frametime_control(float (*fixed_rate)(), void (*after)());

unsigned long sim_thread_id();
bool on_sim_thread();

// Main-loop frames ended so far, and the game state the last one ended in (any thread).
unsigned long long frames_ended();
int last_state();

}  // namespace gameloop
