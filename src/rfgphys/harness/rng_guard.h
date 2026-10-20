#pragma once
#include <cstdint>
#include <string>

// Randomness during harness sessions.
//
// The game's generator (Vlib_rand_instance) is a pre-filled table read through one shared index by 13 functions
// (vrand, fl_rand, int_rand_range, ..., timestamp::set_rand), and MSVCR100's rand() keeps one state per thread.
// Both are shared by everything on a thread, so a cosmetic consumer whose call count depends on timing (cloth
// flaps waiting for animation results, foliage placement following streaming, render-thread effects) shifts the
// numbers that destruction and gameplay get next.
//
// While a session runs (set_isolation(true)):
// - every call site draws from its own position in the table (a stream per return address, starting at a
//   position derived from the seed and the call site), so call sites cannot disturb each other; call sites on
//   other threads (loader, render) have their own streams, separate from the sim thread's;
// - CRT rand() on the sim thread is a stream per call site too (MSVC's LCG), except inside create_cells, whose
//   foliage placement seeds rand() per terrain cell with srand() and keeps the real CRT.
// Optionally every sim-thread draw is traced (Harness.TraceRng=1) to diff two runs.
namespace rng_guard {

constexpr uint32_t kCrtRand = 13;  // trace id for MSVCR100 rand()

bool install();                     // before hooks::enable_all
void set_isolation(bool on);        // on from the start event to the end of a session
// rand_regenerate(seed) and fresh streams (sim thread): all streams at the start event, the sim thread's at each
// loading frame.
void reseed(uint32_t seed, bool session_start);

// CRT rand() on the sim thread: true and the value when a stream serves the call.
bool crt_rand(uintptr_t caller, int& value);
void enter_cells();                 // create_cells: placement seeded with srand per cell keeps the real CRT
void leave_cells();

// Fingerprints of all sim-thread streams (game generator, CRT), compared frame by frame during replays.
uint32_t vlib_hash();
uint32_t crt_hash();

void set_trace(bool on);
void set_frame(int32_t frame);
void write_trace(const std::wstring& path);  // rng_trace.csv; clears the trace
std::string other_threads_summary();  // draws on other threads by function and caller; resets the counts

}  // namespace rng_guard
