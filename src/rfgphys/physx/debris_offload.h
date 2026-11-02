#pragma once
#include <cstdint>
#include <unordered_set>
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
// Where the level's debris lies (Test.Viewpoints, simulation thread): the live pieces' mean position, the horizontal
// distance from it within which 80% of them lie, and the height of the lowest tenth. False without pieces.
bool debris_extent(float centre[3], float& radius, float& low_y, uint32_t& pieces);
// Test.Viewpoints 2-4 (what the rest of the scene costs): from the next sim/render handshake, the debris (1: all,
// 2: the piles, 3: the pieces of their own) is drawn 10 km below where it lies, and nothing moves any more.
void hide_debris_for_test(int what);
// Test.StressPieces: off, explosions no longer kick the pieces in PhysX, so the rubble of the cycles before stays at
// rest while the same buildings come down again (on by default).
void kick_pieces_for_test(bool on);
// Test.StressPieces (simulation thread): the pieces the host simulates and the slots there are for them, the piles
// and the pieces in them, the still pieces that waited for a pile at the last scan and the still pieces of their own
// looked over for piles; since the level's start, pieces that joined and left piles and pieces given up far away for
// render instances, and the render task's frames and its time in moves, splits out of piles, piles and the rest.
struct Census {
    uint32_t live, slots, piles, piled, waiting, candidates;
    uint64_t joined, unpiled, recycled, task_frames;
    double task_ms[4];
};
Census census();
// Experiment.PhysXChunks (research/docs/08; Test.ChunkFall, simulation thread): the loose chunks the host simulates
// now, their parts and mass, the fastest of them (PhysX's last step), how far a chunk's Havok body was from where it
// was sent at most since the last call, and the loose chunks within reach left in Havok at the last scan; the chunks
// driven since the level's start.
struct ChunkStats {
    uint32_t driven, parts;
    float mass_kg, fastest, follow_max;
    uint32_t in_havok, made, too_large;
    uint32_t impacts, impacts_weak;  // since the level's start: passed to the game's destruction, too weak for it
    float energy_max_kj;
    uint32_t crushed, parts_removed;  // since the level's start: subpieces broken off by impacts, parts taken off bodies
    float crushed_t;
    uint32_t pieces_at_once;          // since the start: pieces that broke off them and went over at once
    float largest_t[6];               // the heaviest of them now
    uint32_t cracks, snaps;           // since the level's start
    float stump_t;                    // of buildings, crushed under them
};
ChunkStats chunk_stats();
// chunk_trace.csv and the host's chunk_steps.csv from now on: each loose chunk, each frame and step (Test.ChunkFall).
void trace_chunks_from_now_on();
// Test.ChunkFall: t of a building still in its own shape, and what these buildings are made of, in the log. And breaks up to
// `room` of a building's subpieces with their centres in a world box, handing them to the game's destruction as if crushed;
// how many: 0 when none are left in it.
float standing_t(uint32_t object_handle);
void log_materials_of(uint8_t* const* objects, size_t n);
uint32_t crush_in_box(uint32_t object_handle, const float lo[3], const float hi[3], uint32_t room);

// Simulation thread: these objects were restored (destroyed and streamed in again): the pieces that broke off them go
// at the next sim/render handshake. And how many live pieces broke off one (tests).
void forget_pieces_of(const std::unordered_set<uint32_t>& object_handles);
uint32_t count_pieces_of(uint32_t object_handle);
// Diagnostics: the object a live piece broke off (0xFFFFFFFF: none, or no piece), and the objects of the last
// restoration.
uint32_t piece_original(uint32_t slot);
const std::unordered_set<uint32_t>& last_restored();

}  // namespace debris_offload
