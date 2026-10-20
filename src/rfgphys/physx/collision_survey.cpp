#include "physx/collision_survey.h"

#include <windows.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <share.h>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "physx/havok_shapes.h"

namespace collision_survey {
namespace {

namespace hs = havok_shapes;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
constexpr double kDelayMs = 5000;

struct Counter : hs::Sink {
    uint64_t n_triangles = 0, n_convex = 0, convex_vertices = 0, n_height_fields = 0, hf_samples = 0;
    uint32_t unsupported_count[hs::kTypeCount + 1] = {};
    float bmin[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, bmax[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    int hf_x = 0, hf_z = 0;

    void grow(const float p[3]) {
        for (int i = 0; i < 3; ++i) {
            bmin[i] = std::min(bmin[i], p[i]);
            bmax[i] = std::max(bmax[i], p[i]);
        }
    }
    void triangle(const float a[3], const float b[3], const float c[3]) override {
        ++n_triangles;
        grow(a);
        grow(b);
        grow(c);
    }
    void convex(const float (*v)[3], int n, float, uint32_t) override {
        ++n_convex;
        convex_vertices += uint64_t(n);
        for (int i = 0; i < n; ++i) grow(v[i]);
    }
    void height_field(const uint8_t* shape, const hs::Transform& t) override {
        ++n_height_fields;
        if (!shape) return;
        // hkpSampledHeightFieldShape: m_xRes, m_zRes; m_intToFloatScale (x, y, z).
        hf_x = *reinterpret_cast<const int*>(shape + 0x28);
        hf_z = *reinterpret_cast<const int*>(shape + 0x2C);
        hf_samples += uint64_t(hf_x) * uint64_t(hf_z);
        const float* scale = reinterpret_cast<const float*>(shape + 0x40);
        const float corners[2][3] = {{0, 0, 0}, {float(hf_x - 1) * scale[0], 0, float(hf_z - 1) * scale[2]}};
        for (const auto& c : corners) {
            float w[3];
            t.apply(c, w);
            grow(w);
        }
    }
    void unsupported(uint8_t type) override { ++unsupported_count[type < hs::kTypeCount ? type : hs::kTypeCount]; }
};

bool walk_guarded(const uint8_t* shape, const hs::Transform& t, Counter& c) {
    __try {
        return hs::walk(shape, t, c);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

bool g_done = false;
double g_gameplay_since = -1;
int g_level = 0;

void survey() {
    uint8_t* world = rfg::global<uint8_t*>(rfg::rva::Havok_world);
    if (!world) return;
    const uint8_t* island = *reinterpret_cast<uint8_t* const*>(world + 0x20);  // hkpWorld::m_fixedIsland
    if (!island) return;
    const uint8_t* const* entities = *reinterpret_cast<const uint8_t* const* const*>(island + 0x4C);
    const int n = *reinterpret_cast<const int*>(island + 0x50);
    ++g_level;
    wchar_t name[64];
    swprintf_s(name, L"\\collision_survey_%d.csv", g_level);
    FILE* f = _wfsopen((session::run_dir() + name).c_str(), L"w", _SH_DENYNO);
    if (f) fprintf(f, "body,root_type,motion,user_data,triangles,convex,convex_vertices,height_fields,hf_x,hf_z,"
                      "min_x,min_y,min_z,max_x,max_y,max_z,walk_ok,unsupported\n");

    Counter total;
    uint64_t by_root_tris[hs::kTypeCount + 1] = {}, by_root_convex[hs::kTypeCount + 1] = {};
    uint32_t by_root_bodies[hs::kTypeCount + 1] = {};
    int failed = 0;
    const int64_t t0 = session::qpc();
    for (int i = 0; i < n; ++i) {
        const uint8_t* e = entities[i];
        if (!e) continue;
        const uint8_t* shape = hs::body_shape(e);
        if (!shape) continue;
        Counter c;
        const bool ok = walk_guarded(shape, hs::body_transform(e), c);
        if (!ok) ++failed;
        const uint8_t root = hs::type_of(shape);
        const int k = root < hs::kTypeCount ? root : hs::kTypeCount;
        ++by_root_bodies[k];
        by_root_tris[k] += c.n_triangles;
        by_root_convex[k] += c.n_convex;
        total.n_triangles += c.n_triangles;
        total.n_convex += c.n_convex;
        total.convex_vertices += c.convex_vertices;
        total.n_height_fields += c.n_height_fields;
        total.hf_samples += c.hf_samples;
        std::string unsup;
        for (int t = 0; t <= hs::kTypeCount; ++t)
            if (c.unsupported_count[t]) {
                total.unsupported_count[t] += c.unsupported_count[t];
                char b[48];
                snprintf(b, sizeof(b), "%s%s:%u", unsup.empty() ? "" : " ", hs::type_name(uint8_t(t)), c.unsupported_count[t]);
                unsup += b;
            }
        if (f)
            fprintf(f, "%d,%s,%u,%08lX,%llu,%llu,%llu,%llu,%d,%d,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d,%s\n", i,
                    hs::type_name(root), unsigned(hs::body_motion_type(e)),
                    static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(e + 0xC)), c.n_triangles, c.n_convex,
                    c.convex_vertices, c.n_height_fields, c.hf_x, c.hf_z, c.bmin[0], c.bmin[1], c.bmin[2], c.bmax[0],
                    c.bmax[1], c.bmax[2], ok ? 1 : 0, unsup.c_str());
    }
    const double ms = session::ticks_to_ms(session::qpc() - t0);
    if (f) fclose(f);
    LOG("collision survey %d: %d fixed bodies walked in %.1f ms (%d with unreadable parts): %llu triangles, %llu convex "
        "pieces (%llu vertices), %llu height fields (%llu samples)",
        g_level, n, ms, failed, total.n_triangles, total.n_convex, total.convex_vertices, total.n_height_fields,
        total.hf_samples);
    for (int t = 0; t <= hs::kTypeCount; ++t)
        if (by_root_bodies[t])
            LOG("  root %-26s %5u bodies, %8llu triangles, %6llu convex pieces", hs::type_name(uint8_t(t)),
                by_root_bodies[t], by_root_tris[t], by_root_convex[t]);
    for (int t = 0; t <= hs::kTypeCount; ++t)
        if (total.unsupported_count[t])
            LOG("  unsupported leaf %s: %u", hs::type_name(uint8_t(t)), total.unsupported_count[t]);

    // The other islands, for context: bodies by motion type.
    uint32_t motions[9] = {};
    for (uint32_t off : {0x28u, 0x34u}) {  // m_activeSimulationIslands, m_inactiveSimulationIslands
        const uint8_t* const* islands = *reinterpret_cast<const uint8_t* const* const*>(world + off);
        const int ni = *reinterpret_cast<const int*>(world + off + 4);
        for (int i = 0; i < ni; ++i) {
            const uint8_t* isl = islands[i];
            const uint8_t* const* es = *reinterpret_cast<const uint8_t* const* const*>(isl + 0x4C);
            const int ne = *reinterpret_cast<const int*>(isl + 0x50);
            for (int j = 0; j < ne; ++j) ++motions[std::min<uint8_t>(hs::body_motion_type(es[j]), 8)];
        }
    }
    LOG("  other islands: %u dynamic, %u sphere-inertia, %u box-inertia, %u keyframed, %u fixed, %u thin-box, %u "
        "character bodies",
        motions[1], motions[2], motions[3], motions[4], motions[5], motions[6], motions[7]);
}

void on_frame(int game_state) {
    if (game_state != kGameplay) {
        if (!rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {  // level gone: survey the next one
            g_done = false;
            g_gameplay_since = -1;
        }
        return;
    }
    const double now = session::since_start_ms();
    if (g_gameplay_since < 0) g_gameplay_since = now;
    if (g_done || now - g_gameplay_since < kDelayMs) return;
    g_done = true;
    survey();
}

}  // namespace

bool install(const Config& cfg) {
    if (!cfg.exp_collision_survey) return true;
    session::add_frame_callback(&on_frame);
    LOG("collision survey armed: the static world is walked 5 s into each level");
    return true;
}

}  // namespace collision_survey
