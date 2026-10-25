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
constexpr uint32_t kChunkTriangles = 16384;       // triangles per StaticMesh record
constexpr uint32_t kDestroyableExisting = 0x48;   // rfg_destroyable_shape::num_existing_child_shapes

bool g_on = false, g_check = false;
int g_hf_variant = 0;  // Experiment.PhysXWorldHf (diagnostic): bit 0 x-major heights, bit 1 the other cell split

// Only what debris collides with is mirrored: the body's layer must collide with a debris layer in the game's layer
// table (havok_game_filter: bit a * 128 + b), and system group 6 never collides with debris (01 §10.3). That leaves
// out triggers, invisible barriers and the like, which Havok's own debris never meets either.
constexpr uint32_t kFilterInfo = 0x2C;   // hkpEntity: m_collidable.m_broadPhaseHandle.m_collisionFilterInfo
constexpr uint32_t kLayerTable = 0x34;   // havok_game_filter::m_layer_lookup_table

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
constexpr uint32_t kEntityUid = 0xD0;  // hkpEntity::m_uid
struct Mirrored {
    uint32_t group;
    uint32_t signature;            // destroyables: 1 + existing pieces; 0: never changes
    bool seen;
    std::vector<uint32_t> keys;    // destroyables: the piece keys mirrored, sorted
    uint32_t uid;                  // hkpEntity::m_uid of the body mirrored
};
std::unordered_map<const uint8_t*, Mirrored> g_bodies;  // Havok entity -> its mirror
uint32_t g_next_group = 1;
std::deque<std::vector<uint8_t>> g_outbox;
size_t g_outbox_bytes = 0;
double g_next_scan = 0;
double g_next_change_log = 0;
uint32_t g_log_added = 0, g_log_changed = 0, g_log_removed = 0, g_log_reused = 0;
uint64_t g_reused_total = 0;
bool g_initial_done = false;                      // the first full export has been queued
bool g_host_has_world = false;                    // and the host has applied it
double g_export_done_ms = 0;
uint32_t g_unsupported[hs::kTypeCount + 1] = {};
FrameStats g_stats{};

