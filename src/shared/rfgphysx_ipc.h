#pragma once
// Shared-memory protocol between rfgphys.asi (32-bit, inside rfg.exe) and rfgphysx_host.exe (64-bit, PhysX).
// research/docs/06 §3 and §13. Both sides compile this header, so every structure uses fixed-size types only and its
// layout is pinned by the static_asserts at the end.
//
// The game creates one named file mapping and starts the host with its name:
//
//   [Control][command ring][query results][snapshot 0][snapshot 1][snapshot 2][event ring]
//
// - Control: start-up handshake, heartbeats, the ring's counters and the snapshot exchange (32-bit atomics only, so
//   neither side ever reads a torn value).
// - Command ring (game -> host): one producer (the game's simulation thread), one consumer (the host loop).
//   Variable-size records, 8-byte aligned, never split at the end of the ring (a Pad record fills the rest).
// - Snapshots (host -> game): a triple buffer. After each PhysX step the host writes a full snapshot into its back
//   buffer and publishes it; the game takes the newest one whenever it wants, without waiting. Each body record
//   carries the pose of this step and of the step before, so the game interpolates from one snapshot alone.
// - Query results (host -> game): the answer to the last RaycastBatch command, one distance per ray, then for each
//   ray what it hit (PxGeometryType + 1; 0 nothing).
// - Event ring (host -> game): fixed-size ContactEvent records, one producer (the host), one consumer (the game's
//   simulation thread); a full ring drops the newest. The loose chunks' impacts (research/docs/08, C2).
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rfgphysx {

constexpr uint32_t kMagic = 0x58505246;  // "FRPX"
constexpr uint32_t kVersion = 22;  // 22: Control::chunk_rest_strength; 21: a support in the event ring is the box of what the chunk bears on; 20: ShapePartPlaced::break_strength, supports in the event ring; 19: ContactEvent::lost; 18: DestroyBody::quiet; 17: CreateBody::made_qpc; 16: cpu_threads, change_every; 15: loose chunks' parts break in the host (break_energy, break_force_kn; ContactEvent: a part that broke); 13: RemoveParts; 12: ContactEvent energy and velocity; 11: the event ring; 10: ShapePartPlaced, kBodyChunk; 9: StaticConvexPlaced
constexpr uint32_t kMaxRays = 4096;  // per RaycastBatch

enum class HostState : uint32_t {
    Starting = 0,  // process started, PhysX not ready yet
    Ready = 1,     // stepping; commands are applied
    Failed = 2,    // gave up (message in Control::host_message); the process exits
    Exited = 3,    // left normally (quit requested or the game is gone)
};

enum Backend : uint32_t {
    kBackendGpu = 1,  // GPU dynamics and broadphase; CPU if CUDA is unavailable
    kBackendCpu = 2,
};

struct Vec3 {
    float x, y, z;
};
struct Quat {
    float x, y, z, w;
};

struct alignas(64) Control {
    // Written once by the game before the host starts.
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t ring_offset;
    uint32_t ring_size;        // bytes, a power of two
    uint32_t snapshot_offset;  // first snapshot; the others follow at snapshot_stride
    uint32_t snapshot_stride;  // bytes per snapshot (header + max_bodies records)
    uint32_t max_bodies;       // body slots 0 .. max_bodies-1
    uint32_t game_pid;
    uint32_t backend;          // Backend
    float step_hz;             // fixed step rate
    uint32_t query_offset;     // kMaxRays floats: the distances of the last RaycastBatch
    uint32_t event_offset;     // the event ring: event_count ContactEvent records
    uint32_t event_count;      // a power of two
    uint32_t change_every;     // bodies freeze, thaw, come and go every so many steps; 0 or 1: every step
    uint32_t cpu_threads;      // PhysX's CPU threads; 0: the host's default

    // Host -> game.
    std::atomic<uint32_t> host_state;      // HostState; host_message is written before Ready or Failed
    std::atomic<uint32_t> host_heartbeat;  // +1 every host loop (about every millisecond)
    std::atomic<uint32_t> host_pid;
    std::atomic<uint32_t> host_gpu;        // 1 when GPU dynamics are on
    std::atomic<uint32_t> snapshot_latest; // index of the newest published snapshot | kFresh while the game has not taken it
    std::atomic<uint32_t> query_done;      // request id of the last RaycastBatch answered
    std::atomic<uint32_t> statics;         // static actors in the scene (the mirrored world)
    std::atomic<uint32_t> event_head;      // events written (only grows; the record is event_head & (count-1))
    uint32_t reserved1[8];
    char host_message[256];

    // Game -> host.
    std::atomic<uint32_t> game_heartbeat;  // +1 every game frame
    std::atomic<uint32_t> quit;            // 1: the host exits
    std::atomic<uint32_t> test_flags;      // TestFlags: tests only, 0 in play (a host before them reads nothing here)
    std::atomic<uint32_t> event_tail;      // events read by the game
    float chunk_rest_strength;             // set before the host starts: a chunk's parts under a load lying still, times their strength when hit
    uint32_t chunk_crush_at_contact;       // set before the host starts: what crushing takes comes off where the parts broke; 0: off the whole chunk
    uint32_t chunk_wake_ahead;             // set before the host starts: a chunk wakes only the rubble ahead of it and on it; 0: all round it
    uint32_t reserved2[9];

    // Command ring counters: byte positions that only grow (and wrap at 2^32); the ring offset is position & (size-1).
    alignas(64) std::atomic<uint32_t> ring_head;  // written by the game: end of the last complete record
    uint32_t reserved3[15];
    alignas(64) std::atomic<uint32_t> ring_tail;  // written by the host: end of the last record it applied
    uint32_t reserved4[15];
};

constexpr uint32_t kFresh = 0x80000000u;   // in snapshot_latest: published and not taken yet
constexpr uint32_t kIndexMask = 0x3u;

// Control::test_flags.
enum TestFlags : uint32_t {
    // Removed statics wake no frozen pieces (Test.StressPieces: buildings restored over their rubble came down on it
    // again and again, and woke all of it each time).
    kTestNoUnsupportedWakes = 1u << 0,
    // chunk_steps.csv next to the host's log: each loose chunk, each step.
    kTestChunkTrace = 1u << 1,
};

// ---- commands -----------------------------------------------------------------------------------------------------

enum class Cmd : uint16_t {
    Pad = 0,         // filler up to the end of the ring
    Reset,           // remove every body, static and shape (level change)
    AddStaticPlane,  // StaticPlane
    AddStaticBox,    // StaticBox
    ShapeBox,        // ShapeBox: a body shape, by id
    ShapeConvex,     // ShapeConvex + vertex_count Vec3: a convex hull (cooked by the host)
    CreateBody,      // CreateBody
    DestroyBody,     // DestroyBody
    Explosion,       // Explosion
    Frame,           // Frame: the game's frame counter and clock, for latency measurement
    TestCrash,       // the host ends itself abruptly (tests the game's side of a host crash)
    StaticMesh,        // StaticMesh + vertex_count Vec3 + 3 * triangle_count uint32: a static triangle mesh, world space
    StaticConvex,      // StaticConvex + vertex_count Vec3: a static convex piece, world space, under a key
    RemoveStatics,     // RemoveStatics: every static added under a group
    RaycastBatch,      // RaycastBatch + count Ray: answered in the query results (static geometry only)
    RemoveStaticKeys,  // RemoveStaticKeys + count uint32: the convex pieces of a group added under these keys
    StaticHeightField, // StaticHeightField + rows * columns int16 heights (row-major)
    ShapePart,         // ShapePart + vertex_count Vec3: one convex part of a body shape, in the body's frame
    KinematicTarget,   // KinematicTarget: where a kinematic body is by the next step
    Impulse,           // Impulse: a push on one body at a point (a hammer blow, a bullet)
    MeleeSweep,        // MeleeSweep: a melee blow on every body in a swept sphere
    StaticConvexPlaced,  // StaticConvexPlaced + vertex_count Vec3: a static convex piece in its own frame, placed
    ShapePartPlaced,     // ShapePartPlaced + vertex_count Vec3: one part of a body shape in its own frame, placed
    RemoveParts,         // RemoveParts + count uint32: parts a loose chunk's body has lost (ShapePartPlaced::part)
};

struct CmdHeader {
    uint16_t type;  // Cmd
    uint16_t reserved;
    uint32_t size;  // bytes including this header, a multiple of 8
};

struct StaticPlane {
    CmdHeader h;
    Vec3 normal;
    float distance;  // plane: dot(normal, p) + distance = 0
};

struct StaticBox {
    CmdHeader h;
    Vec3 pos;
    Quat rot;
    Vec3 half_extents;
};

struct ShapeBox {
    CmdHeader h;
    uint32_t shape;
    Vec3 half_extents;
};

struct ShapeConvex {
    CmdHeader h;
    uint32_t shape;
    uint32_t vertex_count;  // Vec3 vertices follow (at most 255)
};

// A body shape made of several convex parts (a debris piece with more than one hull). Part 0 starts the shape over;
// bodies created with it get every part sent so far.
struct ShapePart {
    CmdHeader h;
    uint32_t shape;
    uint32_t part;
    uint32_t vertex_count;  // Vec3 vertices follow (at least 3, at most 255)
    uint32_t flags;         // ShapeFlags (the shape's first part decides)
    float radius;           // Havok's convex radius: the part is that much larger all round than its vertices
    uint32_t pad;
};

// ShapePart with the points in the part's own frame, placed in the body's frame at pos, rot: a loose chunk's parts
// are building pieces whose hulls the host cooked for the static world already, and it finds them by their points.
struct ShapePartPlaced {
    CmdHeader h;
    uint32_t shape;
    uint32_t part;
    uint32_t vertex_count;  // Vec3 vertices follow (at least 3, at most 255)
    uint32_t flags;         // ShapeFlags (the shape's first part decides)
    float radius;
    Vec3 pos;
    Quat rot;
    float break_strength;   // a loose chunk's part: its share of the chunk's break force, from its material
    float break_energy;     // J: a loose chunk's part: what breaking off takes from the chunk (0: it does not break)
};

// What a body shape is, for contact filtering: the player's capsule does not touch heavy pieces (the player stands
// on those through their Havok stand-ins in the game).
enum ShapeFlags : uint32_t {
    kShapeHeavy = 1u << 0,
    kShapePlayer = 1u << 1,
    kShapeChunk = 1u << 2,  // a loose chunk's part: its contacts are reported (ContactEvent)
    kShapeGone = 1u << 3,   // host only: it broke off and touches nothing any more, until it is taken off its body
};

enum BodyFlags : uint32_t {
    kBodyKinematic = 1u << 0,
    kBodyMass = 1u << 1,    // CreateBody::density holds the body's mass in kg (inertia from its shape)
    kBodyPusher = 1u << 2,  // kinematic, standing for a heavy game body (a vehicle's): wakes every frozen piece it
                            // is about to reach, whatever its mass
    kBodyAtRest = 1u << 3,  // it lay still in Havok: it starts frozen, as a piece that came to rest in the host
    kBodyGentle = 1u << 4,  // it waited in Havok: what it overlaps when it comes over is pushed apart slowly
    kBodyChunk = 1u << 5,   // a loose building chunk simulated here (research/docs/08): dynamic, its game body
                            // follows it; like a pusher it wakes the frozen pieces it is about to reach, but with
                            // no cap on its contacts (it lands on them)
};

struct CreateBody {
    CmdHeader h;
    uint32_t slot;
    uint32_t shape;
    Vec3 pos;
    Quat rot;
    Vec3 lin_vel;
    Vec3 ang_vel;
    float density;  // kg/m^3
    uint32_t flags; // BodyFlags
    float break_force_kn;  // a loose chunk: the kN on a part that break it off (0: none do)
    int64_t made_qpc;      // when the game made it: a loose chunk starts where it would be by then
};

struct DestroyBody {
    CmdHeader h;
    uint32_t slot;
    uint32_t quiet;  // 1: what rests on it stays frozen (a chunk parked: it still lies there in the game)
};

// A loose chunk's body loses parts where it stands (research/docs/08, C2c): the game broke them off (crushed, shot,
// split into a chunk of their own). What rested on them wakes.
struct RemoveParts {
    CmdHeader h;
    uint32_t slot;
    uint32_t count;  // uint32_t parts follow
    float mass;      // kg: the body's mass now (0: as it is)
    uint32_t pad;
};

enum ExplosionMode : uint32_t {
    kExplosionImpulse = 0,         // `impulse` in N*s
    kExplosionVelocityChange = 1,  // `impulse` in m/s, whatever the mass
    kImpulseAtCentre = 2,          // Impulse only: `impulse` in N*s through the body's centre of mass (no point): the
                                   // game's own pushes on a loose chunk (research/docs/08), as Havok would apply them
};

struct Explosion {
    CmdHeader h;
    Vec3 center;
    float radius;
    float impulse;  // at the center, falling off linearly to 0 at the radius
    float up;       // extra upward share of the push direction
    uint32_t mode;  // ExplosionMode
    uint32_t pad;
};

enum TargetFlags : uint32_t {
    kTargetTeleport = 1u << 0,  // there at once, not swept through what lies between (a respawn, a seat change)
};

struct KinematicTarget {
    CmdHeader h;
    uint32_t slot;
    uint32_t flags;  // TargetFlags
    Vec3 pos;
    float pad2;
    Quat rot;
};

// A push on one body at a world-space point. A frozen body moves again.
struct Impulse {
    CmdHeader h;
    uint32_t slot;
    uint32_t mode;  // ExplosionMode: `impulse` in N*s, or in m/s whatever the mass
    Vec3 impulse;
    float pad;
    Vec3 point;
    float pad2;
};

// A stretch of a melee blow whose sphere sweeps from `start` to `end`: every body within `radius` of that segment gets
// `impulse` (N*s, the one the game gives each object the blow hits), its speed change capped at `max_speed`, once per
// blow however many stretches reach it. Frozen bodies move again.
struct MeleeSweep {
    CmdHeader h;
    Vec3 start;
    float radius;
    Vec3 end;
    float max_speed;  // m/s
    Vec3 impulse;
    uint32_t blow;    // the blow it belongs to (nonzero)
};

struct Frame {
    CmdHeader h;
    uint32_t frame;
    uint32_t pad;
    int64_t qpc;  // QueryPerformanceCounter when the frame ended (the same clock in both processes)
};

// The static world mirror (research/docs/06 §5): every static is added under a group (one per Havok body). Convex
// pieces also carry a key (the Havok shape key of the piece within its body), so a building that loses pieces loses
// just those (RemoveStaticKeys); a whole body goes with RemoveStatics.
struct StaticMesh {
    CmdHeader h;
    uint32_t group;
    uint32_t vertex_count;
    uint32_t triangle_count;
    uint32_t pad;
};

struct StaticConvex {
    CmdHeader h;
    uint32_t group;
    uint32_t vertex_count;  // at least 3, at most 255
    uint32_t key;
    float radius;           // as ShapePart's
};

// StaticConvex with the points in the piece's own frame (world = rot * local + pos). Pieces alike come with the same
// points to the bit wherever they stand, so the host cooks them once (research/docs/07, B5b: a building's modules
// repeat on every storey and each avenue type stands four times; 390,000 hulls cooked one by one took 33-43 s).
struct StaticConvexPlaced {
    CmdHeader h;
    uint32_t group;
    uint32_t vertex_count;  // at least 3, at most 255
    uint32_t key;
    float radius;           // as ShapePart's
    Vec3 pos;
    Quat rot;
    uint32_t pad;
};

struct RemoveStaticKeys {
    CmdHeader h;
    uint32_t group;
    uint32_t count;  // uint32 keys follow
};

// A height field: vertex (row, column) is at origin + rot * (row * row_scale, height * height_scale,
// column * column_scale), with heights as int16 samples (row-major). Each cell is split along the diagonal from
// (row, column) to (row + 1, column + 1), or along the other one with `flip`.
struct StaticHeightField {
    CmdHeader h;
    uint32_t group;
    uint32_t rows;
    uint32_t columns;
    uint32_t flip;
    Vec3 origin;
    float row_scale;
    Quat rot;
    float column_scale;
    float height_scale;
};

struct RemoveStatics {
    CmdHeader h;
    uint32_t group;
    uint32_t pad;
};

struct Ray {
    Vec3 origin;
    float max_distance;
    Vec3 direction;  // unit length
    uint32_t pad;
};

struct RaycastBatch {
    CmdHeader h;
    uint32_t request;  // echoed in Control::query_done once the distances are written
    uint32_t count;    // at most kMaxRays
};

// ---- snapshots ----------------------------------------------------------------------------------------------------

struct SnapshotHeader {
    uint32_t sequence;     // 1, 2, 3, ... per published snapshot
    uint32_t step;         // PhysX steps taken
    double sim_time;       // seconds of simulated time
    int64_t publish_qpc;   // QueryPerformanceCounter when published
    int64_t step_qpc;      // QueryPerformanceCounter when the step began
    float step_ms;         // simulate + fetchResults
    float work_ms;         // commands + step + snapshot for this step
    uint32_t bodies;       // live bodies
    uint32_t awake;        // bodies PhysX moved in this step
    uint32_t slots;        // records valid in this snapshot: 0 .. slots-1
    uint32_t last_frame;   // the last Frame command applied before this step
    uint32_t commands;     // commands applied since the previous snapshot
    uint32_t frozen;       // bodies frozen (calm long enough to be made kinematic until something disturbs them)
};

enum PoseFlags : uint32_t {
    kPoseAlive = 1u << 0,
    kPoseAwake = 1u << 1,
    kPoseFrozen = 1u << 2,
};

struct BodyPose {  // 64 bytes
    Vec3 pos;
    uint32_t flags;  // PoseFlags
    Quat rot;
    Vec3 prev_pos;   // the pose one step earlier: the game interpolates between the two
    uint32_t pad;
    Quat prev_rot;
};

// A snapshot: the header, max_bodies poses, then each slot's flags again as one byte (PoseFlags, the low 8 bits), so
// the game finds the bodies that move or changed in 64 KB instead of reading every pose (4 MB at 65,536 slots).
constexpr uint32_t snapshot_stride(uint32_t max_bodies) {
    return uint32_t((sizeof(SnapshotHeader) + size_t(max_bodies) * (sizeof(BodyPose) + 1) + 63) & ~size_t(63));
}
inline const uint8_t* snapshot_states(const SnapshotHeader* s, uint32_t max_bodies) {
    return reinterpret_cast<const uint8_t*>(s + 1) + size_t(max_bodies) * sizeof(BodyPose);
}
inline uint8_t* snapshot_states(SnapshotHeader* s, uint32_t max_bodies) {
    return reinterpret_cast<uint8_t*>(s + 1) + size_t(max_bodies) * sizeof(BodyPose);
}

// ---- events (host -> game) -----------------------------------------------------------------------------------------

// A part of a loose chunk that broke off in the host under its load (research/docs/08 §12): the game breaks it too.
// What pressed on it is what carried most of that load.
constexpr uint32_t kEventStatic = 0x80000000u;  // ContactEvent::other: a static group | this
constexpr uint32_t kEventNone = 0xFFFFFFFFu;    // ContactEvent::other: something else (a height field, a mesh)
struct ContactEvent {
    uint32_t slot;        // the chunk's body slot
    uint32_t part;        // its part (ShapePartPlaced::part); kEventNone: no part broke, where the chunk bears on what holds it, `total` N
    uint32_t other;       // a body slot, kEventStatic | a static group, or kEventNone
    uint32_t other_key;   // a static piece's key (StaticConvexPlaced::key); a chunk's part; else kEventNone
    Vec3 point;           // world; part kEventNone: unused
    float total;          // J: on the part that carried the most in its step, the chunk's kinetic energy before it; else 0
    Vec3 normal;          // world, toward the chunk; part kEventNone: the box of what it bears on, its low corner, in the chunk's frame
    float energy;         // J: what breaking this part took
    Vec3 velocity;        // m/s: the chunk's (its centre of mass) before this step; part kEventNone: that box's high corner
    uint32_t step;        // the host's step
    float lost;           // J: on the first of a chunk's parts in a step, the energy it lost in that step, what cracks it
    uint32_t under;       // what it pressed on through rubble or another chunk: kEventStatic | a static group, or kEventNone
    uint32_t under_key;   // and that static piece's key
    uint32_t reserved;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>) == 4);
