#include "harness/checkpoint.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/rfg.h"
#include "harness/rec_file.h"
#include "harness/rng_guard.h"

namespace checkpoint {
namespace {

constexpr uint32_t kRandInstanceSize = 0x29C8;  // rand_instance: MT state, table index, 2048-entry table
constexpr uint32_t kRandIndex = 0x9C4;          // table index (vrand reads entry [index] and increments it)

// world: all_objects (base_array<object*>: vtable, elt, array_size, num) at +0x60.
constexpr uint32_t kWorldAllObjects = 0x60;
// object: pos (vector) at +0x04, orient (matrix 3x3) at +0x10, handle at +0x6C, obj_type / sub_type at +0x7E / +0x7F.
constexpr uint32_t kObjPos = 0x04, kObjOrient = 0x10, kObjHandle = 0x6C, kObjType = 0x7E;
constexpr uint8_t kTypeEffect = 4;  // OT_EFFECT
// hkpWorld active islands at +0x28 (hkArray<island*>), island entities at +0x4C; hkpEntity m_uid at +0xD0,
// m_motion at +0xE0: transform translation at motion+0x10+0x30, linear velocity at motion+0xD0.
constexpr uint32_t kWorldActiveIslands = 0x28, kIslandEntities = 0x4C;
constexpr uint32_t kEntityUid = 0xD0, kEntityTranslation = 0xE0 + 0x40, kEntityLinVel = 0xE0 + 0xD0;

struct HkPtrArray {
    void** data;
    int32_t size;
    int32_t capacity_and_flags;
};

uint32_t fnv(const void* data, size_t n, uint32_t h = 2166136261u) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Mixes one element's hash so that summing over elements is order-independent but still sensitive to each value.
uint32_t mix(uint32_t h) {
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}

}  // namespace

State capture(uint32_t frame, int game_state) {
    State s;
    s.frame = frame;
    s.state = game_state;

    // Generator state and table (the shared index is excluded: other threads swap theirs in) and every sim-thread
    // stream position. The sim thread's real CRT state is left out: only foliage placement uses it, reseeding it
    // per terrain cell, and how many cells a frame places follows streaming.
    const auto* rand_instance = static_cast<const uint8_t*>(rfg::addr(rfg::rva::Vlib_rand_instance));
    uint32_t h = fnv(rand_instance, kRandIndex);
    h = fnv(rand_instance + kRandIndex + 4, kRandInstanceSize - kRandIndex - 4, h);
    const uint32_t streams[2] = {rng_guard::vlib_hash(), rng_guard::crt_hash()};
    s.rng = fnv(streams, sizeof(streams), h);

    if (auto* player = rfg::fn<uint8_t*(__cdecl*)()>(rfg::rva::player_get_local_player)()) {
        s.player = fnv(player + kObjOrient, 36, fnv(player + kObjPos, 12));
        const float* pos = reinterpret_cast<const float*>(player + kObjPos);
        s.px = pos[0];
        s.py = pos[1];
        s.pz = pos[2];
    }

    auto* world = static_cast<uint8_t*>(rfg::addr(rfg::rva::World));
    auto** objects = *reinterpret_cast<uint8_t***>(world + kWorldAllObjects + 4);
    const int32_t n_objects = *reinterpret_cast<int32_t*>(world + kWorldAllObjects + 12);
    // Effect objects (sounds, particles, decals) are left out: whether one is created can follow audio voices and
    // rendering. Handles are left out too: they are allocated in sequence, so one extra effect shifts every later
    // object's handle. What remains is each object's type, subtype and position.
    uint32_t objects_hash = 0;
    for (int32_t i = 0; objects && i < n_objects; ++i) {
        const uint8_t* o = objects[i];
        if (!o || o[kObjType] == kTypeEffect) continue;
        objects_hash += mix(fnv(o + kObjPos, 12, fnv(o + kObjType, 2)));
        ++s.n_objects;
    }
    s.objects = objects_hash;

    if (auto* hk_world = rfg::global<uint8_t*>(rfg::rva::Havok_world)) {
        const auto& islands = *reinterpret_cast<HkPtrArray*>(hk_world + kWorldActiveIslands);
        uint32_t havok_hash = 0;
        for (int32_t i = 0; i < islands.size; ++i) {
            auto* island = static_cast<uint8_t*>(islands.data[i]);
            if (!island) continue;
            const auto& entities = *reinterpret_cast<HkPtrArray*>(island + kIslandEntities);
            for (int32_t e = 0; e < entities.size; ++e) {
                const auto* entity = static_cast<const uint8_t*>(entities.data[e]);
                if (!entity) continue;
                uint32_t eh = fnv(entity + kEntityUid, 4);
                eh = fnv(entity + kEntityTranslation, 12, eh);
                eh = fnv(entity + kEntityLinVel, 12, eh);
                havok_hash += mix(eh);
                ++s.n_active;
            }
        }
        s.havok = havok_hash;
    }
    s.movers_destroyed = rfg::global<int32_t>(rfg::rva::Movers_destroyed_count);
    return s;
}

void dump_objects(const std::wstring& path) {
    auto* world = static_cast<uint8_t*>(rfg::addr(rfg::rva::World));
    auto** objects = *reinterpret_cast<uint8_t***>(world + kWorldAllObjects + 4);
    const int32_t n = *reinterpret_cast<int32_t*>(world + kWorldAllObjects + 12);
    struct Row {
        uint32_t handle;
        int type, subtype;
        float pos[3];
    };
    std::vector<Row> rows;
    for (int32_t i = 0; objects && i < n; ++i) {
        const uint8_t* o = objects[i];
        if (!o) continue;
        Row r{};
        memcpy(&r.handle, o + kObjHandle, 4);
        r.type = int(int8_t(o[0x7E]));
        r.subtype = int(int8_t(o[0x7F]));
        memcpy(r.pos, o + kObjPos, 12);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.handle < b.handle; });
    rec::TextFile f;
    if (!f.open(path)) return;
    f.write("handle,type,subtype,x,y,z,x_bits,y_bits,z_bits\n");
    for (const Row& r : rows) {
        uint32_t bits[3];
        memcpy(bits, r.pos, 12);
        f.printf("%08X,%d,%d,%.5f,%.5f,%.5f,%08X,%08X,%08X\n", r.handle, r.type, r.subtype, r.pos[0], r.pos[1], r.pos[2],
                 bits[0], bits[1], bits[2]);
    }
    f.close();
}

