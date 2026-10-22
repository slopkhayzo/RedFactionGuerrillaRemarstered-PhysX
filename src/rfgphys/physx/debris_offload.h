#pragma once
#include <cstdint>
#include <vector>

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
// Explosions are forwarded to the host as velocity kicks; the player is a kinematic capsule there, and vehicles,
// walkers and loose building chunks are kinematic pushers, so pieces are pushed around. Havok sees the pieces where it
// matters through stand-ins with their own shapes (near the player, ahead of projectiles, on the line of fire), so the
// player stands on heavy rubble, rockets strike piles, and melee blows and bullets push pieces. What does not work
// yet: vehicles do not feel the pieces (they plough through), and pieces take no damage (research/docs/06 §6, X3).
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

// For unattended tests (simulation thread): the live pieces' host slots within `radius` of `center`, and where a piece
// is now (from the newest snapshot; false if the slot holds no piece).
std::vector<uint32_t> pieces_near(const float center[3], float radius);
bool piece_position(uint32_t slot, float out[3]);

}  // namespace debris_offload
