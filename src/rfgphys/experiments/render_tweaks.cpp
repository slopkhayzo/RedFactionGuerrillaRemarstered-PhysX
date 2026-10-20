#include "experiments/render_tweaks.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace experiments {
namespace {

constexpr uint32_t kClbPassIndex = 0x1D388;  // rl_visibility_manager: bit index of the near cascade; mid +1, far +2
float g_min_radius_far = 0, g_min_radius_mid = 0, g_min_radius_near = 0;

// rl_visibility_manager::bounding_volume_is_visible_for_passes(rl_bvolume*, ...): the mask of visibility passes
// (base pass, shadow cascades, ...) an instance's or a sector's world bounding volume is visible in. rl_bvolume
// starts with rl_sphere {position, radius}. Called from the visibility worker threads too: no state here.
uint32_t(__fastcall* o_bv_visible)(uint8_t*, void*, const float*, uint32_t, uint32_t, uint32_t, uint32_t);
uint32_t __fastcall d_bv_visible(uint8_t* vm, void* edx, const float* bv, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t mask = o_bv_visible(vm, edx, bv, a, b, c, d);
    if (mask && bv) {
        const uint32_t near_bit = *reinterpret_cast<const uint32_t*>(vm + kClbPassIndex);
        if (near_bit < 30) {
            const float radius = bv[3];
            if (radius < g_min_radius_far) mask &= ~(1u << (near_bit + 2));
            if (radius < g_min_radius_mid) mask &= ~(1u << (near_bit + 1));
            if (radius < g_min_radius_near) mask &= ~(1u << near_bit);
        }
    }
    return mask;
}

// ---- the list of instances a batched pass draws one by one ----

// rl_batched_pass's constructor points m_unbatched_renderables (+0x18) at the static Unbatched_renderable_buffer
// (4,096 pointers); rl_scene_renderer::render (inline) and add_unbatched_renderable_instance add to it up to 4,096
// (flag 0x200 past that) while the count (+0x14) keeps counting; finalize_construction clamps the count to 4,096.
constexpr uint32_t kGameCap = 0x1000;
// The immediate operands patched (function + offset of the operand).
namespace r = rfg::rva;
const rfg::CodeSite kCtorBufferImm{r::rl_batched_pass__rl_batched_pass, 0x2B};            // mov [esi+0x18], imm32
const rfg::CodeSite kSceneRenderCapImm{r::rl_scene_renderer__render, 0x7E1};              // cmp eax, 0x1000
const rfg::CodeSite kAddUnbatchedCapImm{r::rl_batched_pass__add_unbatched_renderable_instance, 0xE};
const rfg::CodeSite kFinalizeCmpImm{r::rl_batched_pass__finalize_construction, 0x34};     // cmp eax, 0x1000
const rfg::CodeSite kFinalizeMovImm{r::rl_batched_pass__finalize_construction, 0x3B};     // mov eax, 0x1000

bool code_u32(const rfg::CodeSite& site, uint32_t expected) {
    uint32_t v;
    memcpy(&v, rfg::addr(site.rva()), 4);
    return v == expected;
}

void write_u32(const rfg::CodeSite& site, uint32_t value) {
    void* p = rfg::addr(site.rva());
    DWORD old;
    VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(p, &value, 4);
    VirtualProtect(p, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 4);
}

void raise_unbatched_cap(int requested) {
    const uint32_t cap = uint32_t(requested > 65536 ? 65536 : requested);
    const uint32_t game_buffer = uint32_t(rfg::base() + r::Unbatched_renderable_buffer);
    if (!code_u32(kCtorBufferImm, game_buffer) || !code_u32(kSceneRenderCapImm, kGameCap) ||
        !code_u32(kAddUnbatchedCapImm, kGameCap) || !code_u32(kFinalizeCmpImm, kGameCap) ||
        !code_u32(kFinalizeMovImm, kGameCap)) {
        LOG("experiment UnbatchedCap: unexpected code at the patch sites; not applied");
        return;
    }
    void** buffer = static_cast<void**>(VirtualAlloc(nullptr, cap * sizeof(void*), MEM_COMMIT | MEM_RESERVE,
                                                     PAGE_READWRITE));
    if (!buffer) {
        LOG("experiment UnbatchedCap: no memory for %u entries", cap);
        return;
    }
    write_u32(kCtorBufferImm, uint32_t(reinterpret_cast<uintptr_t>(buffer)));
    write_u32(kSceneRenderCapImm, cap);
    write_u32(kAddUnbatchedCapImm, cap);
    write_u32(kFinalizeCmpImm, cap);
    write_u32(kFinalizeMovImm, cap);
    LOG("experiment UnbatchedCap: a batched pass draws up to %u instances one by one (the game: 4,096)", cap);
}

// ---- instances drawn batched ----

// rl_batched_pass's constructor points m_instances (+0x0C) at the static Batched_instance_buffer (16-byte entries);
// partition_batched_instances gives each material batch its range and drops the batches past 0x6000 (24,576,
// warning flag 4); begin_construction allocates the frame's instance stream (0x7627 = 30,247 entries of 0x34 bytes:
// transforms) and rl_renderer::initialize creates the GPU buffer the compiled groups copy them into (0x180000 bytes,
// one stream's worth). All grow together.
constexpr uint32_t kBatchedGameCap = 0x6000, kStreamGameCap = 0x7627, kStreamEntry = 0x34;
const rfg::CodeSite kCtorInstancesImm{r::rl_batched_pass__rl_batched_pass, 0x24};          // mov [esi+0xC], imm32
const rfg::CodeSite kPartitionCapImm{r::rl_batched_pass__partition_batched_instances, 0x7B};  // cmp ecx, 0x6000
const rfg::CodeSite kStreamCountImm1{r::rl_batched_pass__begin_construction, 0xB};   // mov eax, 0x7627 (allocation)
const rfg::CodeSite kStreamBytesImm{r::rl_batched_pass__begin_construction, 0x2E};   // mov [esi+0x14], 0x17FFEC
const rfg::CodeSite kStreamCountImm2{r::rl_batched_pass__begin_construction, 0x35};  // mov [esi+0xC], 0x7627
const rfg::CodeSite kGpuBufferImm{r::rl_renderer__initialize, 0x5B};                 // push 0x180000
// The compiled render groups take their draw calls (0x18 bytes each) from Call_pool, a linear block of 0x60000
// bytes (16,384 calls) made in rl_compiled_render_group_init at start-up; when it is full the rest of a group is not
// drawn (warning flag 0x800). It grows with the batched cap.
const rfg::CodeSite kCallPoolPushImm{r::rl_compiled_render_group_init, 0x10};  // push 0x60000
const rfg::CodeSite kCallPoolSizeImm{r::rl_compiled_render_group_init, 0x1A};  // mov [Call_pool_memBlock+4], 0x60000
constexpr uint32_t kCallPoolGameBytes = 0x60000, kCallBytes = 0x18;

void raise_batched_cap(int requested) {
    const uint32_t cap = uint32_t(requested > 262144 ? 262144 : requested);
    const uint32_t game_buffer = uint32_t(rfg::base() + r::Batched_instance_buffer);
    if (!code_u32(kCtorInstancesImm, game_buffer) || !code_u32(kPartitionCapImm, kBatchedGameCap) ||
        !code_u32(kStreamCountImm1, kStreamGameCap) || !code_u32(kStreamBytesImm, kStreamGameCap * kStreamEntry) ||
        !code_u32(kStreamCountImm2, kStreamGameCap) || !code_u32(kGpuBufferImm, 0x180000) ||
        !code_u32(kCallPoolPushImm, kCallPoolGameBytes) || !code_u32(kCallPoolSizeImm, kCallPoolGameBytes)) {
        LOG("experiment BatchedCap: unexpected code at the patch sites; not applied");
        return;
    }
    void* buffer = VirtualAlloc(nullptr, size_t(cap) * 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buffer) {
        LOG("experiment BatchedCap: no memory for %u entries", cap);
        return;
    }
    // The stream also takes the instances of other users of the pass (and keeps the game's slack over the cap).
    const uint32_t stream = cap + (kStreamGameCap - kBatchedGameCap);
    // Half again as much GPU buffer as one stream: other users (clone meshes) draw instanced from it too.
    const uint32_t gpu_bytes = ((stream * kStreamEntry * 3 / 2) + 0xFFFF) & ~0xFFFFu;
    write_u32(kCtorInstancesImm, uint32_t(reinterpret_cast<uintptr_t>(buffer)));
    write_u32(kPartitionCapImm, cap);
    write_u32(kStreamCountImm1, stream);
    write_u32(kStreamBytesImm, stream * kStreamEntry);
    write_u32(kStreamCountImm2, stream);
    write_u32(kGpuBufferImm, gpu_bytes);
    const uint32_t call_bytes = std::max(kCallPoolGameBytes, ((cap * kCallBytes) + 0xFFFF) & ~0xFFFFu);
    write_u32(kCallPoolPushImm, call_bytes);
    write_u32(kCallPoolSizeImm, call_bytes);
    LOG("experiment BatchedCap: up to %u instances drawn batched per frame (the game: 24,576); instance stream %u "
        "entries, GPU instancing buffer %u KB, %u draw calls (the game: 16,384)", cap, stream, gpu_bytes / 1024,
        call_bytes / kCallBytes);
}

// Telemetry: the most instances a pass asked to draw one by one this frame (before the clamp), and the most entries
// the scene renderer's deferred sort list was asked to hold (9,216 fit; rl_scene_renderer +0x724 counts on past
// that). rl_scene_renderer::render calls finalize_construction right after routing the visible instances, while
// Current_scene_renderer is set.
constexpr uint32_t kSceneSortCount = 0x724;
std::atomic<uint32_t> g_unbatched_peak{0}, g_sort_peak{0};
void atomic_max(std::atomic<uint32_t>& a, uint32_t v) {
    uint32_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v)) {
    }
}
// partition_batched_instances runs once the visibility pass has counted the instances of each material batch
// (rl_batched_material_info, 0x14 bytes: num_instances at +0x0C; m_num_batched_materials +0x24, the array +0x28).
std::atomic<uint32_t> g_batched_peak{0};
void(__fastcall* o_partition)(uint8_t*, void*) = nullptr;
void __fastcall d_partition(uint8_t* pass, void* edx) {
    const uint32_t n = std::min<uint32_t>(*reinterpret_cast<const uint32_t*>(pass + 0x24), 0x400);
    const uint8_t* infos = *reinterpret_cast<uint8_t* const*>(pass + 0x28);
    uint32_t total = 0;
    if (infos)
        for (uint32_t i = 0; i < n; ++i) total += *reinterpret_cast<const uint32_t*>(infos + i * 0x14 + 0x0C);
    atomic_max(g_batched_peak, total);
    o_partition(pass, edx);
}