void queue(std::vector<uint8_t>&& record) {
    g_outbox_bytes += record.size();
    g_outbox.push_back(std::move(record));
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
    for (size_t i = 0; i < keys.size(); i += 4096) {
        const uint32_t n = uint32_t(std::min<size_t>(4096, keys.size() - i));
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

// Turns one Havok body into StaticMesh, StaticConvex and StaticHeightField records. With `only`, just the convex
// pieces under those keys (pieces added back to a building).
struct Export : hs::Sink {
    uint32_t group;
    const std::unordered_set<uint32_t>* only = nullptr;
    std::vector<uint32_t>* keys_out = nullptr;
    std::vector<ipc::Vec3> verts;
    std::vector<uint32_t> idx;
    std::unordered_map<uint64_t, uint32_t> lookup;

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
        if (keys_out) keys_out->push_back(key);
    }

    // hkpSampledHeightFieldShape (storage: float, compressed: offset + scale * uint16): heights on an xRes x zRes
    // grid stored z-major (getHeightAtImpl: storage[z * xRes + x]), vertices at (x, h, z) * m_intToFloatScale in the
    // shape's space. PhysX wants rows along x, row-major, as int16 samples around a midpoint.
    void height_field(const uint8_t* s, const hs::Transform& t) override {
        if (only || !s) return;
        const int xr = *reinterpret_cast<const int*>(s + 0x28), zr = *reinterpret_cast<const int*>(s + 0x2C);
        const uint8_t kind = s[0x35];
        const float* scale = reinterpret_cast<const float*>(s + 0x40);
        const uint8_t* storage = *reinterpret_cast<const uint8_t* const*>(s + 0x80);
        if (xr < 2 || zr < 2 || xr > 4096 || zr > 4096 || !storage || kind > 1) {
            ++g_unsupported[hs::kSampledHeightField];
            return;
        }
        const float offset = *reinterpret_cast<const float*>(s + 0x90), hscale = *reinterpret_cast<const float*>(s + 0x94);
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
        c.flip = (s[0x8C] != 0 ? 0u : 1u) ^ ((g_hf_variant & 2) ? 1u : 0u);
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
}

uint8_t* world() { return rfg::global<uint8_t*>(rfg::rva::Havok_world); }

void scan() {
    uint8_t* w = world();
    if (!w) return;
    const uint8_t* island = *reinterpret_cast<uint8_t* const*>(w + 0x20);  // hkpWorld::m_fixedIsland
    if (!island) return;
    const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + 0x4C);
    const int n = *reinterpret_cast<const int*>(island + 0x50);
    const int64_t t0 = session::qpc();
    for (auto& [e, m] : g_bodies) m.seen = false;
    bool budget_left = true;
    uint32_t added = 0, changed = 0, removed = 0;
    uint32_t reused = 0;
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
            auto& m = g_bodies.emplace(e, Mirrored{g_next_group++, sig, true, {}, uid}).first->second;
            export_body(e, m);
            ++added;
        } else {
            continue;  // next scan
        }
        if (session::ticks_to_ms(session::qpc() - t0) > kScanBudgetMs) budget_left = false;
    }
    for (auto it = g_bodies.begin(); it != g_bodies.end();) {
        if (!it->second.seen) {
            remove_group(it->second.group);
            it = g_bodies.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    const float ms = float(session::ticks_to_ms(session::qpc() - t0));
    g_stats.scan_ms += ms;
    g_stats.changes += added + changed + removed;
    if (!g_initial_done && budget_left) {
        g_initial_done = true;
        g_export_done_ms = session::since_start_ms();
        uint32_t unsupported = 0;
        for (uint32_t c : g_unsupported) unsupported += c;
        LOG("world mirror: %zu Havok bodies exported (%.1f MB of commands queued)%s", g_bodies.size(),
            double(g_outbox_bytes) / (1024.0 * 1024.0), unsupported ? "; leaf shapes left out:" : "");
        for (int t = 0; t <= hs::kTypeCount; ++t)
            if (g_unsupported[t]) LOG("  %s: %u", hs::type_name(uint8_t(t)), g_unsupported[t]);
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

void flush_outbox() {
    while (!g_outbox.empty()) {
        const std::vector<uint8_t>& r = g_outbox.front();
        if (pxbridge::ring_free() < r.size()) break;
        if (!pxbridge::send_record(r.data(), uint32_t(r.size()))) break;
        g_stats.sent_kb += uint32_t(r.size() >> 10);
        g_outbox_bytes -= r.size();
        g_outbox.pop_front();
    }
}

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
    alignas(16) uint8_t in[0x30] = {};
    float* f = reinterpret_cast<float*>(in);
    f[0] = r.origin.x, f[1] = r.origin.y, f[2] = r.origin.z;
    f[3] = r.origin.x + r.direction.x * r.max_distance;
    f[4] = r.origin.y + r.direction.y * r.max_distance;
    f[5] = r.origin.z + r.direction.z * r.max_distance;
    alignas(16) uint8_t out[0x10] = {};
    rfg::fn<void(__fastcall*)(void*, void*)>(rfg::rva::raycast_output__raycast_output)(out, nullptr);
    const bool hit = rfg::fn<bool(__cdecl*)(void*, void*)>(fn_rva)(in, out);
    float d = -1.0f;
    const uint8_t* hits = *reinterpret_cast<const uint8_t* const*>(out);
    const int count = *reinterpret_cast<const int*>(out + 4);
    if (hit && hits && count > 0) {
        float best = 2.0f;
        int best_i = 0;
        for (int i = 0; i < count; ++i) {
            const float fr = *reinterpret_cast<const float*>(hits + i * 0x38 + 0x24);
            if (fr < best) best = fr, best_i = i;
        }
        if (best <= 1.0f) {
            d = best * r.max_distance;
            const uint32_t handle = *reinterpret_cast<const uint32_t*>(hits + best_i * 0x38 + 0x30);
            const uint32_t object = *reinterpret_cast<const uint32_t*>(hits + best_i * 0x38 + 0x28);
            if (what) {
                what->havok_handle = handle;
                if (object != 0xFFFFFFFFu) {
                    const auto from_handle = rfg::fn<uint8_t*(__fastcall*)(void*, void*, uint32_t)>(
                        rfg::rva::world__object_from_handle);
                    if (const uint8_t* obj = from_handle(rfg::addr(rfg::rva::World), nullptr, object))
                        what->object_type = int8_t(obj[0x7E]);
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
    rfg::fn<void(__fastcall*)(void*, void*)>(rfg::rva::raycast_output___raycast_output)(out, nullptr);
    return d;
}

void check_start() {
    uint8_t* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)();
    if (!player) return;
    const float* p = reinterpret_cast<const float*>(player + 0x04);
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
            // The level is gone: drop its mirror.
            for (auto& [e, m] : g_bodies) remove_group(m.group);
            g_bodies.clear();
            g_initial_done = false;
            g_host_has_world = false;
            g_check_state = Check::Idle;
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
    if (g_check) {
        if (g_check_state == Check::Idle && g_initial_done && g_outbox.empty()) {
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
    session::add_frame_callback(&on_frame);
    LOG("world mirror armed: Havok's static world goes to the PhysX host during gameplay%s",
        g_check ? ", then is checked with 1,024 rays" : "");
    return true;
}

// The host cooks the static world over several steps (the open world sends 2,000+ bodies, 11 MB of commands): debris
// waits until it has applied the export, so that the first pieces find their ground.
bool ready() {
    if (!g_on || !g_initial_done || !g_outbox.empty()) return false;
    if (!g_host_has_world && pxbridge::ring_used() < (64u << 10)) {
        g_host_has_world = true;
        LOG("world mirror: the host has applied the static world, %.1f s after the export was queued",
            (session::since_start_ms() - g_export_done_ms) / 1000.0);
    }
    return g_host_has_world;
}

void check_again() {
    if (g_check && g_check_state == Check::Done) g_check_state = Check::Idle;
}

FrameStats take_frame() {
    FrameStats f = g_stats;
    f.groups = uint32_t(g_bodies.size());
    f.queued_kb = uint32_t(g_outbox_bytes >> 10);
    g_stats = {};
    return f;
}

}  // namespace world_mirror
