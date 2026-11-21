#include "physx/world_mirror.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <random>
#include <share.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/layout.h"  // OpenRFG64: layouts by name
#include "game/rfg.h"
#include "physx/bridge.h"
#include "physx/havok_shapes.h"

namespace world_mirror {
namespace {

namespace hs = havok_shapes;
namespace ipc = rfgphysx;

constexpr int kGameplay = 1;                      // GS_GAMEPLAY
constexpr double kScanEveryMs = 250;
constexpr double kScanBudgetMs = 20;              // export work per scan; the rest waits for the next one
// Commands waiting for room in the ring, at most, before a scan exports more bodies; the rest wait for later scans.
// Without it the avenue's export queued 54 MB at once, in 390,000 records of their own, as the level loaded: memory
// the game, 32-bit, was short of then (a crash in this queue, and the game's own "out of memory" at the avenue's load
// with more body slots, 2026-11-07/09). The host takes the commands no faster than it cooks them anyway.
constexpr size_t kOutboxCap = size_t(6) << 20;
constexpr uint32_t kChunkTriangles = 16384;       // triangles per StaticMesh record
const uint32_t& kDestroyableExisting = layout::destroyable_existing_children;  // rfg_destroyable_shape::num_existing_child_shapes

bool g_on = false, g_check = false;
int g_hf_variant = 0;  // Experiment.PhysXWorldHf (diagnostic): bit 0 x-major heights, bit 1 the other cell split

// OpenRFG64: the area mode (big maps; world_mirror.h). Ten avenues of tall buildings make 4.1 million statics: the
// host took about 22,000 a second, at half speed, and its GPU broadphase reached 1.7 GB by a twelfth of them; the
// debris waited for the whole export and never came over (2026-11-20). Mirroring whole buildings wherever debris lay
// still came to a million statics, and the host's steps to 22 ms with 4,700 pieces (2026-11-21): a building far
// from the player sends only its pieces near the debris.
uint32_t g_budget = 0;                  // Experiment.PhysXWorldStatics; 0: the whole static world, as in rfg.exe
float g_near_m = 60.0f;                 // Experiment.PhysXWorldNearM
constexpr double kHoldMs = 10000;       // a body no longer wanted stays in the host this long
constexpr float kKeyCellM = 8.0f;       // the grid where pieces in PhysX, areas asked for and buildings' pieces meet
constexpr float kKeyMarginM = 4.0f;     // a building's piece counts for the cells within this of it
constexpr float kAskCellM = 16.0f;      // ready_at answers per cell of this size (and radius)
constexpr double kPiecesEveryMs = 1000; // the pieces in PhysX are counted again
constexpr double kRequestMs = 5000;     // an area asked for stays wanted this long after it was last asked for
constexpr double kBoundsBudgetMs = 30;  // measuring new bodies, per scan
bool area() { return g_budget != 0; }

// Only what debris collides with is mirrored: the body's layer must collide with a debris layer in the game's layer
// table (havok_game_filter: bit a * 128 + b), and system group 6 never collides with debris (01 §10.3). That leaves
// out triggers, invisible barriers and the like, which Havok's own debris never meets either.
const uint32_t& kFilterInfo = layout::hk_entity_filter_info;     // hkpEntity: m_collidable.m_broadPhaseHandle.m_collisionFilterInfo
const uint32_t& kLayerTable = layout::game_filter_layer_table;   // havok_game_filter::m_layer_lookup_table

bool collides_with_debris(const uint8_t* entity) {
    const uint32_t info = *reinterpret_cast<const uint32_t*>(entity + kFilterInfo);
    const uint32_t layer = info & 0x7F, group = info >> 16;
    if (group == 6) return false;
    const uint8_t* filter = rfg::global<uint8_t*>(rfg::rva::Havok_filter);
    if (!filter) return true;
    const uint32_t* table = *reinterpret_cast<const uint32_t* const*>(filter + kLayerTable);
    if (!table) return true;
    for (uint32_t debris = 41; debris <= 44; ++debris) {
        const uint32_t bit = layer * 128 + debris;
        if (table[bit >> 5] & (1u << (bit & 31))) return true;
    }
    return false;
}

// Bodies are found by address, and an address comes back: Havok's heap hands a freed entity's memory to the next one,
// and when a zone streams out and another in between two scans, new ground took the place of the old in the map,
// never reached the host, and the old stayed there. A long session in the open world gathered such holes until
// pieces fell through the ground everywhere (players: "fixed by loading the game"). hkpEntity::m_uid is new for every
// entity the world adds.
const uint32_t& kEntityUid = layout::hk_entity_uid;  // hkpEntity::m_uid
struct Mirrored {
    uint32_t group;                // its statics' group in the host; area mode: 0 while it is not there
    uint32_t signature;            // destroyables: 1 + existing pieces; 0: never changes
    bool seen;
    std::vector<uint32_t> keys;    // destroyables: the piece keys mirrored, sorted
    uint32_t uid;                  // hkpEntity::m_uid of the body mirrored
    uint32_t object = 0xFFFFFFFFu; // its game object's handle (object_of_group)
    uint32_t shapes = 0;           // the statics it makes in the host (when exported; area mode: when measured)
    // Area mode:
    bool bounded = false;          // lo, hi and shapes are known
    bool wanted = false;           // at the last scan
    bool stale = false;            // in the host, but pieces it lost or got back are not yet on their way there
    float lo[3] = {}, hi[3] = {};  // its box in the world
    uint32_t pending = 0;          // its records still waiting in an outbox
    uint32_t flushed_frame = 0;    // pxbridge::frame() when the last of them went into the ring
    double wanted_ms = 0;          // when it was last wanted
    // A building far from the player, wanted for the debris near it and the areas asked for: only its pieces near
    // those (cells of kKeyCellM) are in the host.
    bool partial = false;
    bool cells_built = false;
    std::unordered_map<uint64_t, std::vector<uint32_t>> key_cells;  // cell -> its pieces that count for it
    std::unordered_set<uint64_t> covered;    // cells whose pieces are wanted (target)
    std::unordered_set<uint32_t> target;     // the pieces wanted in the host
    std::unordered_set<uint64_t> asks_done;  // the areas asked for whose cells are covered (their ready_at keys)
    std::vector<uint64_t> asks;              // the areas asked for that reach it, at the last scan
    uint32_t stamp = 0;                      // the last tile query that met it
};
std::unordered_map<const uint8_t*, Mirrored> g_bodies;  // Havok entity -> its mirror (area mode: every body)
std::unordered_map<uint32_t, const uint8_t*> g_entity_of_group;  // the bodies in the host, by group
uint32_t g_next_group = 1;
// The commands for the host wait here for room in the ring. Area mode: changes to the bodies in the host and their
// removals go ahead of the exports (g_urgent), which can take seconds on a big map; a body's own records keep their
// order (a change or removal goes urgent only once its earlier records are in the ring).
struct Out {
    std::vector<uint8_t> rec;
    uint32_t group;  // area mode: the body whose records are counted (Mirrored::pending); 0: none
};
std::deque<Out> g_outbox;
std::deque<Out> g_urgent;
Mirrored* g_queue_body = nullptr;  // area mode: the body the records queued now are counted for
bool g_queue_urgent = false;
size_t g_outbox_bytes = 0;
size_t g_outbox_peak = 0;         // the most waiting at once, since the level's mirror began
uint64_t g_export_bytes = 0;      // queued since then
double g_next_scan = 0;
double g_next_change_log = 0;
uint32_t g_log_added = 0, g_log_changed = 0, g_log_removed = 0, g_log_reused = 0, g_log_dropped = 0;
uint64_t g_reused_total = 0;
bool g_initial_done = false;                      // the first full export has been queued
uint32_t g_complete_scans = 0, g_flushed_scans = 0;  // scans that looked at every body; those all in the ring
bool g_host_has_world = false;                    // and the host has applied it (area mode: around the player)
double g_export_done_ms = 0;
uint32_t g_unsupported[hs::kTypeCount + 1] = {};
FrameStats g_stats{};
// Area mode, per level:
double g_level_ms = 0;              // the level's first scan
uint32_t g_unbounded = 0;           // bodies not measured yet
bool g_measured_logged = false;
uint32_t g_mirrored = 0;            // bodies in the host, at the last scan
uint64_t g_mirrored_statics = 0;    // and their statics
uint32_t g_missing = 0;             // wanted bodies not in the host yet, at the last scan
double g_next_pieces_ms = 0;
uint32_t g_pieces_counted = 0;
bool g_level_whole = false;         // the level's static world is within the budget: all of it goes
uint32_t g_partial = 0;             // bodies in the host in part, at the last scan
uint32_t g_log_grown = 0;
struct Request {
    float x0, z0, x1, z1, y1;  // the area: from below everything up to y1
    double until;
};
std::unordered_map<uint64_t, Request> g_requests;  // areas asked for (ready_at), by their key
std::unordered_map<uint64_t, bool> g_ready_cache;  // ready_at's answers this frame, by key
uint32_t g_ready_cache_frame = 0;
uint64_t g_asked = 0, g_asked_logged = 0;          // ready_at calls answered "not yet"
// The bodies by tile (x and z, kTileM) their box (widened by kKeyMarginM) reaches: what an area asked for meets is
// found among a few dozen bodies instead of all of them (ten avenues: 544; ready_at asked thousands of times a frame).
constexpr float kTileM = 64.0f;
std::unordered_map<uint64_t, std::vector<std::pair<const uint8_t*, Mirrored*>>> g_tiles;
bool g_tiles_dirty = true;
uint32_t g_stamp = 0;
double g_log_scan_ms = 0, g_log_ask_ms = 0;  // time in scans and in ready_at, since the last 5 s line
uint64_t g_log_asks = 0, g_log_misses = 0;   // ready_at calls, and those not answered from this frame's cache

void queue(std::vector<uint8_t>&& record) {
    g_outbox_bytes += record.size();
    g_export_bytes += record.size();
    g_outbox_peak = std::max(g_outbox_peak, g_outbox_bytes);
    uint32_t group = 0;
    if (g_queue_body) {
        group = g_queue_body->group;
        ++g_queue_body->pending;
    }
    (g_queue_urgent ? g_urgent : g_outbox).push_back({std::move(record), group});
}

template <typename T>
std::vector<uint8_t> record(ipc::Cmd type, const T& fixed, size_t extra) {
    std::vector<uint8_t> buf((sizeof(T) + extra + 7) & ~size_t(7));
    memcpy(buf.data(), &fixed, sizeof(T));
    auto* h = reinterpret_cast<ipc::CmdHeader*>(buf.data());
    h->type = uint16_t(type);
    h->reserved = 0;
    h->size = uint32_t(buf.size());
    return buf;
}

void remove_group(uint32_t group) {
    ipc::RemoveStatics c{};
    c.group = group;
    queue(record(ipc::Cmd::RemoveStatics, c, 0));
}

void remove_keys(uint32_t group, const std::vector<uint32_t>& keys) {
    // Taking a shape off a static takes the host about 0.1 ms, so not many in one command: it steps between commands.
    for (size_t i = 0; i < keys.size(); i += 1024) {
        const uint32_t n = uint32_t(std::min<size_t>(1024, keys.size() - i));
        ipc::RemoveStaticKeys c{};
        c.group = group;
        c.count = n;
        auto buf = record(ipc::Cmd::RemoveStaticKeys, c, size_t(n) * 4);
        memcpy(buf.data() + sizeof(c), keys.data() + i, size_t(n) * 4);
        queue(std::move(buf));
    }
}

ipc::Quat quat_of(const float m[3][3]) {
    // Rotation matrix (rows) to quaternion; the scale-free part of Havok's transforms.
    ipc::Quat q;
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2.0f;
        q = {(m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25f * s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q = {0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q = {(m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s};
    }
    return q;
}

// A rotation (orthonormal, not mirrored) to within float rounding: no scale or shear for quat_of to lose.
bool rigid(const float m[3][3]) {
    for (int a = 0; a < 3; ++a)
        for (int b = a; b < 3; ++b) {
            const float d = m[0][a] * m[0][b] + m[1][a] * m[1][b] + m[2][a] * m[2][b];
            if (std::fabs(d - (a == b ? 1.0f : 0.0f)) > 1e-4f) return false;
        }
    const float det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                      m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                      m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    return det > 0.0f;
}

// Turns one Havok body into StaticMesh, StaticConvex and StaticHeightField records. With `only`, just the convex
// pieces under those keys (pieces added back to a building).
struct Export : hs::Sink {
    uint32_t group;
    const std::unordered_set<uint32_t>* only = nullptr;
    std::vector<uint32_t>* keys_out = nullptr;
    std::vector<ipc::Vec3> verts;
    std::vector<uint32_t> idx;
    std::unordered_map<uint64_t, uint32_t> lookup;
    uint32_t shapes = 0;  // statics made

    explicit Export(uint32_t g) : group(g) {}

    uint32_t vertex(const float p[3]) {
        // Shared vertices (to the millimetre) keep the cooked mesh connected, so contacts glide over its edges.
        auto q = [](float v) { return uint64_t(int64_t(std::lround(double(v) * 1000.0)) & 0x1FFFFF); };
        const uint64_t key = q(p[0]) | (q(p[1]) << 21) | (q(p[2]) << 42);
        auto [it, inserted] = lookup.try_emplace(key, uint32_t(verts.size()));
        if (inserted) verts.push_back({p[0], p[1], p[2]});
        return it->second;
    }

    void triangle(const float a[3], const float b[3], const float c[3]) override {
        if (only) return;
        idx.push_back(vertex(a));
        idx.push_back(vertex(b));
        idx.push_back(vertex(c));
        if (idx.size() / 3 >= kChunkTriangles) flush();
    }

    void flush() {
        if (idx.empty()) return;
        ipc::StaticMesh c{};
        c.group = group;
        c.vertex_count = uint32_t(verts.size());
        c.triangle_count = uint32_t(idx.size() / 3);
        auto buf = record(ipc::Cmd::StaticMesh, c, verts.size() * sizeof(ipc::Vec3) + idx.size() * 4);
        memcpy(buf.data() + sizeof(c), verts.data(), verts.size() * sizeof(ipc::Vec3));
        memcpy(buf.data() + sizeof(c) + verts.size() * sizeof(ipc::Vec3), idx.data(), idx.size() * 4);
        queue(std::move(buf));
        ++shapes;
        verts.clear();
        idx.clear();
        lookup.clear();
    }

    void convex(const float (*v)[3], int n, float radius, uint32_t key) override {
        if (n < 3 || (only && !only->count(key))) return;
        if (n > 255) n = 255;
        ipc::StaticConvex c{};
        c.group = group;
        c.vertex_count = uint32_t(n);
        c.key = key;
        c.radius = radius;
        auto buf = record(ipc::Cmd::StaticConvex, c, size_t(n) * sizeof(ipc::Vec3));
        memcpy(buf.data() + sizeof(c), v, size_t(n) * sizeof(ipc::Vec3));
        queue(std::move(buf));
        ++shapes;
        if (keys_out) keys_out->push_back(key);
    }

    // In the piece's own frame, so that the host cooks pieces alike once; scaled or mirrored ones in world space.
    void convex_placed(const float (*local)[3], int n, float radius, const hs::Transform& t, uint32_t key) override {
        if (!rigid(t.rot)) return hs::Sink::convex_placed(local, n, radius, t, key);
        if (n < 3 || (only && !only->count(key))) return;
        if (n > 255) n = 255;
        ipc::StaticConvexPlaced c{};
        c.group = group;
        c.vertex_count = uint32_t(n);
        c.key = key;
        c.radius = radius;
        c.pos = {t.pos[0], t.pos[1], t.pos[2]};
        c.rot = quat_of(t.rot);
        auto buf = record(ipc::Cmd::StaticConvexPlaced, c, size_t(n) * sizeof(ipc::Vec3));
        memcpy(buf.data() + sizeof(c), local, size_t(n) * sizeof(ipc::Vec3));
        queue(std::move(buf));
        ++shapes;
        if (keys_out) keys_out->push_back(key);
    }

    // hkpSampledHeightFieldShape (storage: float, compressed: offset + scale * uint16): heights on an xRes x zRes
    // grid stored z-major (getHeightAtImpl: storage[z * xRes + x]), vertices at (x, h, z) * m_intToFloatScale in the
    // shape's space. PhysX wants rows along x, row-major, as int16 samples around a midpoint.
    void height_field(const uint8_t* s, const hs::Transform& t) override {
        if (only || !s) return;
        const int xr = *reinterpret_cast<const int*>(s + layout::hk_hf_xres), zr = *reinterpret_cast<const int*>(s + layout::hk_hf_zres);
        const uint8_t kind = s[layout::hk_hf_kind];
        const float* scale = reinterpret_cast<const float*>(s + layout::hk_hf_int_to_float);
        const uint8_t* storage = *reinterpret_cast<const uint8_t* const*>(s + layout::hk_hf_storage);
        // OpenRFG64: the compressed kind's fields are 0 where the build has no such class.
        const bool compressed_known = layout::hk_hf_compressed_offset != 0 && layout::hk_hf_compressed_scale != 0;
        if (xr < 2 || zr < 2 || xr > 4096 || zr > 4096 || !storage || kind > 1 || (kind == 1 && !compressed_known)) {
            ++g_unsupported[hs::kSampledHeightField];
            return;
        }
        const float offset = kind == 1 ? *reinterpret_cast<const float*>(s + layout::hk_hf_compressed_offset) : 0.0f,
                    hscale = kind == 1 ? *reinterpret_cast<const float*>(s + layout::hk_hf_compressed_scale) : 0.0f;
        auto height = [&](int x, int z) {
            const int i = (g_hf_variant & 1) ? x * zr + z : z * xr + x;
            const float h = kind == 0 ? reinterpret_cast<const float*>(storage)[i]
                                      : offset + hscale * float(reinterpret_cast<const uint16_t*>(storage)[i]);
            return h * scale[1];
        };
        float lo = 1e30f, hi = -1e30f;
        for (int z = 0; z < zr; ++z)
            for (int x = 0; x < xr; ++x) {
                const float h = height(x, z);
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
        const float mid = 0.5f * (lo + hi);
        const float step = std::max((hi - lo) / 65000.0f, 1e-4f);
        ipc::StaticHeightField c{};
        c.group = group;
        c.rows = uint32_t(xr);
        c.columns = uint32_t(zr);
        // Havok splits an unflipped cell along the other diagonal than PhysX's tessellation flag: the 1,024-ray
        // check matched 935 rays this way (on the remaining ones Havok hits bodies the mirror leaves out) against
        // 758 the other way, 06 §13.
        c.flip = (s[layout::hk_hf_flip] != 0 ? 0u : 1u) ^ ((g_hf_variant & 2) ? 1u : 0u);
        const float centre[3] = {0, mid, 0};
        float o[3];
        t.apply(centre, o);
        c.origin = {o[0], o[1], o[2]};
        c.rot = quat_of(t.rot);
        c.row_scale = scale[0];
        c.column_scale = scale[2];
        c.height_scale = step;
        auto buf = record(ipc::Cmd::StaticHeightField, c, size_t(xr) * size_t(zr) * 2);
        auto* out = reinterpret_cast<int16_t*>(buf.data() + sizeof(c));
        for (int x = 0; x < xr; ++x)
            for (int z = 0; z < zr; ++z) {
                const float v = std::round((height(x, z) - mid) / step);
                out[size_t(x) * size_t(zr) + size_t(z)] = int16_t(std::clamp(v, -32767.0f, 32767.0f));
            }
        queue(std::move(buf));
        ++shapes;
    }

    void unsupported(uint8_t type) override { ++g_unsupported[type < hs::kTypeCount ? type : hs::kTypeCount]; }
};

// Collects the keys of a body's convex pieces, nothing else.
struct Keys : hs::Sink {
    std::vector<uint32_t> keys;
    void triangle(const float*, const float*, const float*) override {}
    void convex(const float (*)[3], int, float, uint32_t key) override { keys.push_back(key); }
    void height_field(const uint8_t*, const hs::Transform&) override {}
    void unsupported(uint8_t) override {}
};

// Area mode: a body's extent in the world and the statics it would make, without making them.
struct Bounds : hs::Sink {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    uint32_t convexes = 0, fields = 0;
    uint64_t triangles = 0;
    void add(const float p[3]) {
        for (int i = 0; i < 3; ++i) lo[i] = std::min(lo[i], p[i]), hi[i] = std::max(hi[i], p[i]);
    }
    void triangle(const float a[3], const float b[3], const float c[3]) override {
        add(a);
        add(b);
        add(c);
        ++triangles;
    }
    void convex(const float (*v)[3], int n, float, uint32_t) override {
        for (int i = 0; i < n; ++i) add(v[i]);
        ++convexes;
    }
    void height_field(const uint8_t* s, const hs::Transform& t) override {
        ++fields;
        if (!s) return;
        const int xr = *reinterpret_cast<const int*>(s + layout::hk_hf_xres), zr = *reinterpret_cast<const int*>(s + layout::hk_hf_zres);
        if (xr < 2 || zr < 2 || xr > 4096 || zr > 4096) return;
        const float* scale = reinterpret_cast<const float*>(s + layout::hk_hf_int_to_float);
        for (int c = 0; c < 4; ++c) {  // its corners (x and z are what matter)
            const float local[3] = {(c & 1) ? float(xr - 1) * scale[0] : 0.0f, 0.0f, (c & 2) ? float(zr - 1) * scale[2] : 0.0f};
            float w[3];
            t.apply(local, w);
            add(w);
        }
    }
    void unsupported(uint8_t) override {}
    uint32_t statics() const { return convexes + fields + uint32_t((triangles + kChunkTriangles - 1) / kChunkTriangles); }
};

bool walk_guarded(const uint8_t* shape, const hs::Transform& t, hs::Sink& sink) {
    __try {
        return hs::walk(shape, t, sink);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

uint32_t signature(const uint8_t* shape) {
    if (*reinterpret_cast<const uintptr_t*>(shape) == uintptr_t(rfg::addr(rfg::rva::rfg_destroyable_shape_bvtree_vftable)))
        return 1u + *reinterpret_cast<const uint32_t*>(shape + kDestroyableExisting);
    return 0;
}

void export_body(const uint8_t* entity, Mirrored& m) {
    Export e(m.group);
    m.keys.clear();
    if (m.signature) e.keys_out = &m.keys;
    if (!walk_guarded(hs::body_shape(entity), hs::body_transform(entity), e))
        LOG("world mirror: a body's shape could not be read completely (group %u)", m.group);
    e.flush();
    std::sort(m.keys.begin(), m.keys.end());
    m.shapes = e.shapes;
}

// A building changed: remove the pieces that left, add the ones that came back.
void update_body(const uint8_t* entity, Mirrored& m) {
    Keys now;
    walk_guarded(hs::body_shape(entity), hs::body_transform(entity), now);
    std::sort(now.keys.begin(), now.keys.end());
    std::vector<uint32_t> gone, back;
    std::set_difference(m.keys.begin(), m.keys.end(), now.keys.begin(), now.keys.end(), std::back_inserter(gone));
    std::set_difference(now.keys.begin(), now.keys.end(), m.keys.begin(), m.keys.end(), std::back_inserter(back));
    if (!gone.empty()) remove_keys(m.group, gone);
    if (!back.empty()) {
        const std::unordered_set<uint32_t> only(back.begin(), back.end());
        Export e(m.group);
        e.only = &only;
        walk_guarded(hs::body_shape(entity), hs::body_transform(entity), e);
    }
    m.keys.swap(now.keys);
    m.shapes = uint32_t(m.keys.size());
}

uint8_t* world() { return rfg::global<uint8_t*>(rfg::rva::Havok_world); }

const float* player_pos() {
    const uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)();
    return player ? reinterpret_cast<const float*>(player + layout::object_pos) : nullptr;
}

void log_export_done() {
    uint32_t unsupported = 0;
    for (uint32_t c : g_unsupported) unsupported += c;
    if (area()) {
        LOG("world mirror: the wanted bodies are exported: %u of %zu in the host, %llu statics (%.1f MB of commands, at "
            "most %.1f MB of them waiting at once)%s", g_mirrored, g_bodies.size(),
            static_cast<unsigned long long>(g_mirrored_statics), double(g_export_bytes) / (1024.0 * 1024.0),
            double(g_outbox_peak) / (1024.0 * 1024.0), unsupported ? "; leaf shapes left out:" : "");
    } else {
        LOG("world mirror: %zu Havok bodies exported (%.1f MB of commands, at most %.1f MB of them waiting at once)%s",
            g_bodies.size(), double(g_export_bytes) / (1024.0 * 1024.0), double(g_outbox_peak) / (1024.0 * 1024.0),
            unsupported ? "; leaf shapes left out:" : "");
    }
    for (int t = 0; t <= hs::kTypeCount; ++t)
        if (g_unsupported[t]) LOG("  %s: %u", hs::type_name(uint8_t(t)), g_unsupported[t]);
    // The game's collision models (every distinct piece shape of every loaded chunk, the terrain's, ...) share one
    // table of 0x7FFF; havok_cm_manager_add gives the ones past it model 0 or none. The B5b avenue (34,897 models
    // in its chunks) filled it: the terrain, loaded after, had no collision, and the last types' pieces took model
    // 0's shape (research/docs/07, B5b).
    const int models = *static_cast<const int*>(rfg::addr(rfg::rva::Num_collision_models));
#if defined(_WIN64)
    // OpenRFG64: its table may grow past rfg.exe's (OpenRFG64-memtest's does), so a count past 32,767 says nothing.
    LOG("world mirror: the game's collision model table holds %d (rfg.exe's: at most 32,767)", models);
#else
    LOG("world mirror: the game's collision model table holds %d of 32,767%s", models,
        models >= 0x7FFF ? ": FULL, the models loaded after it filled have no shape of their own (the terrain's, "
                           "a building type's pieces)" : "");
#endif
}

// Without the area mode: every body of the fixed island goes to the host.
void scan_whole() {
    uint8_t* w = world();
    if (!w) return;
    const uint8_t* island = *reinterpret_cast<uint8_t* const*>(w + layout::hk_world_fixed_island);  // hkpWorld::m_fixedIsland
    if (!island) return;
    const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + layout::hk_island_entities);
    const int n = *reinterpret_cast<const int*>(island + layout::hk_island_entity_count);
    const int64_t t0 = session::qpc();
    for (auto& [e, m] : g_bodies) m.seen = false;
    bool budget_left = g_outbox_bytes < kOutboxCap;
    uint32_t added = 0, changed = 0, removed = 0;
    uint32_t reused = 0;
    // OpenRFG64: bodies not mirrored yet are exported nearest the player first (by their origin). A big map's
    // export takes minutes (ten avenues: 544 bodies, 252 MB of commands, 160 s), and in the island's order the debris
    // around the player fell through what was not there yet: the plateau's ground among it.
    std::vector<std::pair<float, int>> fresh;  // (squared distance to the player, island index)
    const float* pp = player_pos();
    for (int i = 0; i < n; ++i) {
        const uint8_t* e = entities[i];
        if (!e || !hs::body_shape(e) || !collides_with_debris(e)) continue;
        const uint32_t sig = signature(hs::body_shape(e));
        const uint32_t uid = *reinterpret_cast<const uint32_t*>(e + kEntityUid);
        auto it = g_bodies.find(e);
        if (it != g_bodies.end() && it->second.uid != uid) {
            // Another body at the address of one that left: the old one goes, this one is new.
            if (!budget_left) {
                it->second.seen = true;  // keeps the old one's group until the next scan replaces it
                continue;
            }
            remove_group(it->second.group);
            g_entity_of_group.erase(it->second.group);
            g_bodies.erase(it);
            it = g_bodies.end();
            ++reused;
        }
        if (it != g_bodies.end()) {
            it->second.seen = true;
            if (it->second.signature != sig && budget_left) {
                it->second.signature = sig;
                if (sig) update_body(e, it->second);
                else {
                    remove_group(it->second.group);
                    export_body(e, it->second);
                }
                ++changed;
            }
        } else if (budget_left) {
            float d2 = 0.0f;
            if (pp) {
                const hs::Transform t = hs::body_transform(e);
                const float dx = t.pos[0] - pp[0], dy = t.pos[1] - pp[1], dz = t.pos[2] - pp[2];
                d2 = dx * dx + dy * dy + dz * dz;
            }
            fresh.emplace_back(d2, i);
            continue;
        } else {
            continue;  // next scan
        }
        if (session::ticks_to_ms(session::qpc() - t0) > kScanBudgetMs || g_outbox_bytes >= kOutboxCap)
            budget_left = false;
    }
    std::sort(fresh.begin(), fresh.end());
    static uint32_t s_first_logged = 0;  // the level's first exports, in the log (which went first)
    if (g_bodies.empty()) s_first_logged = 0;  // nothing mirrored yet: a level's first scan (on_frame clears a gone one)
    for (const auto& [d2, i] : fresh) {
        if (!budget_left) break;  // next scan
        const uint8_t* e = entities[i];
        const uint32_t sig = signature(hs::body_shape(e));
        const uint32_t uid = *reinterpret_cast<const uint32_t*>(e + kEntityUid);
        auto& m = g_bodies.emplace(e, Mirrored{g_next_group++, sig, true, {}, uid}).first->second;
        g_entity_of_group[m.group] = e;
        if (const uint8_t* obj = rfg::fn<uint8_t*(__cdecl*)(const uint8_t*)>(rfg::rva::object_from_world_obj)(e))
            m.object = *reinterpret_cast<const uint32_t*>(obj + layout::object_handle);  // object::handle
        export_body(e, m);
        ++added;
        if (!g_initial_done && s_first_logged < 3) {
            ++s_first_logged;
            LOG("world mirror: exported %s nearest: group %u, %zu shapes, its origin %.0f m from the player",
                s_first_logged == 1 ? "1st" : s_first_logged == 2 ? "2nd" : "3rd", m.group, m.keys.size(),
                double(std::sqrt(d2)));
        }
        if (session::ticks_to_ms(session::qpc() - t0) > kScanBudgetMs || g_outbox_bytes >= kOutboxCap)
            budget_left = false;
    }
    if (!fresh.empty() && added < fresh.size()) budget_left = false;  // some wait for the next scan
    for (auto it = g_bodies.begin(); it != g_bodies.end();) {
        if (!it->second.seen) {
            remove_group(it->second.group);
            g_entity_of_group.erase(it->second.group);
            it = g_bodies.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    const float ms = float(session::ticks_to_ms(session::qpc() - t0));
    g_stats.scan_ms += ms;
    g_stats.changes += added + changed + removed;
    g_mirrored = uint32_t(g_bodies.size());
    if (budget_left) ++g_complete_scans;  // every body looked at: what changed is in the outbox
    if (!g_initial_done && budget_left) {
        g_initial_done = true;
        g_export_done_ms = session::since_start_ms();
        log_export_done();
    } else if (g_initial_done) {
        g_log_added += added;
        g_log_changed += changed;
        g_log_removed += removed;
        g_log_reused += reused;
        g_reused_total += reused;
        const double now = session::since_start_ms();
        if (now >= g_next_change_log && (g_log_added || g_log_changed || g_log_removed)) {
            LOG("world mirror: in the last 5 s %u bodies added (%u at the address of one that left), %u changed "
                "(buildings losing pieces), %u removed", g_log_added, g_log_reused, g_log_changed, g_log_removed);
            g_log_added = g_log_changed = g_log_removed = g_log_reused = 0;
            g_next_change_log = now + 5000;
        }
    }
}

// ---- area mode ----

int kc(float v) { return int(std::floor(v / kKeyCellM)); }
uint64_t key3(int x, int y, int z) {  // a cell of kKeyCellM, 21 bits a coordinate
    constexpr int kBias = 1 << 20;
    return uint64_t(uint32_t(x + kBias) & 0x1FFFFFu) | (uint64_t(uint32_t(y + kBias) & 0x1FFFFFu) << 21) |
           (uint64_t(uint32_t(z + kBias) & 0x1FFFFFu) << 42);
}
uint64_t col_key(int x, int z) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(z); }

float distance_to(const Mirrored& m, float x, float z) {
    const float dx = std::max(std::max(m.lo[0] - x, x - m.hi[0]), 0.0f);
    const float dz = std::max(std::max(m.lo[2] - z, z - m.hi[2]), 0.0f);
    return std::sqrt(dx * dx + dz * dz);
}

// The cells (kKeyCellM) a body's box takes, widened by kKeyMarginM: what its pieces count for.
struct CellBox {
    int x0, x1, y0, y1, z0, z1;
};
CellBox cells_of(const Mirrored& m) {
    return {kc(m.lo[0] - kKeyMarginM), kc(m.hi[0] + kKeyMarginM), kc(m.lo[1] - kKeyMarginM),
            kc(m.hi[1] + kKeyMarginM), kc(m.lo[2] - kKeyMarginM), kc(m.hi[2] + kKeyMarginM)};
}

// The body's statics are in the host, with the pieces it lost or got back, and the host has applied them.
bool applied(const Mirrored& m) {
    if (!m.group || m.pending || m.stale) return false;
    const ipc::SnapshotHeader* s = pxbridge::snapshot();
    return s && int32_t(s->last_frame - m.flushed_frame) > 0;
}

Mirrored* body_of_group(uint32_t group) {
    const auto g = g_entity_of_group.find(group);
    if (g == g_entity_of_group.end()) return nullptr;
    const auto b = g_bodies.find(g->second);
    return b != g_bodies.end() && b->second.group == group ? &b->second : nullptr;
}

void clear_partial(Mirrored& m) {
    m.partial = false;
    m.cells_built = false;
    std::unordered_map<uint64_t, std::vector<uint32_t>>().swap(m.key_cells);
    std::unordered_set<uint64_t>().swap(m.covered);
    std::unordered_set<uint32_t>().swap(m.target);
    std::unordered_set<uint64_t>().swap(m.asks_done);
}

// The body's statics leave the host: after its records still waiting, if there are any.
void forget(Mirrored& m) {
    if (!m.group) return;
    g_queue_urgent = m.pending == 0;
    remove_group(m.group);
    g_queue_urgent = false;
    g_entity_of_group.erase(m.group);
    m.group = 0;
    m.pending = 0;
    m.stale = false;
    std::vector<uint32_t>().swap(m.keys);
    clear_partial(m);
}

void measure(const uint8_t* e, Mirrored& m) {
    Bounds b;
    const hs::Transform t = hs::body_transform(e);
    walk_guarded(hs::body_shape(e), t, b);
    if (b.lo[0] > b.hi[0]) b.add(t.pos);  // nothing it could read: its origin
    for (int i = 0; i < 3; ++i) m.lo[i] = b.lo[i], m.hi[i] = b.hi[i];
    m.shapes = b.statics();
    m.bounded = true;
}

// The cells each piece of a building counts for: its box widened by kKeyMarginM.
struct KeyCells : hs::Sink {
    std::unordered_map<uint64_t, std::vector<uint32_t>>& out;
    explicit KeyCells(std::unordered_map<uint64_t, std::vector<uint32_t>>& o) : out(o) {}
    void triangle(const float*, const float*, const float*) override {}
    void convex(const float (*v)[3], int n, float, uint32_t key) override {
        if (n <= 0) return;
        float lo[3] = {v[0][0], v[0][1], v[0][2]}, hi[3] = {v[0][0], v[0][1], v[0][2]};
        for (int i = 1; i < n; ++i)
            for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], v[i][a]), hi[a] = std::max(hi[a], v[i][a]);
        const int x0 = kc(lo[0] - kKeyMarginM), x1 = kc(hi[0] + kKeyMarginM), y0 = kc(lo[1] - kKeyMarginM),
                  y1 = kc(hi[1] + kKeyMarginM), z0 = kc(lo[2] - kKeyMarginM), z1 = kc(hi[2] + kKeyMarginM);
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z) out[key3(x, y, z)].push_back(key);
    }
    void height_field(const uint8_t*, const hs::Transform&) override {}
    void unsupported(uint8_t) override {}
};

bool keys_guarded(const uint8_t* shape, std::vector<uint32_t>& out) {
    __try {
        return hs::container_keys(shape, out);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool walk_keys_guarded(const uint8_t* shape, const hs::Transform& t, const uint32_t* keys, size_t n, hs::Sink& sink) {
    __try {
        return hs::walk_keys(shape, t, keys, n, sink);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

std::vector<uint32_t> current_keys(const uint8_t* e) {
    std::vector<uint32_t> now;
    keys_guarded(hs::body_shape(e), now);
    std::sort(now.begin(), now.end());
    return now;
}

// The pieces in PhysX by column (x and z, kKeyCellM) and the y cells they lie in: the snapshot's live slots (debris,
// loose chunks, the player's and vehicles' stand-ins), and for the moving ones where they go in the next 2 s.
std::unordered_map<uint64_t, std::vector<int>> g_piece_cols;
uint32_t g_piece_cells = 0;
void mark_piece(float x, float y, float z) {
    std::vector<int>& ys = g_piece_cols[col_key(kc(x), kc(z))];
    const int cy = kc(y);
    if (std::find(ys.begin(), ys.end(), cy) == ys.end()) {
        ys.push_back(cy);
        ++g_piece_cells;
    }
}

void count_pieces() {
    g_piece_cols.clear();
    g_piece_cells = 0;
    g_pieces_counted = 0;
    const ipc::SnapshotHeader* s = pxbridge::snapshot();
    if (!s) return;
    const ipc::BodyPose* poses = pxbridge::poses(s);
    const uint8_t* states = pxbridge::states(s);
    const float hz = pxbridge::step_hz();
    const uint32_t slots = std::min(s->slots, pxbridge::max_bodies());
    int lx = INT32_MIN, ly = 0, lz = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        if (!(states[i] & ipc::kPoseAlive)) continue;
        const ipc::BodyPose& p = poses[i];
        ++g_pieces_counted;
        const int cx = kc(p.pos.x), cy = kc(p.pos.y), cz = kc(p.pos.z);
        if (cx != lx || cy != ly || cz != lz) {
            mark_piece(p.pos.x, p.pos.y, p.pos.z);
            lx = cx, ly = cy, lz = cz;
        }
        if (!(states[i] & ipc::kPoseAwake) || hz <= 0.0f) continue;
        float v[3] = {(p.pos.x - p.prev_pos.x) * hz, (p.pos.y - p.prev_pos.y) * hz, (p.pos.z - p.prev_pos.z) * hz};
        const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (speed < 2.0f) continue;
        const float k = std::min(speed, 40.0f) / speed;
        for (float& c : v) c *= k;
        for (const float t : {0.25f, 0.5f, 1.0f, 1.5f, 2.0f})
            mark_piece(p.pos.x + v[0] * t, p.pos.y + v[1] * t - 4.9f * t * t, p.pos.z + v[2] * t);
    }
}

bool near_pieces(const Mirrored& m) {
    if (g_piece_cols.empty()) return false;
    const CellBox b = cells_of(m);
    auto any = [&](const std::vector<int>& ys) {
        for (const int y : ys)
            if (y >= b.y0 && y <= b.y1) return true;
        return false;
    };
    const uint64_t cols = uint64_t(int64_t(b.x1) - b.x0 + 1) * uint64_t(int64_t(b.z1) - b.z0 + 1);
    if (cols > g_piece_cols.size()) {
        for (const auto& [k, ys] : g_piece_cols) {
            const int x = int32_t(uint32_t(k >> 32)), z = int32_t(uint32_t(k));
            if (x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1 && any(ys)) return true;
        }
        return false;
    }
    for (int x = b.x0; x <= b.x1; ++x)
        for (int z = b.z0; z <= b.z1; ++z) {
            const auto it = g_piece_cols.find(col_key(x, z));
            if (it != g_piece_cols.end() && any(it->second)) return true;
        }
    return false;
}

// The cells of a body's box with pieces in them.
void piece_cells_of(const Mirrored& m, std::vector<uint64_t>& out) {
    const CellBox b = cells_of(m);
    auto add = [&](int x, int z, const std::vector<int>& ys) {
        for (const int y : ys)
            if (y >= b.y0 && y <= b.y1) out.push_back(key3(x, y, z));
    };
    const uint64_t cols = uint64_t(int64_t(b.x1) - b.x0 + 1) * uint64_t(int64_t(b.z1) - b.z0 + 1);
    if (cols > g_piece_cols.size()) {
        for (const auto& [k, ys] : g_piece_cols) {
            const int x = int32_t(uint32_t(k >> 32)), z = int32_t(uint32_t(k));
            if (x >= b.x0 && x <= b.x1 && z >= b.z0 && z <= b.z1) add(x, z, ys);
        }
        return;
    }
    for (int x = b.x0; x <= b.x1; ++x)
        for (int z = b.z0; z <= b.z1; ++z) {
            const auto it = g_piece_cols.find(col_key(x, z));
            if (it != g_piece_cols.end()) add(x, z, it->second);
        }
}

void index_bodies() {
    g_tiles.clear();
    for (auto& [e, m] : g_bodies) {
        if (!m.bounded) continue;
        const int x0 = int(std::floor((m.lo[0] - kKeyMarginM) / kTileM)), x1 = int(std::floor((m.hi[0] + kKeyMarginM) / kTileM));
        const int z0 = int(std::floor((m.lo[2] - kKeyMarginM) / kTileM)), z1 = int(std::floor((m.hi[2] + kKeyMarginM) / kTileM));
        for (int x = x0; x <= x1; ++x)
            for (int z = z0; z <= z1; ++z) g_tiles[col_key(x, z)].emplace_back(e, &m);
    }
    g_tiles_dirty = false;
}

// Calls fn(entity, body) once for each body whose tiles meet the area (x and z).
template <typename Fn>
void for_bodies_near(float x0, float z0, float x1, float z1, Fn&& fn) {
    ++g_stamp;
    const int tx0 = int(std::floor(x0 / kTileM)), tx1 = int(std::floor(x1 / kTileM));
    const int tz0 = int(std::floor(z0 / kTileM)), tz1 = int(std::floor(z1 / kTileM));
    for (int x = tx0; x <= tx1; ++x)
        for (int z = tz0; z <= tz1; ++z) {
            const auto it = g_tiles.find(col_key(x, z));
            if (it == g_tiles.end()) continue;
            for (const auto& [e, m] : it->second) {
                if (m->stamp == g_stamp) continue;
                m->stamp = g_stamp;
                if (!fn(e, *m)) return;
            }
        }
}

// Areas asked for (ready_at): from the ground up to a little above where a piece waits.
bool overlaps_ask(const Mirrored& m, const Request& r) {
    return m.lo[0] - kKeyMarginM <= r.x1 && m.hi[0] + kKeyMarginM >= r.x0 && m.lo[2] - kKeyMarginM <= r.z1 &&
           m.hi[2] + kKeyMarginM >= r.z0 && m.lo[1] - kKeyMarginM <= r.y1;
}

bool requested(const Mirrored& m) { return !m.asks.empty(); }

// The cells of an area asked for within a body's box.
CellBox ask_cells(const Mirrored& m, float x0, float z0, float x1, float z1, float y1) {
    const CellBox b = cells_of(m);
    return {std::max(kc(x0), b.x0), std::min(kc(x1), b.x1), b.y0, std::min(kc(y1), b.y1), std::max(kc(z0), b.z0),
            std::min(kc(z1), b.z1)};
}

// A building wanted for the pieces near it or areas asked for: the pieces in those cells join it in the host.
// Returns the pieces sent.
uint32_t grow(const uint8_t* e, Mirrored& m) {
    if (!m.cells_built) {
        KeyCells cells(m.key_cells);
        walk_guarded(hs::body_shape(e), hs::body_transform(e), cells);
        m.cells_built = true;
    }
    std::vector<uint64_t> cells;
    piece_cells_of(m, cells);
    for (const uint64_t k : m.asks) {
        if (!m.asks_done.insert(k).second) continue;
        const auto found = g_requests.find(k);
        if (found == g_requests.end()) continue;
        const Request& r = found->second;
        const CellBox c = ask_cells(m, r.x0, r.z0, r.x1, r.z1, r.y1);
        for (int x = c.x0; x <= c.x1; ++x)
            for (int y = c.y0; y <= c.y1; ++y)
                for (int z = c.z0; z <= c.z1; ++z) cells.push_back(key3(x, y, z));
    }
    std::vector<uint32_t> add;
    for (const uint64_t c : cells) {
        if (!m.covered.insert(c).second) continue;
        const auto it = m.key_cells.find(c);
        if (it == m.key_cells.end()) continue;
        for (const uint32_t k : it->second)
            if (m.target.insert(k).second) add.push_back(k);
    }
    if (!m.group) {
        m.group = g_next_group++;
        g_entity_of_group[m.group] = e;
        m.signature = signature(hs::body_shape(e));
        m.partial = true;
        m.pending = 0;
        m.stale = false;
        m.flushed_frame = pxbridge::frame();
        m.keys.clear();
    }
    if (add.empty()) return 0;
    std::sort(add.begin(), add.end());
    const std::vector<uint32_t> now = current_keys(e);
    std::vector<uint32_t> live;
    std::set_intersection(add.begin(), add.end(), now.begin(), now.end(), std::back_inserter(live));
    if (live.empty()) return 0;
    const size_t before = m.keys.size();
    g_queue_body = &m;
    Export ex(m.group);
    ex.keys_out = &m.keys;
    walk_keys_guarded(hs::body_shape(e), hs::body_transform(e), live.data(), live.size(), ex);
    ex.flush();
    g_queue_body = nullptr;
    std::sort(m.keys.begin(), m.keys.end());
    m.shapes = uint32_t(m.keys.size());
    if (!m.pending) m.flushed_frame = pxbridge::frame();
    return uint32_t(m.keys.size() - before);
}

// A building in part that lost or got back pieces: those it lost leave, those back that it wants come.
void update_partial(const uint8_t* e, Mirrored& m) {
    const std::vector<uint32_t> now = current_keys(e);
    std::vector<uint32_t> gone, back, kept;
    std::set_difference(m.keys.begin(), m.keys.end(), now.begin(), now.end(), std::back_inserter(gone));
    std::set_intersection(m.keys.begin(), m.keys.end(), now.begin(), now.end(), std::back_inserter(kept));
    for (const uint32_t k : now)
        if (m.target.count(k) && !std::binary_search(m.keys.begin(), m.keys.end(), k)) back.push_back(k);
    if (!gone.empty()) remove_keys(m.group, gone);
    if (!back.empty()) {
        Export ex(m.group);
        walk_keys_guarded(hs::body_shape(e), hs::body_transform(e), back.data(), back.size(), ex);
        ex.flush();
    }
    kept.insert(kept.end(), back.begin(), back.end());
    std::sort(kept.begin(), kept.end());
    m.keys.swap(kept);
    m.shapes = uint32_t(m.keys.size());
}

// A building in part that is wanted whole now (the player came near): the rest of its pieces come.
void make_whole(const uint8_t* e, Mirrored& m) {
    std::vector<uint32_t> now = current_keys(e);
    std::vector<uint32_t> add;
    std::set_difference(now.begin(), now.end(), m.keys.begin(), m.keys.end(), std::back_inserter(add));
    if (!add.empty()) {
        Export ex(m.group);
        walk_keys_guarded(hs::body_shape(e), hs::body_transform(e), add.data(), add.size(), ex);
        ex.flush();
    }
    m.keys.swap(now);
    m.shapes = uint32_t(m.keys.size());
    clear_partial(m);
}

void scan_area() {
    uint8_t* w = world();
    if (!w) return;
    const uint8_t* island = *reinterpret_cast<uint8_t* const*>(w + layout::hk_world_fixed_island);  // hkpWorld::m_fixedIsland
    if (!island) return;
    const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + layout::hk_island_entities);
    const int n = *reinterpret_cast<const int*>(island + layout::hk_island_entity_count);
    const int64_t t0 = session::qpc();
    auto elapsed = [&] { return session::ticks_to_ms(session::qpc() - t0); };
    auto out_of_budget = [&] { return elapsed() > kScanBudgetMs || g_outbox_bytes >= kOutboxCap; };
    const double now = session::since_start_ms();
    if (g_level_ms == 0) g_level_ms = now;
    const float* pp = player_pos();
    uint32_t added = 0, grown = 0, changed = 0, removed = 0, dropped = 0, reused = 0;

    // The bodies in Havok's fixed island.
    for (auto& [e, m] : g_bodies) m.seen = false;
    const auto object_of = rfg::fn<uint8_t*(__cdecl*)(const uint8_t*)>(rfg::rva::object_from_world_obj);
    for (int i = 0; i < n; ++i) {
        const uint8_t* e = entities[i];
        if (!e || !hs::body_shape(e) || !collides_with_debris(e)) continue;
        const uint32_t uid = *reinterpret_cast<const uint32_t*>(e + kEntityUid);
        auto it = g_bodies.find(e);
        if (it != g_bodies.end() && it->second.uid != uid) {
            // Another body at the address of one that left: the old one goes, this one is new.
            if (it->second.group) ++removed;
            forget(it->second);
            g_bodies.erase(it);
            g_tiles_dirty = true;
            it = g_bodies.end();
            ++reused;
        }
        if (it == g_bodies.end()) {
            Mirrored fresh{0, signature(hs::body_shape(e)), true, {}, uid};
            if (const uint8_t* obj = object_of(e)) fresh.object = *reinterpret_cast<const uint32_t*>(obj + layout::object_handle);
            it = g_bodies.emplace(e, std::move(fresh)).first;
        }
        it->second.seen = true;
    }
    for (auto it = g_bodies.begin(); it != g_bodies.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        if (it->second.group) ++removed;
        forget(it->second);
        it = g_bodies.erase(it);
        g_tiles_dirty = true;
    }

    // New bodies are measured, the nearest first.
    std::vector<std::pair<float, std::pair<const uint8_t*, Mirrored*>>> unbounded;
    for (auto& [e, m] : g_bodies) {
        if (m.bounded) continue;
        float d2 = 0.0f;
        if (pp) {
            const hs::Transform t = hs::body_transform(e);
            const float dx = t.pos[0] - pp[0], dz = t.pos[2] - pp[2];
            d2 = dx * dx + dz * dz;
        }
        unbounded.push_back({d2, {e, &m}});
    }
    std::sort(unbounded.begin(), unbounded.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    uint32_t measured = 0;
    for (const auto& [d2, em] : unbounded) {
        if (measured && elapsed() > kBoundsBudgetMs) break;
        measure(em.first, *em.second);
        ++measured;
    }
    g_unbounded = uint32_t(unbounded.size()) - measured;
    if (measured) g_tiles_dirty = true;
    if (g_tiles_dirty) index_bodies();
    if (g_unbounded == 0 && !g_measured_logged && !g_bodies.empty()) {
        g_measured_logged = true;
        uint64_t total = 0;
        for (const auto& [e, m] : g_bodies) total += m.shapes;
        g_level_whole = total <= g_budget;
        if (g_level_whole)
            LOG("world mirror: %zu Havok bodies make %llu statics in the host, within PhysXWorldStatics (%u): all of "
                "them go there (measured in %.1f s)", g_bodies.size(), static_cast<unsigned long long>(total), g_budget,
                (now - g_level_ms) / 1000.0 + elapsed() / 1000.0);
        else
            LOG("world mirror: %zu Havok bodies would make %llu statics in the host, more than PhysXWorldStatics (%u): "
                "it gets the ground and buildings within %.0f m of the player, and of the others the pieces near the "
                "debris in PhysX and where pieces wait to come over, as they come and go (measured in %.1f s)",
                g_bodies.size(), static_cast<unsigned long long>(total), g_budget, double(g_near_m),
                (now - g_level_ms) / 1000.0 + elapsed() / 1000.0);
    }

    // What is wanted: all of a body near the player (all of the level when it is within the budget, and any body that
    // is not a building); of the other buildings, the pieces near the debris and where pieces wait to come over.
    if (now >= g_next_pieces_ms) {
        g_next_pieces_ms = now + kPiecesEveryMs;
        count_pieces();
    }
    for (auto it = g_requests.begin(); it != g_requests.end();) it = it->second.until < now ? g_requests.erase(it) : std::next(it);
    for (auto& [e, m] : g_bodies) m.asks.clear();
    for (const auto& [k, r] : g_requests)
        for_bodies_near(r.x0, r.z0, r.x1, r.z1, [&, key = k, area = r](const uint8_t*, Mirrored& m) {
            if (overlaps_ask(m, area)) m.asks.push_back(key);
            return true;
        });
    struct Candidate {
        float distance;
        const uint8_t* e;
        Mirrored* m;
    };
    std::vector<Candidate> wholes, parts;
    for (auto& [e, m] : g_bodies) {
        m.wanted = false;
        if (!m.bounded) continue;
        const float d = pp ? distance_to(m, pp[0], pp[2]) : 1e9f;
        const bool whole = g_level_whole || d <= g_near_m;
        if (!whole && !near_pieces(m) && !requested(m)) continue;
        m.wanted = true;
        m.wanted_ms = now;
        if (whole || !m.signature) {
            if (!m.group || m.partial) wholes.push_back({d, e, &m});
        } else if (!m.group || m.partial) {
            parts.push_back({d, e, &m});  // a building in the host whole stays whole
        }
    }

    // Bodies no longer wanted leave the host; buildings in it that lost or got back pieces change there.
    bool complete = g_unbounded == 0;
    for (auto& [e, m] : g_bodies) {
        if (!m.group) continue;
        if (!m.wanted && now - m.wanted_ms >= kHoldMs && !m.pending) {
            forget(m);
            ++dropped;
            continue;
        }
        const uint32_t sig = signature(hs::body_shape(e));
        if (sig == m.signature) {
            m.stale = false;
            continue;
        }
        if (m.pending || out_of_budget()) {
            m.stale = true;  // the next scan
            complete = false;
            continue;
        }
        m.signature = sig;
        g_queue_body = &m;
        g_queue_urgent = true;
        if (m.partial) {
            update_partial(e, m);
        } else if (sig) {
            update_body(e, m);
        } else {
            remove_group(m.group);
            export_body(e, m);
        }
        g_queue_body = nullptr;
        g_queue_urgent = false;
        if (!m.pending) m.flushed_frame = pxbridge::frame();
        m.stale = false;
        ++changed;
    }

    // Exports, the nearest first: whole bodies, then the pieces of the others.
    auto nearest = [](const Candidate& a, const Candidate& b) { return a.distance < b.distance; };
    std::sort(wholes.begin(), wholes.end(), nearest);
    std::sort(parts.begin(), parts.end(), nearest);
    uint32_t missing = 0;
    for (const Candidate& c : wholes) {
        if (out_of_budget()) {
            ++missing;
            continue;
        }
        Mirrored& m = *c.m;
        g_queue_body = &m;
        if (m.group) {
            make_whole(c.e, m);
        } else {
            m.group = g_next_group++;
            g_entity_of_group[m.group] = c.e;
            m.signature = signature(hs::body_shape(c.e));
            m.pending = 0;
            m.stale = false;
            m.flushed_frame = pxbridge::frame();
            export_body(c.e, m);
            ++added;
        }
        g_queue_body = nullptr;
        if (!m.pending) m.flushed_frame = pxbridge::frame();
    }
    for (const Candidate& c : parts) {
        if (out_of_budget()) {
            ++missing;
            continue;
        }
        if (!c.m->group) ++added;
        grown += grow(c.e, *c.m);
    }
    g_missing = missing;

    uint32_t mirrored = 0, partial = 0;
    uint64_t statics = 0;
    for (const auto& [e, m] : g_bodies)
        if (m.group) ++mirrored, partial += m.partial ? 1 : 0, statics += m.shapes;
    g_mirrored = mirrored;
    g_partial = partial;
    g_mirrored_statics = statics;
    const float ms = float(elapsed());
    g_stats.scan_ms += ms;
    g_log_scan_ms += ms;
    g_stats.changes += added + changed + removed + dropped;
    if (complete) ++g_complete_scans;  // every body in the host looked at: what changed is in the outbox

    if (!g_initial_done && g_unbounded == 0 && missing == 0) {
        g_initial_done = true;
        g_export_done_ms = now;
        log_export_done();
    }
    // Debris may come over once the bodies around the player are in the host.
    if (!g_host_has_world && g_unbounded == 0 && pp) {
        bool all = true;
        uint32_t near_bodies = 0;
        uint64_t near_statics = 0;
        for (const auto& [e, m] : g_bodies) {
            if (distance_to(m, pp[0], pp[2]) > g_near_m) continue;
            if (!applied(m) || m.partial) {
                all = false;
                break;
            }
            ++near_bodies;
            near_statics += m.shapes;
        }
        if (all) {
            g_host_has_world = true;
            LOG("world mirror: the host has the static world within %.0f m of the player (%u bodies, %llu statics), "
                "%.1f s after the level's first scan", double(g_near_m), near_bodies,
                static_cast<unsigned long long>(near_statics), (now - g_level_ms) / 1000.0);
        }
    }
    g_log_added += added;
    g_log_grown += grown;
    g_log_changed += changed;
    g_log_removed += removed;
    g_log_reused += reused;
    g_log_dropped += dropped;
    g_reused_total += reused;
    if (g_host_has_world && now >= g_next_change_log &&
        (g_log_added || g_log_grown || g_log_changed || g_log_removed || g_log_dropped || g_asked != g_asked_logged)) {
        LOG("world mirror: %u of %zu bodies in the host (%u of them in part), %llu statics; in the last 5 s %u added, "
            "%u pieces near debris added, %u changed (buildings losing pieces), %u dropped (no longer near), %u removed "
            "(%u at the address of one that left); %u cells with pieces (%u counted), %zu areas asked for (%llu asks not "
            "ready yet), %u wanted bodies still to come; the scans took %.0f ms, ready_at %.0f ms (%llu asks, %llu "
            "worked out)", g_mirrored, g_bodies.size(), g_partial,
            static_cast<unsigned long long>(g_mirrored_statics), g_log_added, g_log_grown, g_log_changed, g_log_dropped,
            g_log_removed, g_log_reused, g_piece_cells, g_pieces_counted, g_requests.size(),
            static_cast<unsigned long long>(g_asked - g_asked_logged), g_missing, g_log_scan_ms, g_log_ask_ms,
            static_cast<unsigned long long>(g_log_asks), static_cast<unsigned long long>(g_log_misses));
        g_log_scan_ms = g_log_ask_ms = 0;
        g_log_asks = g_log_misses = 0;
        g_log_added = g_log_grown = g_log_changed = g_log_removed = g_log_reused = g_log_dropped = 0;
        g_asked_logged = g_asked;
        g_next_change_log = now + 5000;
    }
}

void scan() {
    if (area()) scan_area();
    else scan_whole();
}

// The urgent records first (area mode), then the rest, in order, while the ring has room. Records counted for a body
// move its pending count; its last one in the ring marks the frame after which the host has applied them all.
// OpenRFG64: the ring (4 MB) is everyone's: the debris coming over, explosions, the player. The static world fills
// it only up to kMirrorRingBytes (a record larger than that goes alone into an empty ring), so the others always find
// room; the host cooks statics far slower than a megabyte a frame anyway. On ten avenues the area mode kept the ring
// full through a demolition, and the debris stayed in Havok: 1,142 pieces taken in 80 s of 5,000-5,700 adoptable,
// the game at 5 fps (2026-11-21).
constexpr uint32_t kMirrorRingBytes = 1u << 20;
bool flush_box(std::deque<Out>& box) {
    uint32_t last_group = 0;
    Mirrored* last = nullptr;
    while (!box.empty()) {
        const Out& o = box.front();
        if (pxbridge::ring_free() < o.rec.size()) return false;
        const uint32_t used = pxbridge::ring_used();
        if (used > 0 && used + o.rec.size() > kMirrorRingBytes) return false;
        if (!pxbridge::send_record(o.rec.data(), uint32_t(o.rec.size()))) return false;
        g_stats.sent_kb += uint32_t(o.rec.size() >> 10);
        g_outbox_bytes -= o.rec.size();
        if (o.group) {
            if (o.group != last_group) last_group = o.group, last = body_of_group(o.group);
            if (last && last->pending && --last->pending == 0) last->flushed_frame = pxbridge::frame();
        }
        box.pop_front();
    }
    return true;
}

void flush_outbox() {
    if (flush_box(g_urgent)) flush_box(g_outbox);
}

// What the flushed scans wait for: the changes (area mode), everything (else).
bool changes_sent() { return area() ? g_urgent.empty() : g_outbox.empty(); }

// ---- the check ----

constexpr uint32_t kCheckRays = 1024;
constexpr float kCheckHeight = 60.0f, kCheckLength = 200.0f, kCheckRadius = 60.0f, kMatchM = 0.05f;
enum class Check { Idle, Waiting, Sent, Done } g_check_state = Check::Idle;
double g_check_at = 0;
uint32_t g_check_request = 0, g_checks = 0;
std::vector<ipc::Ray> g_rays;
std::vector<float> g_havok;
struct HavokHit {
    uint32_t filter_info = 0;  // of the body hit
    uint8_t shape_type = 0xFF;
    uint8_t motion = 0;        // hkpMotion::MotionType: 5 fixed (mirrored), others move
    bool debris_collides = true;
    int8_t object_type = -1;   // the game object hit (object_type, -1: none)
    uint32_t havok_handle = 0xFFFFFFFFu;
};
std::vector<HavokHit> g_havok_hit;

// raycast_input (start, end, radius, filter params, filter callback, filter info, collector type, flags) and
// raycast_output (hkArray<hit_info>: data, size, capacity); hit_info: hit_fraction +0x24, hit_havok_handle +0x30.
float havok_distance(uint32_t fn_rva, const ipc::Ray& r, HavokHit* what = nullptr) {
    // OpenRFG64: the buffers are as large as the running build's raycast_input / raycast_output (64 and 16 bytes
    // on x64, 48 and 12 in rfg.exe), zeroed; start and end are their first members in both.
    alignas(16) uint8_t in[0x80] = {};
    alignas(16) uint8_t out[0x40] = {};
    if (layout::raycast_input_size > sizeof(in) || layout::raycast_output_size > sizeof(out)) return -1.0f;
    float* f = reinterpret_cast<float*>(in);
    f[0] = r.origin.x, f[1] = r.origin.y, f[2] = r.origin.z;
    f[3] = r.origin.x + r.direction.x * r.max_distance;
    f[4] = r.origin.y + r.direction.y * r.max_distance;
    f[5] = r.origin.z + r.direction.z * r.max_distance;
    rfg::fn<void(__fastcall*)(void* RFG_EDX_TYPE)>(rfg::rva::raycast_output__raycast_output)(out RFG_EDX_ARG);
    const bool hit = rfg::fn<bool(__cdecl*)(void*, void*)>(fn_rva)(in, out);
    float d = -1.0f;
    const uint8_t* hits = *reinterpret_cast<const uint8_t* const*>(out);
    const int count = *reinterpret_cast<const int*>(out + layout::raycast_output_count);
    const uint32_t stride = layout::raycast_hit_size;
    if (hit && hits && count > 0) {
        float best = 2.0f;
        int best_i = 0;
        for (int i = 0; i < count; ++i) {
            const float fr = *reinterpret_cast<const float*>(hits + i * stride + layout::raycast_hit_fraction);
            if (fr < best) best = fr, best_i = i;
        }
        if (best <= 1.0f) {
            d = best * r.max_distance;
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(hits + best_i * stride + layout::raycast_hit_havok_handle);
            const uint32_t object = *reinterpret_cast<const uint32_t*>(hits + best_i * stride + layout::raycast_hit_object);
            if (what) {
                what->havok_handle = handle;
                if (object != 0xFFFFFFFFu) {
                    const auto from_handle = rfg::fn<uint8_t*(__fastcall*)(void* RFG_EDX_TYPE, uint32_t)>(
                        rfg::rva::world__object_from_handle);
                    if (const uint8_t* obj = from_handle(rfg::addr(rfg::rva::World) RFG_EDX_ARG, object))
                        what->object_type = int8_t(obj[layout::object_type]);
                }
            }
            if (what && handle != 0xFFFFFFFFu) {
                const uint8_t* body = static_cast<uint8_t* const*>(rfg::addr(rfg::rva::Hk_rigid_bodies))[handle];
                if (body) {
                    what->filter_info = *reinterpret_cast<const uint32_t*>(body + kFilterInfo);
                    what->shape_type = hs::body_shape(body) ? hs::type_of(hs::body_shape(body)) : 0xFF;
                    what->debris_collides = collides_with_debris(body);
                    what->motion = hs::body_motion_type(body);
                }
            }
        }
    }
    rfg::fn<void(__fastcall*)(void* RFG_EDX_TYPE)>(rfg::rva::raycast_output___raycast_output)(out RFG_EDX_ARG);
    return d;
}

void check_start() {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)();
    if (!player) return;
    const float* p = reinterpret_cast<const float*>(player + layout::object_pos);
    std::mt19937 rng(20261020);
    std::uniform_real_distribution<float> u(-kCheckRadius, kCheckRadius);
    g_rays.clear();
    g_havok.clear();
    g_havok_hit.clear();
    for (uint32_t i = 0; i < kCheckRays; ++i) {
        ipc::Ray r{};
        r.origin = {p[0] + u(rng), p[1] + kCheckHeight, p[2] + u(rng)};
        r.direction = {0, -1, 0};
        r.max_distance = kCheckLength;
        g_rays.push_back(r);
        // The nearer of Havok's static level and its destroyable geometry: what the mirror holds.
        HavokHit ha, hb;
        const float a = havok_distance(rfg::rva::havok_raycast_against_static_level_only, r, &ha);
        const float b = havok_distance(rfg::rva::havok_raycast_against_destroyable_geometry, r, &hb);
        const bool use_a = a >= 0 && (b < 0 || a <= b);
        g_havok.push_back(use_a ? a : b);
        g_havok_hit.push_back(use_a ? ha : hb);
    }
    g_check_request = 0x5EED0000u | (GetTickCount() & 0xFFFF);
    if (pxbridge::raycast_batch(g_check_request, g_rays.data(), uint32_t(g_rays.size()))) g_check_state = Check::Sent;
}

void check_finish() {
    const float* px = pxbridge::query_results();
    const uint32_t* kinds = pxbridge::query_hit_kinds();
    const std::wstring name = ++g_checks == 1 ? L"\\world_mirror_check.csv"
                                              : L"\\world_mirror_check_" + std::to_wstring(g_checks) + L".csv";
    if (FILE* f = _wfsopen((session::run_dir() + name).c_str(), L"w", _SH_DENYNO)) {
        fprintf(f, "ray,x,z,havok_m,mirror_m,mirror_hit,havok_layer,havok_group,havok_shape,havok_motion,"
                   "havok_debris_collides,havok_handle,object_type\n");
        static const char* const kKinds[] = {"none", "sphere", "plane", "capsule", "box", "convex_core", "convex", "particles",
                                             "tetmesh", "trimesh", "heightfield"};
        for (size_t i = 0; i < g_rays.size(); ++i)
            fprintf(f, "%zu,%.2f,%.2f,%.3f,%.3f,%s,%u,%u,%s,%u,%d,%08X,%d\n", i, double(g_rays[i].origin.x),
                    double(g_rays[i].origin.z), double(g_havok[i]), double(px[i]), kinds[i] < 11 ? kKinds[kinds[i]] : "other",
                    g_havok_hit[i].filter_info & 0x7F, g_havok_hit[i].filter_info >> 16,
                    g_havok_hit[i].shape_type == 0xFF ? "-" : hs::type_name(g_havok_hit[i].shape_type),
                    unsigned(g_havok_hit[i].motion), g_havok_hit[i].debris_collides ? 1 : 0, g_havok_hit[i].havok_handle,
                    int(g_havok_hit[i].object_type));
        fclose(f);
    }
    uint32_t match = 0, differ = 0, missing = 0, extra = 0, none = 0, excluded = 0, logged = 0;
    double max_match = 0, max_differ = 0;
    for (size_t i = 0; i < g_rays.size(); ++i) {
        const float h = g_havok[i], x = px[i];
        const double rx = double(g_rays[i].origin.x), rz = double(g_rays[i].origin.z);
        if (h < 0 && x < 0) {
            ++none;
        } else if (h >= 0 && x < 0) {
            ++missing;
            if (logged++ < 10) LOG("  ray at (%.1f, %.1f): Havok hits at %.2f m, the mirror nothing", rx, rz, double(h));
        } else if (h < 0) {
            ++extra;
            if (logged++ < 10) LOG("  ray at (%.1f, %.1f): the mirror hits at %.2f m, Havok nothing", rx, rz, double(x));
        } else if (std::fabs(h - x) <= kMatchM) {
            ++match;
            max_match = std::max(max_match, double(std::fabs(h - x)));
        } else if (h < x && (!g_havok_hit[i].debris_collides || g_havok_hit[i].motion != 5)) {
            ++excluded;  // Havok's ray stops at a body the mirror leaves out: one debris does not collide with, or
                         // one that moves (rubble, chunks; X3 territory)
        } else {
            ++differ;
            max_differ = std::max(max_differ, double(std::fabs(h - x)));
            if (logged++ < 10) LOG("  ray at (%.1f, %.1f): Havok %.2f m, the mirror %.2f m", rx, rz, double(h), double(x));
        }
    }
    LOG("world mirror check: %zu rays: %u match within 5 cm (largest difference %.3f m), %u differ (up to %.2f m), %u stop "
        "in Havok at moving bodies or ones debris does not collide with, %u hit only in Havok, %u only in the mirror, %u "
        "hit nothing; %u static shapes in the host; %llu bodies so far came at the address of one that left",
        g_rays.size(), match, max_match, differ, max_differ, excluded, missing, extra, none, pxbridge::host_statics(),
        static_cast<unsigned long long>(g_reused_total));
    g_check_state = Check::Done;
}

void on_frame(int game_state) {
    if (!g_on) return;
    if (game_state != kGameplay) {
        if (!g_bodies.empty() && !rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
            // The level is gone: drop its mirror (after whatever of it still waits, which keeps its order).
            for (auto& [e, m] : g_bodies) {
                if (!m.group) continue;
                g_queue_urgent = false;
                remove_group(m.group);
            }
            g_bodies.clear();
            g_entity_of_group.clear();
            g_initial_done = false;
            g_export_bytes = 0;
            g_outbox_peak = g_outbox_bytes;
            g_host_has_world = false;
            g_check_state = Check::Idle;
            g_level_ms = 0;
            g_unbounded = 0;
            g_measured_logged = false;
            g_mirrored = 0;
            g_mirrored_statics = 0;
            g_missing = 0;
            g_piece_cols.clear();
            g_piece_cells = 0;
            g_next_pieces_ms = 0;
            g_level_whole = false;
            g_partial = 0;
            g_tiles.clear();
            g_tiles_dirty = true;
            g_requests.clear();
            g_ready_cache.clear();
        }
        flush_outbox();
        return;
    }
    if (!pxbridge::ready()) return;
    const double now = session::since_start_ms();
    if (now >= g_next_scan) {
        g_next_scan = now + kScanEveryMs;
        scan();
    }
    flush_outbox();
    if (changes_sent()) g_flushed_scans = g_complete_scans;
    if (g_check) {
        if (g_check_state == Check::Idle && g_initial_done && g_outbox.empty() && g_urgent.empty()) {
            g_check_state = Check::Waiting;
            g_check_at = now + 3000;  // the host cooks, then a step updates its query structure
        } else if (g_check_state == Check::Waiting && now >= g_check_at) {
            check_start();
        } else if (g_check_state == Check::Sent && pxbridge::query_done() == g_check_request) {
            check_finish();
        }
    }
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.exp_physx <= 0 || !cfg.exp_physx_world) return true;
    g_on = true;
    g_check = cfg.exp_physx_world_check != 0;
    g_hf_variant = cfg.exp_physx_world_hf;
    g_budget = uint32_t(std::max(cfg.exp_physx_world_statics, 0));
    g_near_m = float(cfg.exp_physx_world_near_m);
    session::add_frame_callback(&on_frame);
    LOG("world mirror armed: Havok's static world goes to the PhysX host during gameplay%s",
        g_check ? ", then is checked with 1,024 rays" : "");
    if (area())
        LOG("world mirror: a level of more than %u statics goes there in part: what is within %.0f m of the player, "
            "and of the other buildings the pieces near the debris and near where pieces wait to come over "
            "(PhysXWorldStatics, PhysXWorldNearM)", g_budget, double(g_near_m));
    return true;
}

// The host cooks the static world over several steps (the open world sends 2,000+ bodies, 11 MB of commands): debris
// waits until it has applied the export, so that the first pieces find their ground.
bool ready() {
    if (!g_on) return false;
    if (area()) return g_host_has_world;  // around the player (scan_area); each piece asks ready_at
    if (!g_initial_done || !g_outbox.empty()) return false;
    if (!g_host_has_world && pxbridge::ring_used() < (64u << 10)) {
        g_host_has_world = true;
        LOG("world mirror: the host has applied the static world, %.1f s after the export was queued",
            (session::since_start_ms() - g_export_done_ms) / 1000.0);
    }
    return g_host_has_world;
}

// Answers for the whole cell (kAskCellM) `pos` lies in, widened by `radius` rounded up to the next 8 m, from below
// everything up to that much above the cell, so that pieces near each other share one answer per frame. The area stays
// wanted while pieces ask for it (kRequestMs): it is asked for whether ready or not, so that it does not leave the
// host while pieces go over and are not counted yet. A building in part has the area once a scan took its cells
// (asks_done: the same key).
bool ready_at(const float pos[3], float radius) {
    if (!area()) return ready();
    if (!g_on || !g_host_has_world) return false;
    const int bucket = std::clamp(int(std::ceil(std::max(radius, 1.0f) / 8.0f)), 1, 15);
    const int cx = int(std::floor(pos[0] / kAskCellM)), cy = int(std::floor(pos[1] / kAskCellM)),
              cz = int(std::floor(pos[2] / kAskCellM));
    constexpr int kBias = 1 << 19;
    const uint64_t key = uint64_t(uint32_t(cx + kBias) & 0xFFFFFu) | (uint64_t(uint32_t(cy + kBias) & 0xFFFFFu) << 20) |
                         (uint64_t(uint32_t(cz + kBias) & 0xFFFFFu) << 40) | (uint64_t(bucket) << 60);
    const float r = float(bucket) * 8.0f;
    const float x0 = float(cx) * kAskCellM - r, x1 = float(cx + 1) * kAskCellM + r, z0 = float(cz) * kAskCellM - r,
                z1 = float(cz + 1) * kAskCellM + r, y1 = float(cy + 1) * kAskCellM + r;
    const uint32_t frame = pxbridge::frame();
    if (frame != g_ready_cache_frame) {
        g_ready_cache.clear();
        g_ready_cache_frame = frame;
    }
    const int64_t t0 = session::qpc();
    bool ok;
    const auto cached = g_ready_cache.find(key);
    if (cached != g_ready_cache.end()) {
        ok = cached->second;
    } else {
        ++g_log_misses;
        ok = g_unbounded == 0 && !g_tiles_dirty;
        const Request area{x0, z0, x1, z1, y1, 0};
        if (ok)
            for_bodies_near(x0, z0, x1, z1, [&](const uint8_t*, const Mirrored& m) {
                if (!overlaps_ask(m, area)) return true;
                if (!applied(m) || (m.partial && !m.asks_done.count(key))) ok = false;
                return ok;
            });
        g_ready_cache.emplace(key, ok);
    }
    g_requests[key] = {x0, z0, x1, z1, y1, session::since_start_ms() + kRequestMs};
    if (!ok) ++g_asked;
    ++g_log_asks;
    g_log_ask_ms += session::ticks_to_ms(session::qpc() - t0);
    return ok;
}

bool partial() { return g_on && area(); }

void check_again() {
    if (g_check && g_check_state == Check::Done) g_check_state = Check::Idle;
}

uint32_t flushed_scans() { return g_on ? g_flushed_scans : UINT32_MAX; }

bool catch_up() {
    if (!g_on) return true;
    if (!pxbridge::ready()) return false;
    const uint32_t complete = g_complete_scans;
    scan();
    flush_outbox();
    if (!changes_sent()) return false;
    g_flushed_scans = g_complete_scans;
    return g_complete_scans != complete;
}

uint8_t* object_of_group(uint32_t group) {
    const auto g = g_entity_of_group.find(group);
    if (g == g_entity_of_group.end()) return nullptr;
    const auto b = g_bodies.find(g->second);
    if (b == g_bodies.end() || b->second.group != group || b->second.object == 0xFFFFFFFFu) return nullptr;
    return rfg::fn<uint8_t*(__fastcall*)(void* RFG_EDX_TYPE, uint32_t)>(rfg::rva::world__object_from_handle)(
        rfg::addr(rfg::rva::World) RFG_EDX_ARG, b->second.object);  // null once the object is gone
}

FrameStats take_frame() {
    FrameStats f = g_stats;
    f.groups = g_mirrored;
    f.queued_kb = uint32_t(g_outbox_bytes >> 10);
    g_stats = {};
    return f;
}

}  // namespace world_mirror
