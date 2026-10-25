#pragma once
// Shared-memory protocol between rfgphys.asi (32-bit, inside rfg.exe) and rfgphysx_host.exe (64-bit, PhysX).
// research/docs/06 §3 and §13. Both sides compile this header, so every structure uses fixed-size types only and its
// layout is pinned by the static_asserts at the end.
//
// The game creates one named file mapping and starts the host with its name:
//
//   [Control][command ring][query results][snapshot 0][snapshot 1][snapshot 2]
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
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rfgphysx {

constexpr uint32_t kMagic = 0x58505246;  // "FRPX"
constexpr uint32_t kVersion = 7;
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
    uint32_t reserved0[4];

    // Host -> game.
    std::atomic<uint32_t> host_state;      // HostState; host_message is written before Ready or Failed
    std::atomic<uint32_t> host_heartbeat;  // +1 every host loop (about every millisecond)
    std::atomic<uint32_t> host_pid;
    std::atomic<uint32_t> host_gpu;        // 1 when GPU dynamics are on
    std::atomic<uint32_t> snapshot_latest; // index of the newest published snapshot | kFresh while the game has not taken it
    std::atomic<uint32_t> query_done;      // request id of the last RaycastBatch answered
    std::atomic<uint32_t> statics;         // static actors in the scene (the mirrored world)
    uint32_t reserved1[9];
    char host_message[256];

    // Game -> host.
    std::atomic<uint32_t> game_heartbeat;  // +1 every game frame
    std::atomic<uint32_t> quit;            // 1: the host exits
    uint32_t reserved2[14];

    // Command ring counters: byte positions that only grow (and wrap at 2^32); the ring offset is position & (size-1).
    alignas(64) std::atomic<uint32_t> ring_head;  // written by the game: end of the last complete record
    uint32_t reserved3[15];
    alignas(64) std::atomic<uint32_t> ring_tail;  // written by the host: end of the last record it applied
    uint32_t reserved4[15];
};

constexpr uint32_t kFresh = 0x80000000u;   // in snapshot_latest: published and not taken yet
constexpr uint32_t kIndexMask = 0x3u;

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

// What a body shape is, for contact filtering: the player's capsule does not touch heavy pieces (the player stands
// on those through their Havok stand-ins in the game).
enum ShapeFlags : uint32_t {
    kShapeHeavy = 1u << 0,
    kShapePlayer = 1u << 1,
};

enum BodyFlags : uint32_t {
    kBodyKinematic = 1u << 0,
    kBodyMass = 1u << 1,    // CreateBody::density holds the body's mass in kg (inertia from its shape)
    kBodyPusher = 1u << 2,  // kinematic, standing for a heavy game body (a vehicle's): wakes every frozen piece it
                            // is about to reach, whatever its mass
    kBodyAtRest = 1u << 3,  // it lay still in Havok: it starts frozen, as a piece that came to rest in the host
    kBodyGentle = 1u << 4,  // it waited in Havok: what it overlaps when it comes over is pushed apart slowly
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
    uint32_t pad;
};

struct DestroyBody {
    CmdHeader h;
    uint32_t slot;
    uint32_t pad;
};

enum ExplosionMode : uint32_t {
    kExplosionImpulse = 0,         // `impulse` in N*s
    kExplosionVelocityChange = 1,  // `impulse` in m/s, whatever the mass
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

constexpr uint32_t snapshot_stride(uint32_t max_bodies) {
    return uint32_t((sizeof(SnapshotHeader) + size_t(max_bodies) * sizeof(BodyPose) + 63) & ~size_t(63));
}

static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>) == 4);
static_assert(sizeof(Control) == 576 && offsetof(Control, host_state) == 64 && offsetof(Control, query_done) == 84 &&
                  offsetof(Control, host_message) == 128 &&
                  offsetof(Control, game_heartbeat) == 384 && offsetof(Control, ring_head) == 448 &&
                  offsetof(Control, ring_tail) == 512,
              "Control layout differs between the 32- and 64-bit builds");
static_assert(sizeof(CmdHeader) == 8 && sizeof(StaticPlane) % 8 == 0 && sizeof(StaticBox) % 8 == 0 &&
              sizeof(ShapeBox) % 8 == 0 && sizeof(ShapeConvex) % 8 == 0 && sizeof(CreateBody) % 8 == 0 &&
              sizeof(DestroyBody) % 8 == 0 && sizeof(Explosion) % 8 == 0 && sizeof(Frame) % 8 == 0 &&
              sizeof(StaticMesh) == 24 && sizeof(StaticConvex) == 24 && sizeof(RemoveStatics) == 16 &&
              sizeof(Ray) == 32 && sizeof(RaycastBatch) == 16 && sizeof(RemoveStaticKeys) == 16 &&
              sizeof(StaticHeightField) == 64 && sizeof(ShapePart) == 32 && sizeof(Explosion) == 40 &&
              sizeof(KinematicTarget) == 48 && sizeof(Impulse) == 48 && sizeof(MeleeSweep) == 56);
static_assert(sizeof(CreateBody) == 80 && sizeof(Frame) == 24 && offsetof(Frame, qpc) == 16);
static_assert(sizeof(SnapshotHeader) == 64 && offsetof(SnapshotHeader, sim_time) == 8 && sizeof(BodyPose) == 64);

}  // namespace rfgphysx