void dump_bodies(const std::wstring& path) {
    auto* hk_world = rfg::global<uint8_t*>(rfg::rva::Havok_world);
    if (!hk_world) return;
    struct Row {
        uint32_t uid, layer;
        float pos[3], vel[3];
    };
    std::vector<Row> rows;
    const auto& islands = *reinterpret_cast<HkPtrArray*>(hk_world + kWorldActiveIslands);
    for (int32_t i = 0; i < islands.size; ++i) {
        auto* island = static_cast<uint8_t*>(islands.data[i]);
        if (!island) continue;
        const auto& entities = *reinterpret_cast<HkPtrArray*>(island + kIslandEntities);
        for (int32_t e = 0; e < entities.size; ++e) {
            const auto* entity = static_cast<const uint8_t*>(entities.data[e]);
            if (!entity) continue;
            Row r{};
            memcpy(&r.uid, entity + kEntityUid, 4);
            r.layer = *reinterpret_cast<const uint32_t*>(entity + 0x2C) & 0xFFFF;
            memcpy(r.pos, entity + kEntityTranslation, 12);
            memcpy(r.vel, entity + kEntityLinVel, 12);
            rows.push_back(r);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.uid < b.uid; });
    rec::TextFile f;
    if (!f.open(path)) return;
    f.write("uid,layer,x,y,z,vx,vy,vz,bits\n");
    for (const Row& r : rows) {
        uint32_t bits[6];
        memcpy(bits, r.pos, 12);
        memcpy(bits + 3, r.vel, 12);
        f.printf("%u,%u,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%08X%08X%08X%08X%08X%08X\n", r.uid, r.layer, r.pos[0], r.pos[1],
                 r.pos[2], r.vel[0], r.vel[1], r.vel[2], bits[0], bits[1], bits[2], bits[3], bits[4], bits[5]);
    }
    f.close();
}

const char* csv_header() {
    return "frame,state,rng,player,objects,havok,n_objects,n_active,movers_destroyed,player_x,player_y,player_z\n";
}

std::string to_csv(const State& s) {
    char line[256];
    snprintf(line, sizeof(line), "%u,%d,%08X,%08X,%08X,%08X,%d,%d,%d,%.4f,%.4f,%.4f\n", s.frame, s.state, s.rng,
             s.player, s.objects, s.havok, s.n_objects, s.n_active, s.movers_destroyed, s.px, s.py, s.pz);
    return line;
}

bool from_csv(const char* line, State& s) {
    return sscanf_s(line, "%u,%d,%x,%x,%x,%x,%d,%d,%d,%f,%f,%f", &s.frame, &s.state, &s.rng, &s.player, &s.objects,
                    &s.havok, &s.n_objects, &s.n_active, &s.movers_destroyed, &s.px, &s.py, &s.pz) == 12;
}

std::string diff(const State& a, const State& b, bool include_rng) {
    std::string out;
    auto add = [&](bool differs, const char* name) {
        if (!differs) return;
        if (!out.empty()) out += ',';
        out += name;
    };
    add(a.state != b.state, "state");
    if (include_rng) add(a.rng != b.rng, "rng");
    add(a.player != b.player, "player");
    add(a.objects != b.objects || a.n_objects != b.n_objects, "objects");
    add(a.havok != b.havok || a.n_active != b.n_active, "havok");
    add(a.movers_destroyed != b.movers_destroyed, "movers_destroyed");
    return out;
}

}  // namespace checkpoint
