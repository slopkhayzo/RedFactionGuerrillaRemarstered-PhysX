#include "test/stream_report.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/config.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"

namespace stream_report {
namespace {

namespace r = rfg::rva;

constexpr int kGameplay = 1;  // GS_GAMEPLAY
// stream2_container: ref_count, entries, num_entries, flags (0x01 loaded, 0x10 loading, 0x40 failed), cdef, name,
// last_error (the text of its last error, 512 chars); stream2_container::entry (8 B): prim; stream2_prim: name, cpu and
// gpu sizes, cpu data (null until loaded), flags, ref counts.
constexpr uint32_t kContRefs = 0x08, kContEntries = 0x1C, kContNumEntries = 0x20, kContFlags = 0x22, kContName = 0x2C,
                   kContLastError = 0x30;
constexpr uint32_t kEntrySize = 8, kEntryPrim = 4;
constexpr uint32_t kPrimName = 0x00, kPrimCpuSize = 0x08, kPrimGpuSize = 0x0C, kPrimCpuData = 0x10, kPrimFlags = 0x21,
                   kPrimRefs = 0x24, kPrimContainerRefs = 0x26;
constexpr uint16_t kFailed = 0x40;
// object: pos, flags; object_mover (rfg_mover): its chunk's stream container (srid: the container itself) and
// rdd.m_loaded. world: all_objects.elt, subtype_objects[OT_SUB_MOVER_RFG] (elt, num).
constexpr uint32_t kObjPos = 0x04, kObjFlags = 0x54, kObjHandle = 0x6C, kObjSrid = 0x98, kMoverLoaded = 0x154;
constexpr uint32_t kWorldAllObjects = 0x64, kWorldRfgMovers = 0x3D4, kWorldRfgMoverCount = 0x3DC;
constexpr int kMaxPrimsListed = 40;

double g_at_s = 0;
int64_t g_since = 0;
bool g_done = false;

const char* text(const uint8_t* p, uint32_t off) {
    const char* s = *reinterpret_cast<const char* const*>(p + off);
    return s ? s : "(none)";
}

bool ends_with(const char* s, const char* tail) {
    const size_t a = std::strlen(s), b = std::strlen(tail);
    return a >= b && _stricmp(s + a - b, tail) == 0;
}

void report_container(const uint8_t* c, bool prims) {
    const uint16_t flags = *reinterpret_cast<const uint16_t*>(c + kContFlags);
    const int n = *reinterpret_cast<const short*>(c + kContNumEntries);
    const auto status = rfg::fn<int(__cdecl*)(uint32_t)>(r::stream2_get_status)(uint32_t(uintptr_t(c)));
    const char* err = *reinterpret_cast<const char* const*>(c + kContLastError);
    LOG("stream report: container %s: status %d, flags %04X, %d references, %d entries%s%s", text(c, kContName), status,
        unsigned(flags), int(*reinterpret_cast<const short*>(c + kContRefs)), n, err ? ", error: " : "",
        err ? err : "");
    if (!prims) return;
    const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(c + kContEntries);
    for (int i = 0; entries && i < n && i < kMaxPrimsListed; ++i) {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(entries + i * kEntrySize + kEntryPrim);
        if (!p) {
            LOG("stream report:   entry %d: no primitive", i);
            continue;
        }
        LOG("stream report:   %s: cpu %d B, gpu %d B, %s, flags %02X, %d references (%d containers)", text(p, kPrimName),
            *reinterpret_cast<const int*>(p + kPrimCpuSize), *reinterpret_cast<const int*>(p + kPrimGpuSize),
            *reinterpret_cast<const void* const*>(p + kPrimCpuData) ? "loaded" : "not loaded",
            unsigned(p[kPrimFlags]), int(*reinterpret_cast<const short*>(p + kPrimRefs)),
            int(*reinterpret_cast<const short*>(p + kPrimContainerRefs)));
    }
}

void report() {
    using Next = uint8_t*(__cdecl*)(uint8_t*);
    int all = 0, failed = 0, types = 0;
    for (uint8_t* c = rfg::fn<uint8_t*(__cdecl*)()>(r::stream2_get_first_container)(); c && all < 20000;
         c = rfg::fn<Next>(r::stream2_get_next_container)(c)) {
        ++all;
        const bool bad = (*reinterpret_cast<const uint16_t*>(c + kContFlags) & kFailed) != 0;
        const bool type = ends_with(text(c, kContName), ".rfgchunkx");
        const bool base = _stricmp(text(c, kContName), "ns_base") == 0;
        const bool waiting = (*reinterpret_cast<const uint16_t*>(c + kContFlags) & 0x03) == 0x02;   // queued, not loaded
        failed += bad;
        types += type;
        if (bad || type || base) report_container(c, bad || (type && waiting));
    }
    LOG("stream report: %d containers, %d failed, %d building type containers", all, failed, types);
    uint8_t* w = static_cast<uint8_t*>(rfg::addr(r::World));
    uint8_t* const* objs = *reinterpret_cast<uint8_t* const**>(w + kWorldAllObjects);
    const uint16_t* movers = *reinterpret_cast<const uint16_t* const*>(w + kWorldRfgMovers);
    const int n = *reinterpret_cast<const int*>(w + kWorldRfgMoverCount);
    for (int i = 0; objs && movers && i < n; ++i) {
        const uint8_t* o = objs[movers[i]];
        if (!o) continue;
        const float* p = reinterpret_cast<const float*>(o + kObjPos);
        const uint8_t* c = *reinterpret_cast<const uint8_t* const*>(o + kObjSrid);
        LOG("stream report: rfg_mover %08X at (%.1f, %.1f, %.1f): flags %02X %02X, chunk %s, container %s",
            *reinterpret_cast<const uint32_t*>(o + kObjHandle), double(p[0]), double(p[1]), double(p[2]),
            unsigned(o[kObjFlags]), unsigned(o[kObjFlags + 1]), o[kMoverLoaded] ? "loaded" : "not loaded",
            c ? text(c, kContName) : "(none)");
    }
}

// A failed container's allocator (stream2_container_def +0x14), what it needed and what it lists unloaded. A container
// takes one cpu and one gpu block for all its entries (stream2_container +0 / +4) from its type's allocator: a
// stream2_mempool_allocator (cpu / gpu static_mempool_base at +4 / +8: name +0xC, max_size +0x40, current_pos +0x48,
// peak +0x34) or a stream2_dynamic_allocator (cpu / gpu split_memmgr at +8 / +0xC: name +0x3C, mem_size +0x44,
// mem_used +0x48; frag_fail +0x10).
constexpr uint32_t kVtMempoolAllocator = 0x00EC68B4, kVtDynamicAllocator = 0x00EBD908;  // GOG RVAs (PDB)
void report_allocator(const uint8_t* c) {
    const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(c + kContEntries);
    const int n = *reinterpret_cast<const short*>(c + kContNumEntries);
    uint64_t cpu = 0, gpu = 0;
    int unloaded = 0;
    for (int i = 0; entries && i < n; ++i) {
        const uint8_t* p = *reinterpret_cast<const uint8_t* const*>(entries + i * kEntrySize + kEntryPrim);
        if (!p) continue;
        cpu += uint32_t(*reinterpret_cast<const int*>(p + kPrimCpuSize));
        gpu += uint32_t(*reinterpret_cast<const int*>(p + kPrimGpuSize));
        if (!*reinterpret_cast<const void* const*>(p + kPrimCpuData) && unloaded++ < 8)
            LOG("stream report:   not loaded: %s (cpu %d B, gpu %d B)", text(p, kPrimName),
                *reinterpret_cast<const int*>(p + kPrimCpuSize), *reinterpret_cast<const int*>(p + kPrimGpuSize));
    }
    const uint8_t* def = *reinterpret_cast<const uint8_t* const*>(c + 0x28);
    const uint8_t* alloc = def ? *reinterpret_cast<const uint8_t* const*>(def + 0x14) : nullptr;
    LOG("stream report:   it needs %.1f MB cpu, %.1f MB gpu in one block each (has %p / %p); %d entries not loaded; "
        "type %s, mode %d", double(cpu) / 1048576.0, double(gpu) / 1048576.0, *reinterpret_cast<void* const*>(c),
        *reinterpret_cast<void* const*>(c + 4), unloaded, def ? text(def, 0x08) : "?", def ? *reinterpret_cast<const int*>(def + 0x0C) : -1);
    if (!alloc) return;
    const uint32_t vt = uint32_t(*reinterpret_cast<const uintptr_t*>(alloc) - rfg::base());
    if (vt == kVtMempoolAllocator) {
        for (int k = 0; k < 2; ++k) {
            const uint8_t* pool = *reinterpret_cast<const uint8_t* const*>(alloc + 4 + 4 * k);
            if (!pool) continue;
            char name[33] = {};
            memcpy(name, pool + 0x0C, 32);
            LOG("stream report:   %s pool '%s': %.1f of %.1f MB used (peak %.1f)", k ? "gpu" : "cpu", name,
                *reinterpret_cast<const uint32_t*>(pool + 0x48) / 1048576.0,
                *reinterpret_cast<const uint32_t*>(pool + 0x40) / 1048576.0,
                *reinterpret_cast<const uint32_t*>(pool + 0x34) / 1048576.0);
        }
    } else if (vt == kVtDynamicAllocator) {
        for (int k = 0; k < 2; ++k) {
            const uint8_t* m = *reinterpret_cast<const uint8_t* const*>(alloc + 8 + 4 * k);
            if (!m) continue;
            LOG("stream report:   %s memory manager '%s': %.1f of %.1f MB used (requested %.1f); fragmentation failure %d",
                k ? "gpu" : "cpu", text(m, 0x3C), *reinterpret_cast<const uint32_t*>(m + 0x48) / 1048576.0,
                *reinterpret_cast<const uint32_t*>(m + 0x44) / 1048576.0,
                *reinterpret_cast<const uint32_t*>(m + 0x4C) / 1048576.0, int(alloc[0x10]));
        }
    } else {
        LOG("stream report:   allocator of another kind (vtable RVA %08X)", vt);
    }
}

// While a level loads: the containers' states when they change, every container that fails (with its error and what
// it lists) as it fails, and the few left unloaded near the end (B5b: the full avenue crashed on its first gameplay
// frame, in the time of day, after a load that logged nothing).
constexpr int kLoad = 2;  // GS_LOAD
std::vector<const uint8_t*> g_failed_seen;
int g_last_counts[4] = {-1, -1, -1, -1};

void watch_load() {
    using Next = uint8_t*(__cdecl*)(uint8_t*);
    int counts[4] = {};  // loaded, queued (not loaded), loading, failed
    std::vector<const uint8_t*> unloaded;
    for (uint8_t* c = rfg::fn<uint8_t*(__cdecl*)()>(r::stream2_get_first_container)(); c && counts[0] < 20000;
         c = rfg::fn<Next>(r::stream2_get_next_container)(c)) {
        const uint16_t flags = *reinterpret_cast<const uint16_t*>(c + kContFlags);
        if (flags & kFailed) {
            ++counts[3];
            if (std::find(g_failed_seen.begin(), g_failed_seen.end(), c) == g_failed_seen.end()) {
                g_failed_seen.push_back(c);
                report_container(c, false);
                report_allocator(c);
            }
        } else if (flags & 0x01) {
            ++counts[0];
        } else {
            ++counts[(flags & 0x10) ? 2 : 1];
            if (unloaded.size() < 12) unloaded.push_back(c);
        }
    }
    if (memcmp(counts, g_last_counts, sizeof(counts)) == 0) return;
    memcpy(g_last_counts, counts, sizeof(counts));
    LOG("stream report: loading: %d containers loaded, %d queued, %d loading, %d failed", counts[0], counts[1],
        counts[2], counts[3]);
    if (counts[1] + counts[2] <= 12)
        for (const uint8_t* c : unloaded) report_container(c, false);
}

void on_frame(int game_state) {
    if (game_state == kLoad) {
        watch_load();
        return;
    }
    if (g_done || game_state != kGameplay) return;
    const int64_t now = session::qpc();
    if (!g_since) g_since = now;
    if (session::ticks_to_ms(now - g_since) / 1000.0 < g_at_s) return;
    g_done = true;
    report();
}

}  // namespace

bool install(const Config& cfg) {
    if (cfg.test_stream_report <= 0) return true;
    g_at_s = double(cfg.test_stream_report);
    session::add_frame_callback(&on_frame);
    LOG("stream report armed: %.0f s into gameplay, the failed stream containers and the building types' ones",
        g_at_s);
    return true;
}

}  // namespace stream_report
