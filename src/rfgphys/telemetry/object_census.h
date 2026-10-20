#pragma once
#include <cstdint>

// Object census: every world::create_object_internal, object_mover_mark_for_fade and world::destroy_object call,
// counted by calling function and object type. Calls made through a small forwarding function
// (world::create_object<T>, the by-handle destroy_object) are counted for that function's caller. Written to
// creations.csv, fades.csv and deletions.csv in the run folder at exit (fades_refused.csv and creations_failed.csv
// when there are any); per-frame totals go into telemetry.csv.
namespace census {

bool install();  // idempotent

// Fade marks go through only when the filter allows them (experiments/keep_debris.h). Refused ones are counted in
// fades_refused.csv instead of fades.csv.
void set_fade_filter(bool (*allow)(uint8_t* mover, uint32_t caller_rva));

// rfg_dyn_create_shard calls whose object could not be created (the general_mover pool was full), since the last
// call.
uint32_t take_failed_shards();

struct FrameCounts {
    uint32_t movers_created;   // object_mover objects created during the frame (any thread)
    uint32_t destroyed;        // objects destroyed during the frame (any thread)
    uint32_t island_watchdog;  // of those, by the island solver-memory watchdog (havok_sim_islands_process_post)
};
FrameCounts take_frame_counts();

}  // namespace census
