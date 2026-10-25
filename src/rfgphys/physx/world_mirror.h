#pragma once
#include <cstdint>

struct Config;

// X1: the static world in the PhysX host (research/docs/06 §5, §13). With Experiment.PhysX and
// Experiment.PhysXWorld=1, every body of Havok's fixed island (terrain, rocks, intact buildings) is mirrored into
// the host's scene as world-space static geometry: triangle meshes (in chunks) and convex hulls, one group per Havok
// body. Every 250 ms of gameplay the fixed island is compared with what was mirrored: new bodies are added, bodies
// that left are removed, and buildings whose piece count changed (rfg_destroyable_shape) are mirrored again.
//
// Experiment.PhysXWorldCheck=1 verifies the mirror once it is complete: 1,024 vertical rays around the player are
// cast against Havok's static level and destroyable geometry and against the host's statics, and the hit distances
// are compared (log line "world mirror check").
namespace world_mirror {

bool install(const Config& cfg);
bool ready();  // the level's static world is in the host (first export sent), so debris can be handed over
// Experiment.PhysXWorldCheck: the check once more, where the player is in 3 s (tests, after zones streamed).
void check_again();

struct FrameStats {
    uint32_t groups;     // Havok bodies mirrored
    uint32_t queued_kb;  // commands waiting for room in the ring
    uint32_t sent_kb;    // sent this frame
    uint32_t changes;    // bodies added, removed or mirrored again this frame
    float scan_ms;       // walking Havok's fixed island and exporting (this frame)
};
FrameStats take_frame();  // telemetry, simulation thread

}  // namespace world_mirror
