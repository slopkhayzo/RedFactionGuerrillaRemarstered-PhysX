#pragma once
#include <cstdint>

#include "rfgphysx_ipc.h"

struct Config;

// The game side of the PhysX host process (research/docs/06 §3, §13). With Experiment.PhysX=1 (GPU) or 2 (CPU) the
// plugin creates the shared memory of src/shared/rfgphysx_ipc.h and starts scripts\rfgphysx\rfgphysx_host.exe in a
// job object, so the host ends with the game. Nothing here ever waits for the host: commands go into a ring (a full
// ring drops them and counts it), snapshots are taken when one is ready, and a host that fails, exits or stops
// answering is logged and left alone while the game goes on.
namespace pxbridge {

bool install(const Config& cfg);
bool active();  // the host was started and has not been lost
bool ready();   // the host is stepping and applies commands

// Commands, simulation thread. false: not sent (no host, or the ring is full).
bool reset();
bool add_static_plane(const rfgphysx::Vec3& normal, float distance);
bool add_static_box(const rfgphysx::Vec3& pos, const rfgphysx::Quat& rot, const rfgphysx::Vec3& half_extents);
bool shape_box(uint32_t shape, const rfgphysx::Vec3& half_extents);
bool shape_convex(uint32_t shape, const rfgphysx::Vec3* vertices, uint32_t count);
bool create_body(uint32_t slot, uint32_t shape, const rfgphysx::Vec3& pos, const rfgphysx::Quat& rot,
                 const rfgphysx::Vec3& lin_vel, const rfgphysx::Vec3& ang_vel, float density, uint32_t flags = 0);
bool destroy_body(uint32_t slot);
bool explosion(const rfgphysx::Vec3& center, float radius, float impulse, float up);
bool test_crash();
// A complete record built by the caller (header type and size filled in), for modules with their own commands.
bool send_record(const void* record, uint32_t size);
uint32_t ring_free();  // bytes the command ring can take now (a record that does not fit is dropped, not queued)
uint32_t ring_used();  // bytes written that the host has not applied yet
// Raycasts against the mirrored static world (physx/world_mirror.h). The distances (-1: no hit) are ready once
// query_done() returns `request`.
bool raycast_batch(uint32_t request, const rfgphysx::Ray* rays, uint32_t count);
uint32_t query_done();
const float* query_results();
const uint32_t* query_hit_kinds();  // per ray: PhysX geometry type + 1 of what it hit (0: nothing)
uint32_t host_statics();  // static actors in the host's scene
uint32_t max_bodies();
float step_hz();
uint64_t host_cpu_100ns();  // CPU time (kernel + user, 100 ns units) the host process has used; 0 without one

// The newest snapshot, taken at the start of this frame's callbacks (simulation thread); null before the first one.
// `fresh` is true in the frame it arrived. Valid until the next frame.
const rfgphysx::SnapshotHeader* snapshot(bool* fresh = nullptr);
inline const rfgphysx::BodyPose* poses(const rfgphysx::SnapshotHeader* s) {
    return reinterpret_cast<const rfgphysx::BodyPose*>(s + 1);
}

enum class State : uint8_t { Off = 0, Starting = 1, Ready = 2, Failed = 3, Lost = 4 };

struct FrameStats {
    State state;
    uint8_t gpu;
    uint32_t sequence;     // newest snapshot taken
    uint32_t snapshots;    // snapshots taken since the previous call
    float age_ms;          // how old the newest snapshot was when the game took it
    float step_ms;         // its PhysX step time
    float work_ms;         // the host's work for that step (commands, step, snapshot)
    uint32_t bodies, awake, frozen;
    float take_us;         // taking snapshots, since the previous call
    uint32_t cmd_bytes;    // command bytes written since the previous call
    uint32_t cmd_dropped;  // commands dropped since the previous call (ring full)
};
FrameStats take_frame();  // telemetry, simulation thread

}  // namespace pxbridge
