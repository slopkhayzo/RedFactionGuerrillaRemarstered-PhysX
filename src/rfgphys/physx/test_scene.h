#pragma once
#include <cstdint>

struct Config;

// X0 test scene for the PhysX host (research/docs/06 §13). With Experiment.PhysXTest=N, once gameplay starts the
// plugin drops N bodies (boxes and convex rocks) in bursts of 1,000 onto a ground plane under the player, then sets
// off an explosion in the pile every 3 s. Nothing is drawn: the scene measures what the game pays per frame to take
// the host's snapshots, copy them (what an upload to the GPU would cost) and interpolate every body (the CPU
// fallback), and how old the poses are when they arrive. Experiment.PhysXTestCrashAfterS=K makes the host end
// itself K s into the scene, to test the game's side of a host crash.
namespace pxtest {

bool install(const Config& cfg);

struct FrameStats {
    float copy_us;    // copying the newest snapshot's records (only in frames that took a new one)
    float interp_us;  // interpolating every live body for this frame
    uint32_t live;    // live bodies in the snapshot
    uint32_t spawned; // bodies sent so far
};
FrameStats take_frame();  // telemetry, simulation thread

}  // namespace pxtest
