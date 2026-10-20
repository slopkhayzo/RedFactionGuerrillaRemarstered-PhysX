#pragma once
#include <cstdint>

struct Config;

// X2 (research/docs/06 §4 v1, §13): debris leaves the game for the PhysX host. With Experiment.PhysX,
// Experiment.PhysXWorld and Experiment.PhysXDebris=1, at the end of each frame every debris piece (a general_mover
// the load governor may delete, with a dynamic Havok body) is adopted:
// - its convex parts (in the body's frame), pose, velocities and mass go to the host, which simulates it against the
//   mirrored static world (physx/world_mirror.h) and every other piece;
// - its render instance is taken over (the general_mover would release it) and moved from the host's snapshots
//   every frame, so the piece looks exactly as before;
// - the game object is destroyed, with its Havok body and its world handle, so neither Havok nor the world's
//   object limits (01 §10.10) see it any more.
// Explosions are forwarded to the host as velocity kicks, and the player is a kinematic capsule there, so pieces
// are pushed around. What does not work yet: pieces do not stop the player, vehicles or projectiles, and they take
// no damage (research/docs/06 §6, X3).
namespace debris_offload {

bool install(const Config& cfg);

struct FrameStats {
    uint32_t adopted;   // pieces taken over this frame
    uint32_t live;      // pieces the host simulates for the game
    uint32_t moved;     // render instances moved this frame
    uint32_t released;  // pieces given up (fell out of the world, level end)
    float adopt_ms, move_ms;
    uint32_t stand_ins;  // Havok stand-ins of heavy pieces near the player (Experiment.PhysXSolidDebris)
};
FrameStats take_frame();  // telemetry, simulation thread

}  // namespace debris_offload
