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
//
// OpenRFG64, big maps (Experiment.PhysXWorldStatics, the area mode): a level whose static world would make more
// statics in the host than that is mirrored in part. The bodies within PhysXWorldNearM of the player go whole (and any
// body that is not a building); of the other buildings, only the pieces near the pieces in PhysX (where they lie, and
// where the moving ones go in the next 2 s) and near where pieces wait to come over (ready_at). A body no longer wanted
// leaves the host 10 s later. A piece comes over only once the static world around it is in the host (ready_at);
// until then it stays with Havok, and its area is asked for. A level within the budget goes whole.
namespace world_mirror {

bool install(const Config& cfg);
// The level's static world is in the host (first export sent), so debris can be handed over. Area mode: the bodies
// within PhysXWorldNearM of the player are (from then on for the level).
bool ready();
// Area mode: the static world around `pos` (`radius` to the sides, from below up to a little above it) is in the
// host and applied there, with the pieces its buildings lost; if not, the area is asked for and comes within a second
// or two. Without the area mode: ready().
bool ready_at(const float pos[3], float radius);
// The area mode is on (Experiment.PhysXWorldStatics): the static world may be in the host in part.
bool partial();
// Complete scans (every body looked at) whose commands are all in the ring (UINT32_MAX without a mirror). Once it has
// moved on since a change to the level's buildings, and the host has read the ring, the host's statics match them:
// Test.StressPieces waits for that after it restores the buildings (research/docs/07, B5b: blown up again while the
// host still cooked them, the new pieces came over inside the old statics and were pushed out underneath).
uint32_t flushed_scans();
// Everything that left the buildings so far is on its way to the host, ahead of whatever is sent after this; false:
// not yet (the export's budget, a full ring). A new loose chunk's parts must leave the static world before they
// come back as a body: research/docs/08, C3.
bool catch_up();
// The game object a static group of the host stands for (a building, the ground), or null if it is gone: for the loose
// chunks' impacts on the static world (research/docs/08, C2). Simulation thread.
uint8_t* object_of_group(uint32_t group);
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