static_assert(sizeof(Control) == 576 && offsetof(Control, host_state) == 64 && offsetof(Control, query_done) == 84 &&
                  offsetof(Control, host_message) == 128 &&
                  offsetof(Control, game_heartbeat) == 384 && offsetof(Control, test_flags) == 392 &&
                  offsetof(Control, ring_head) == 448 &&
                  offsetof(Control, ring_tail) == 512,
              "Control layout differs between the 32- and 64-bit builds");
static_assert(sizeof(CmdHeader) == 8 && sizeof(StaticPlane) % 8 == 0 && sizeof(StaticBox) % 8 == 0 &&
              sizeof(ShapeBox) % 8 == 0 && sizeof(ShapeConvex) % 8 == 0 && sizeof(CreateBody) % 8 == 0 &&
              sizeof(DestroyBody) % 8 == 0 && sizeof(Explosion) % 8 == 0 && sizeof(Frame) % 8 == 0 &&
              sizeof(StaticMesh) == 24 && sizeof(StaticConvex) == 24 && sizeof(RemoveStatics) == 16 &&
              sizeof(Ray) == 32 && sizeof(RaycastBatch) == 16 && sizeof(RemoveStaticKeys) == 16 &&
              sizeof(StaticHeightField) == 64 && sizeof(ShapePart) == 32 && sizeof(Explosion) == 40 &&
              sizeof(KinematicTarget) == 48 && sizeof(Impulse) == 48 && sizeof(MeleeSweep) == 56);
static_assert(sizeof(CreateBody) == 88 && offsetof(CreateBody, made_qpc) == 80 && sizeof(Frame) == 24 && offsetof(Frame, qpc) == 16);
static_assert(sizeof(SnapshotHeader) == 64 && offsetof(SnapshotHeader, sim_time) == 8 && sizeof(BodyPose) == 64);
static_assert(sizeof(ContactEvent) == 80 && offsetof(Control, event_head) == 92 && offsetof(Control, event_tail) == 396,
              "event layout differs between the 32- and 64-bit builds");

}  // namespace rfgphysx
