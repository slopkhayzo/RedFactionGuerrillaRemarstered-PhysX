#pragma once

struct Config;

// Sampling profiler: periodically suspends the sim and render threads, records the instruction pointer plus
// return addresses found by scanning the stack, and writes them to profile.bin (+ profile_modules.csv) in the run
// folder. tools/profile_report.py turns that into per-thread hot-function tables using the PDB symbol tables.
namespace profiler {

bool start(const Config& cfg);

}  // namespace profiler