// rl_renderer::begin_instancing_batch(GeometryInstancingBatch*, count, stride) returns null when the GPU instancing
// buffer cannot take the batch; the group is then not drawn, with no warning flag. Counted for telemetry.
std::atomic<uint32_t> g_instancing_failed{0};
void*(__fastcall* o_begin_instancing)(void*, void*, void*, uint32_t, uint32_t) = nullptr;
void* __fastcall d_begin_instancing(void* renderer, void* edx, void* batch, uint32_t count, uint32_t stride) {
    void* p = o_begin_instancing(renderer, edx, batch, count, stride);
    if (!p && count) g_instancing_failed.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void(__fastcall* o_finalize)(uint8_t*, void*, void*) = nullptr;
void __fastcall d_finalize(uint8_t* pass, void* edx, void* renderer) {
    atomic_max(g_unbatched_peak, *reinterpret_cast<const uint32_t*>(pass + 0x14));
    if (const uint8_t* scene = rfg::global<uint8_t*>(rfg::rva::Current_scene_renderer))
        atomic_max(g_sort_peak, *reinterpret_cast<const uint32_t*>(scene + kSceneSortCount));
    o_finalize(pass, edx, renderer);
}

}  // namespace

uint32_t take_unbatched_peak() { return g_unbatched_peak.exchange(0, std::memory_order_relaxed); }
uint32_t take_batched_peak() { return g_batched_peak.exchange(0, std::memory_order_relaxed); }
uint32_t take_sort_peak() { return g_sort_peak.exchange(0, std::memory_order_relaxed); }
uint32_t take_instancing_failed() { return g_instancing_failed.exchange(0, std::memory_order_relaxed); }

