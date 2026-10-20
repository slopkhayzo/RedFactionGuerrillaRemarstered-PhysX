#include "experiments/debris_collision.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <utility>

#include "core/config.h"
#include "core/contacts.h"
#include "core/log.h"
#include "core/session.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace experiments {
namespace {

// Island solver memory. Havok solves each simulation island in one block of its solver allocator, whose buffer the
// game creates with 1 MB (hkMemorySystem::FrameInfo in havok_memory_init). After every step
// havok_sim_islands_process_post checks the islands of 64 or more bodies: when hkpConstraintSolverSetup::calcBufferSize
// exceeds Havok_mem_largest_runtime_block (0xFD800, the buffer minus a margin) and splitting the island does not help,
// havok_memory_watch_dog_free_memory destroys its bodies by auto-remove level until at least (bodies - 64) are gone;
// every debris piece has the same level, so the whole pile goes in one frame. Debris never touches debris in the
// vanilla game, so its islands stay small; colliding debris merges into islands that need more (01 §10.4). The
// experiment gives the solver a larger buffer of its own and raises the limit to match. The game's 1 MB block stays
// allocated and goes back into the solver allocator before Havok frees it at shutdown.
constexpr uint32_t kGameSolverBuffer = 0x100000;
constexpr uint32_t kSolverMargin = kGameSolverBuffer - 0xFD800;  // the game's limit is its buffer minus this
constexpr uint32_t kSolverAllocator = 0x94;  // hkFreeListMemorySystem::m_solverAllocator (bufferStart +4, bufferEnd +8)
uint32_t g_solver_size = 0;  // requested size, 0: keep the game's buffer
void* g_game_solver_buffer = nullptr;
int g_game_solver_size = 0;
bool g_solver_checked = false;

void(__fastcall* o_set_buffer)(void*, void*, void*, int);
void*(__fastcall* o_main_quit)(void*, void*, void*, int);

// hkSolverAllocator::setBuffer: called once by hkFreeListMemorySystem::mainInit (from havok_memory_init) and once by
// mainQuit (null) at shutdown.
void __fastcall d_set_buffer(void* allocator, void* edx, void* buffer, int size) {
    if (buffer && size > 0 && !g_game_solver_buffer && g_solver_size > uint32_t(size)) {
        void* own = VirtualAlloc(nullptr, g_solver_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (own) {
            g_game_solver_buffer = buffer;
            g_game_solver_size = size;
            o_set_buffer(allocator, edx, own, int(g_solver_size));
            // havok_memory_init sets the limit after mainInit returns, and only ever raises it.
            rfg::global<uint32_t>(rfg::rva::Havok_mem_largest_runtime_block) = g_solver_size - kSolverMargin;
            LOG("experiment DebrisCollide: Havok solver buffer %u KB instead of %d KB, island limit %u KB",
                g_solver_size / 1024, size / 1024, (g_solver_size - kSolverMargin) / 1024);
            return;
        }
        LOG("experiment DebrisCollide: could not allocate a %u KB solver buffer, keeping the game's", g_solver_size / 1024);
    }
    o_set_buffer(allocator, edx, buffer, size);
}

// hkFreeListMemorySystem::mainQuit(hkResult* result, flags) frees the solver buffer it finds into Havok's pool.
void* __fastcall d_main_quit(void* system, void* edx, void* result, int flags) {
    if (g_game_solver_buffer) {
        o_set_buffer(static_cast<uint8_t*>(system) + kSolverAllocator, nullptr, g_game_solver_buffer,
                     g_game_solver_size);
        g_game_solver_buffer = nullptr;
    }
    return o_main_quit(system, edx, result, flags);
}

void check_solver_buffer() {
    g_solver_checked = true;
    auto* system = rfg::global<uint8_t*>(rfg::rva::Havok_memory);
    if (!system) return;
    auto* const* range = reinterpret_cast<uint8_t* const*>(system + kSolverAllocator + 4);
    LOG("experiment DebrisCollide: solver buffer %u KB, island limit (Havok_mem_largest_runtime_block) %u KB",
        unsigned((range[1] - range[0]) / 1024),
        rfg::global<uint32_t>(rfg::rva::Havok_mem_largest_runtime_block) / 1024);
}

constexpr int GS_GAMEPLAY = 1;
constexpr int kNumLayers = 128;
constexpr uint32_t kFilterLayerTable = 0x34;  // havok_game_filter::m_layer_lookup_table (bit a * 128 + b)
constexpr int kLayerUltraLite = 43, kLayerWorldOnly = 44;
constexpr int kDebrisLayers[] = {41, 42, 43, 44};  // RFG_NORMAL_FAST, RFG_LITE, RFG_ULTRA_LITE, RFG_WORLD_ONLY
// Layers whose only collision types are in system group 6 (attached projectile, personal shield, no collision, dead
// human, hit detection, world anchor, dynamic link, object collection only, invisible barrier). Layer 38 is shared
// with a group-0 type and is left alone.
constexpr int kGroup6OnlyLayers[] = {17, 18, 22, 37, 49, 54, 55, 59, 61};

// Their addresses, read when used (they are the running build's only after start-up).
const uint32_t* const kDebrisTypes[] = {&rfg::rva::COLLIDE_RFG_NORMAL_FAST, &rfg::rva::COLLIDE_RFG_LITE,
                                       &rfg::rva::COLLIDE_RFG_ULTRA_LITE, &rfg::rva::COLLIDE_RFG_WORLD_ONLY};
uint32_t g_original[4];
bool g_patched = false;
int g_last_state = -1000;

// The collision types are const globals in .rdata; every user reads them from memory (none is inlined).
void set_types(bool strip_group) {
    for (int i = 0; i < 4; ++i) {
        auto* p = static_cast<uint32_t*>(rfg::addr(*kDebrisTypes[i]));
        DWORD old;
        VirtualProtect(p, 4, PAGE_READWRITE, &old);
        *p = strip_group ? (g_original[i] & 0xFFFF) : g_original[i];
        VirtualProtect(p, 4, old, &old);
    }
    g_patched = strip_group;
}

bool get_bit(const uint32_t* table, int a, int b) {
    const uint32_t i = uint32_t(a * kNumLayers + b);
    return (table[i >> 5] >> (i & 31)) & 1u;
}

int set_pair(uint32_t* table, int a, int b, bool value) {
    int changed = 0;
    for (const auto [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
        const uint32_t i = uint32_t(x * kNumLayers + y);
        const uint32_t mask = 1u << (i & 31);
        if (bool(table[i >> 5] & mask) != value) {
            table[i >> 5] ^= mask;
            ++changed;
        }
    }
    return changed;
}

void apply_layers() {
    auto* filter = rfg::global<uint8_t*>(rfg::rva::Havok_filter);
    if (!filter) return;
    auto* table = *reinterpret_cast<uint32_t**>(filter + kFilterLayerTable);
    if (!table) return;
    int changed = 0;
    for (int layer = 0; layer < kNumLayers; ++layer) {
        if (layer == kLayerWorldOnly) continue;
        changed += set_pair(table, kLayerWorldOnly, layer, get_bit(table, kLayerUltraLite, layer));
    }
    for (int a : kDebrisLayers)
        for (int b : kDebrisLayers) changed += set_pair(table, a, b, true);
    for (int a : kDebrisLayers)
        for (int b : kGroup6OnlyLayers) changed += set_pair(table, a, b, false);
    LOG("experiment DebrisCollide: debris collision types in system group 0, layer table updated (%d bits changed)",
        changed);
    char row[160];
    int n = 0;
    for (int l : {2, 3, 6, 7, 9, 37, 38, 40, 41, 42, 43, 44, 61})
        n += snprintf(row + n, sizeof(row) - n, "%d:%d ", l, int(get_bit(table, kLayerWorldOnly, l)));
    LOG("  RFG_WORLD_ONLY row now (layer:collides): %s", row);
}

void on_frame(int state) {
    const bool multiplayer = rfg::global<uint8_t>(rfg::rva::Multiplayer) != 0;
    if (multiplayer == g_patched) {  // multiplayer levels get the game's own collision types
        set_types(!multiplayer);
        LOG("experiment DebrisCollide: %s", multiplayer ? "multiplayer, debris collision types restored"
                                                        : "debris collision types in system group 0 again");
    }
    if (state == GS_GAMEPLAY && g_last_state != GS_GAMEPLAY && !multiplayer) {
        apply_layers();
        if (!g_solver_checked) check_solver_buffer();
    }
    g_last_state = state;
}

}  // namespace

bool install_debris_collision(const Config& cfg) {
    if (!cfg.exp_debris_collide) return true;
    for (int i = 0; i < 4; ++i) g_original[i] = *static_cast<uint32_t*>(rfg::addr(*kDebrisTypes[i]));
    for (uint32_t v : g_original) {
        if ((v >> 16) != 6) {
            LOG("experiment DebrisCollide: unexpected collision type %08X, not applied", v);
            return false;
        }
    }
    set_types(true);
    session::add_frame_callback(&on_frame);
    // Runs before the game's start-up code (InitializeASI), so havok_memory_init has not created the buffer yet.
    if (cfg.exp_solver_buffer_mb > 1) {
        g_solver_size = uint32_t(cfg.exp_solver_buffer_mb) << 20;
        if (!hooks::create(rfg::rva::hkSolverAllocator__setBuffer, &d_set_buffer, &o_set_buffer,
                           "hkSolverAllocator::setBuffer") ||
            !hooks::create(rfg::rva::hkFreeListMemorySystem__mainQuit, &d_main_quit, &o_main_quit,
                           "hkFreeListMemorySystem::mainQuit"))
            return false;
    }
    // Mode 1: debris-debris contacts bypass the game's listener, so impacts between pieces never become damage.
    const bool skip_impacts = cfg.exp_debris_collide == 1;
    if (skip_impacts) {
        if (!contacts::install()) return false;
        contacts::set_skip_debris_pairs(true);
    }
    LOG("experiment DebrisCollide=%d enabled: collision types %05X %05X %05X %05X -> system group 0; debris-debris "
        "impacts %s",
        cfg.exp_debris_collide, g_original[0], g_original[1], g_original[2], g_original[3],
        skip_impacts ? "skip the game's contact logic (no damage)" : "go through the game's contact logic (damage)");
    return true;
}

}  // namespace experiments
