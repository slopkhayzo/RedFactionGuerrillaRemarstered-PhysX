#include "experiments/big_buildings.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/config.h"
#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace experiments {
namespace {

namespace r = rfg::rva;

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

// ---- many sub-instances handed back at once ----

// release_sub_instances_internal moves the sub-instances it hands back in batches of up to 4,096 (32 B each: 128 KB)
// through a scratch block from the thread's scratch mempool, and writes into the null it gets when the pool cannot
// give that much. Batches of 512 need 16 KB. The loop steps by its batch size in three more places (the sub-instance
// pointer by 4,096 x 32 B, the count left and the count done by 4,096), so all five constants change together: with
// the cap alone each batch skipped 3,584 sub-instances and the renderer then read garbage (B5a's first try).
constexpr uint32_t kReleaseGameBatch = 0x1000, kReleaseBatch = 0x200, kSubInstanceSize = 32;
struct Imm {
    rfg::CodeSite site;
    uint32_t game, ours;
};
const Imm kReleaseImms[] = {
    {{r::rl_destroyable_model_instance__release_sub_instances_internal, 0x8A}, kReleaseGameBatch, kReleaseBatch},
    {{r::rl_destroyable_model_instance__release_sub_instances_internal, 0x91}, kReleaseGameBatch, kReleaseBatch},
    {{r::rl_destroyable_model_instance__release_sub_instances_internal, 0x18E}, kReleaseGameBatch * kSubInstanceSize,
     kReleaseBatch * kSubInstanceSize},                                                    // add [esp+0x40], imm
    {{r::rl_destroyable_model_instance__release_sub_instances_internal, 0x196}, kReleaseGameBatch, kReleaseBatch},
    {{r::rl_destroyable_model_instance__release_sub_instances_internal, 0x19F}, kReleaseGameBatch, kReleaseBatch},
};

void smaller_release_batches() {
    for (const Imm& i : kReleaseImms)
        if (!code_u32(i.site, i.game)) {
            LOG("big buildings: unexpected code where sub-instances are released in batches; left as it is");
            return;
        }
    for (const Imm& i : kReleaseImms) write_u32(i.site, i.ours);
    LOG("big buildings: sub-instances are released in batches of %u (the game: 4,096, through a 128 KB scratch "
        "block)", kReleaseBatch);
}

// ---- the sim thread's scratch pool ----

// sim_scratch_system_init (once, when the sim thread starts): the game cuts 128 KB out of RFG_scratch_pool for
// RFG_sim_scratch_pool and makes it the thread's scratch mempool. Here the pool gets a block of its own.
constexpr uint32_t kSimScratchBytes = 2u << 20;
void(__cdecl* o_sim_scratch_init)() = nullptr;
void __cdecl d_sim_scratch_init() {
    void* block = VirtualAlloc(nullptr, kSimScratchBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!block) {
        LOG("big buildings: no memory for the sim thread's scratch pool; the game's 128 KB stay");
        o_sim_scratch_init();
        return;
    }
    void* pool = rfg::addr(r::RFG_sim_scratch_pool);
    using Init = void(__thiscall*)(void*, void*, uint32_t, const char*, uint32_t, uint8_t);
    rfg::fn<Init>(r::mempool__init)(pool, block, kSimScratchBytes, "RFG sim scratch (rfgphys)", 4, 0);
    rfg::fn<void(__cdecl*)(void*)>(r::memory_scratch_thread_init)(pool);
    LOG("big buildings: the sim thread's scratch pool has %u KB (the game: 128)", kSimScratchBytes >> 10);
}

}  // namespace

void install_big_buildings(const Config& cfg) {
    (void)cfg;
    smaller_release_batches();
    if (!hooks::create(r::sim_scratch_system_init, &d_sim_scratch_init, &o_sim_scratch_init, "sim_scratch_system_init"))
        LOG("big buildings: the sim scratch hook failed; the game's 128 KB stay");
}

}  // namespace experiments