void install_render_tweaks(const Config& cfg) {
    if (cfg.exp_unbatched_cap > int(kGameCap)) raise_unbatched_cap(cfg.exp_unbatched_cap);
    if (cfg.exp_batched_cap > int(kBatchedGameCap)) raise_batched_cap(cfg.exp_batched_cap);
    if (cfg.telemetry && (!hooks::create(rfg::rva::rl_batched_pass__finalize_construction, &d_finalize, &o_finalize,
                                         "rl_batched_pass::finalize_construction") ||
                          !hooks::create(rfg::rva::rl_batched_pass__partition_batched_instances, &d_partition,
                                         &o_partition, "rl_batched_pass::partition_batched_instances") ||
                          !hooks::create(rfg::rva::rl_renderer__begin_instancing_batch, &d_begin_instancing,
                                         &o_begin_instancing, "rl_renderer::begin_instancing_batch")))
        LOG("render tweaks: a renderer-list telemetry hook failed");
    if (cfg.exp_gpu_occlusion == 0) {
        rfg::global<uint8_t>(rfg::rva::RL_gpu_vis_enabled) = 0;
        LOG("experiment GpuOcclusion=0: GPU occlusion queries off");
    }
    if (cfg.exp_shadow_min_radius_far_cm > 0 || cfg.exp_shadow_min_radius_mid_cm > 0 ||
        cfg.exp_shadow_min_radius_near_cm > 0) {
        g_min_radius_far = cfg.exp_shadow_min_radius_far_cm / 100.f;
        g_min_radius_mid = cfg.exp_shadow_min_radius_mid_cm / 100.f;
        g_min_radius_near = cfg.exp_shadow_min_radius_near_cm / 100.f;
        const bool ok = hooks::create(rfg::rva::rl_visibility_manager__bounding_volume_is_visible_for_passes,
                                      &d_bv_visible, &o_bv_visible, "bounding_volume_is_visible_for_passes");
        LOG("experiment ShadowMinRadius: objects under %.2f / %.2f / %.2f m cast no shadow into the far / mid / near "
            "cascade%s", g_min_radius_far, g_min_radius_mid, g_min_radius_near, ok ? "" : " (HOOK FAILED)");
    }
}

}  // namespace experiments
