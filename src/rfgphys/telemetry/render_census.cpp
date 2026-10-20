#include "telemetry/render_census.h"

#include <atomic>
#include <cstring>

#include "game/rfg.h"
#include "hooks/hooks.h"

namespace render_census {
namespace {

// Render-thread counters (only the render thread touches them), published into the atomics once per render frame.
uint32_t t_uploads = 0, t_redundant = 0, t_bytes = 0, t_queries = 0, t_polls = 0;
std::atomic<uint32_t> g_uploads{0}, g_redundant{0}, g_bytes{0}, g_queries{0}, g_polls{0};

// Last upload per stage and constant-buffer slot.
struct Last {
    void* cb;
    uint32_t hash;
    uint32_t size;
};
constexpr int kSlots = 16;
Last g_last[2][kSlots];

bool g_redundancy = false;  // Telemetry.CbRedundancy: hash every upload (diagnostic, costs render-thread time)

// FNV-1a over 32-bit words (constant data is a whole number of float4s), then the tail bytes.
uint32_t hash_words(const void* data, uint32_t size) {
    uint32_t h = 2166136261u;
    const auto* p = static_cast<const uint8_t*>(data);
    uint32_t i = 0;
    for (; i + 4 <= size; i += 4) {
        uint32_t w;
        memcpy(&w, p + i, 4);
        h = (h ^ w) * 16777619u;
    }
    for (; i < size; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

void count_upload(int stage, uint32_t bit, uint32_t dirty, void* cb, uint32_t slot, const void* data, uint32_t size) {
    if (!(bit & dirty) || !cb || !data) return;  // the flush only uploads when the slot is dirty
    ++t_uploads;
    t_bytes += size;
    if (g_redundancy && slot < kSlots) {
        const uint32_t h = hash_words(data, size);
        Last& last = g_last[stage][slot];
        if (last.cb == cb && last.hash == h && last.size == size) ++t_redundant;
        last = Last{cb, h, size};
    }
}

// flushDynamic{Vertex,Fragment}ConstantBuffer(writer, slot bit, dirty mask, DynamicConstantBuffer*, slot, data, size)
using FlushFn = void(__cdecl*)(void*, uint32_t, uint32_t, void*, uint32_t, void*, uint32_t);
FlushFn o_flush_vs, o_flush_ps;
void __cdecl d_flush_vs(void* w, uint32_t bit, uint32_t dirty, void* cb, uint32_t slot, void* data, uint32_t size) {
    count_upload(0, bit, dirty, cb, slot, data, size);
    o_flush_vs(w, bit, dirty, cb, slot, data, size);
}
void __cdecl d_flush_ps(void* w, uint32_t bit, uint32_t dirty, void* cb, uint32_t slot, void* data, uint32_t size) {
    count_upload(1, bit, dirty, cb, slot, data, size);
    o_flush_ps(w, bit, dirty, cb, slot, data, size);
}

void(__cdecl* o_mesh_query)(void*, void*);
void __cdecl d_mesh_query(void* renderer, void* entry) {
    ++t_queries;
    o_mesh_query(renderer, entry);
}

bool(__fastcall* o_poll)(void*, void*, void*, uint32_t, uint64_t*);
bool __fastcall d_poll(void* manager, void* edx, void* buffer, uint32_t query, uint64_t* result) {
    ++t_polls;
    return o_poll(manager, edx, buffer, query, result);
}

}  // namespace

bool install(bool cb_redundancy) {
    g_redundancy = cb_redundancy;
    namespace r = rfg::rva;
    bool ok = hooks::create(r::flushDynamicVertexConstantBuffer, &d_flush_vs, &o_flush_vs, "flushDynamicVertexConstantBuffer");
    ok &= hooks::create(r::flushDynamicFragmentConstantBuffer, &d_flush_ps, &o_flush_ps,
                        "flushDynamicFragmentConstantBuffer");
    ok &= hooks::create(r::rl_gpu_vis_render_mesh_query, &d_mesh_query, &o_mesh_query, "rl_gpu_vis_render_mesh_query");
    ok &= hooks::create(r::keen__OcclusionQueryManager__pollOcclusionQueryResult, &d_poll, &o_poll,
                        "OcclusionQueryManager::pollOcclusionQueryResult");
    return ok;
}

void publish() {
    g_uploads.fetch_add(t_uploads, std::memory_order_relaxed);
    g_redundant.fetch_add(t_redundant, std::memory_order_relaxed);
    g_bytes.fetch_add(t_bytes, std::memory_order_relaxed);
    g_queries.fetch_add(t_queries, std::memory_order_relaxed);
    g_polls.fetch_add(t_polls, std::memory_order_relaxed);
    t_uploads = t_redundant = t_bytes = t_queries = t_polls = 0;
}

FrameCounts take() {
    return {g_uploads.exchange(0, std::memory_order_relaxed), g_redundant.exchange(0, std::memory_order_relaxed),
            g_bytes.exchange(0, std::memory_order_relaxed) / 1024, g_queries.exchange(0, std::memory_order_relaxed),
            g_polls.exchange(0, std::memory_order_relaxed)};
}

}  // namespace render_census
