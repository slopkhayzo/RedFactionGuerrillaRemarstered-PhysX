#include "experiments/debris_collision.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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

// havok_memory_watch_dog_free_memory(world, island, count), from havok_sim_islands_process_post right after the step:
// an island of 64 or more bodies still needs more solver memory than Havok_mem_largest_runtime_block when split, and
// the game destroys `count` of its bodies (all but 64) by auto-remove level, building chunks as well as debris; with
// colliding debris a big shatter can make such an island, and most of it would vanish at once. Instead the solver gets
// a buffer the island fits in (between steps nothing is allocated from it), up to kMaxSolverBuffer; only an island
// needing more than that is left to the game's watchdog.
constexpr uint32_t kMaxSolverBuffer = 96u << 20;
constexpr uint32_t kSolverGrowStep = 1u << 20;
void(__cdecl* o_watch_dog)(void*, void*, int) = nullptr;
uint32_t g_islands_kept = 0, g_islands_cut = 0;

bool grow_solver_buffer(uint32_t need) {
    auto* system = rfg::global<uint8_t*>(rfg::rva::Havok_memory);
    if (!system || !o_set_buffer) return false;
    uint8_t* allocator = system + kSolverAllocator;
    auto* const* range = reinterpret_cast<uint8_t* const*>(allocator + 4);
    uint8_t* current = range[0];
    const uint32_t current_size = uint32_t(range[1] - range[0]);
    uint64_t size = std::max<uint64_t>(uint64_t(current_size) * 2, uint64_t(need) + need / 2 + kSolverMargin);
    size = (size + kSolverGrowStep - 1) & ~uint64_t(kSolverGrowStep - 1);
    if (size > kMaxSolverBuffer) size = kMaxSolverBuffer;
    if (uint64_t(need) + kSolverMargin > size || size <= current_size) return false;
    void* fresh = VirtualAlloc(nullptr, size_t(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!fresh) return false;
    o_set_buffer(allocator, nullptr, fresh, int(size));
    if (!g_game_solver_buffer) {  // the game's own: back to Havok at shutdown (d_main_quit)
        g_game_solver_buffer = current;
        g_game_solver_size = int(current_size);
    } else {
        VirtualFree(current, 0, MEM_RELEASE);
    }
    g_solver_size = uint32_t(size);
    rfg::global<uint32_t>(rfg::rva::Havok_mem_largest_runtime_block) = g_solver_size - kSolverMargin;
    return true;
}

void __cdecl d_watch_dog(void* world, void* island, int count) {
    const auto need = uint32_t(rfg::fn<int(__cdecl*)(void*)>(rfg::rva::hkpConstraintSolverSetup__calcBufferSize)(island));
    const int bodies = island ? *reinterpret_cast<const int*>(static_cast<uint8_t*>(island) + 0x50) : 0;  // m_entities
    if (grow_solver_buffer(need)) {
        ++g_islands_kept;
        LOG("experiment DebrisCollide: an island of %d bodies needs %u KB of solver memory: the buffer grows to %u KB, "
            "instead of the game destroying %d of them", bodies, need / 1024, g_solver_size / 1024, count);
        return;
    }
    ++g_islands_cut;
    LOG("experiment DebrisCollide: an island of %d bodies needs %u KB of solver memory, past %u KB: the game destroys %d "
        "of them", bodies, need / 1024, kMaxSolverBuffer / 1024, count);
    o_watch_dog(world, island, count);
}

// hkSolverBuildJacobianFromAtomsNotContact(atoms, size, in, out) sets up the solver for a constraint that is not a
// plain contact, atom by atom: it reads an atom's type and jumps to its case, and each case moves on past the atom. A
// type with no case (TYPE_OVERWRITE_PIVOT 27, TYPE_ELLIPTICAL_LIMIT 29, a bare TYPE_CONTACT 30, or past the last
// modifier type, 36) goes straight to the loop's end check without moving on, and the step never ends. The first
// sessions with Sledge's Sledgehammer Shockwave hung there twice, a few hundred ms after a blow shattered a tower into
// 2,700 pieces (hang report: one worker at the end check, the other threads waiting). Those paths now go to a stub that
// notes the atom and ends the constraint's atoms: the constraint keeps what was built so far (at least its header), the
// step ends, and the log reports the atom. Padding (TYPE_INVALID, 0) moves on to the next 16-byte boundary and is read
// again without the end check, so padding at a boundary (zeroed memory) loops as well; it goes to the same stub, and
// padding that reaches the end ends the loop.
const rfg::CodeSite kJacobianUnknownJa{rfg::rva::hkSolverBuildJacobianFromAtomsNotContact, 0xF4};  // ja (type > 36)
const rfg::CodeSite kJacobianDispatch{rfg::rva::hkSolverBuildJacobianFromAtomsNotContact, 0xFA};   // jmp [eax*4 + table]
const rfg::CodeSite kJacobianPadding{rfg::rva::hkSolverBuildJacobianFromAtomsNotContact, 0x101};  // case TYPE_INVALID
const rfg::CodeSite kJacobianRedispatch{rfg::rva::hkSolverBuildJacobianFromAtomsNotContact, 0x107};
const rfg::CodeSite kJacobianUnknownJmp{rfg::rva::hkSolverBuildJacobianFromAtomsNotContact, 0x10F};  // after padding
const rfg::CodeSite kJacobianEndCheck{rfg::rva::hkSolverBuildJacobianFromAtomsNotContact, 0x5F09};
constexpr uint32_t kAtomTypesInTable = 37;
constexpr uint32_t kAtomTypesWithoutCase[] = {27, 29, 30};

struct AtomSkip {  // written by the stub (any Havok thread)
    volatile LONG count;
    uint32_t type;
    const uint8_t* atom;
    const uint8_t* atoms;   // the constraint's atoms, from the function's arguments
    int32_t size;
    uint32_t atoms_bytes[4];  // the 16-byte blocks around each (aligned, so never across a page)
    uint32_t atom_bytes[4];
};
AtomSkip g_atom_skip{};
LONG g_atom_skips_logged = 0;
uint32_t g_atom_skip_logs = 0;

bool rel32_to(const uint8_t* insn, size_t opcode_size, const void* target) {
    int32_t rel;
    memcpy(&rel, insn + opcode_size, 4);
    return insn + opcode_size + 4 + rel == target;
}

void set_rel32(uint8_t* insn, size_t opcode_size, const void* target) {
    const int32_t rel =
        int32_t(reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(insn + opcode_size + 4));
    DWORD old;
    VirtualProtect(insn + opcode_size, 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy(insn + opcode_size, &rel, 4);
    VirtualProtect(insn + opcode_size, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), insn, opcode_size + 4);
}

bool guard_jacobian_atoms() {
    auto* ja = static_cast<uint8_t*>(rfg::addr(kJacobianUnknownJa.rva()));
    auto* dispatch = static_cast<uint8_t*>(rfg::addr(kJacobianDispatch.rva()));
    auto* padding = static_cast<uint8_t*>(rfg::addr(kJacobianPadding.rva()));
    auto* redispatch = static_cast<uint8_t*>(rfg::addr(kJacobianRedispatch.rva()));
    auto* jmp = static_cast<uint8_t*>(rfg::addr(kJacobianUnknownJmp.rva()));
    auto* end_check = static_cast<uint8_t*>(rfg::addr(kJacobianEndCheck.rva()));
    // fstp st(0); cmp ebx, [esp+0x13C] (the end of the atoms)
    static const uint8_t kEndCheck[] = {0xDD, 0xD8, 0x3B, 0x9C, 0x24, 0x3C, 0x01, 0x00, 0x00};
    static const uint8_t kDispatch[] = {0xFF, 0x24, 0x85};  // jmp dword ptr [eax*4 + table]
    // add ebx, 0xF; and ebx, -16; movzx eax, word ptr [ebx]; cmp eax, 0x24; jbe dispatch
    static const uint8_t kPadding[] = {0x83, 0xC3, 0x0F, 0x83, 0xE3, 0xF0, 0x0F, 0xB7,
                                       0x03, 0x83, 0xF8, 0x24, 0x76, 0xEB};
    if (memcmp(end_check, kEndCheck, sizeof(kEndCheck)) != 0 || ja[0] != 0x0F || ja[1] != 0x87 ||
        !rel32_to(ja, 2, end_check) || jmp[0] != 0xE9 || !rel32_to(jmp, 1, end_check) ||
        memcmp(dispatch, kDispatch, sizeof(kDispatch)) != 0 || memcmp(padding, kPadding, sizeof(kPadding)) != 0)
        return false;
    uint32_t* table;
    memcpy(&table, dispatch + sizeof(kDispatch), 4);
    for (uint32_t type : kAtomTypesWithoutCase)
        if (table[type] != reinterpret_cast<uint32_t>(end_check)) return false;
    if (table[0] != reinterpret_cast<uint32_t>(padding)) return false;
    const auto fn = reinterpret_cast<uint32_t>(rfg::addr(rfg::rva::hkSolverBuildJacobianFromAtomsNotContact));
    for (uint32_t type = 0; type < kAtomTypesInTable; ++type)  // the other cases lie in the function
        if (table[type] - fn >= rfg::size::hkSolverBuildJacobianFromAtomsNotContact) return false;

    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 256, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    uint8_t* p = stub;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    };
    auto put_u32 = [&](const volatile void* v) {
        const uint32_t a = reinterpret_cast<uint32_t>(v);
        memcpy(p, &a, 4);
        p += 4;
    };
    auto jump = [&](std::initializer_list<uint8_t> opcode, const void* target) {
        put(opcode);
        p += 4;
        set_rel32(p - 4 - opcode.size(), opcode.size(), target);
    };
    // The unknown atom: noted, then the atoms end.
    uint8_t* const unknown = p;
    put({0x50, 0x51, 0x56, 0x57});  // push eax, ecx, esi, edi
    put({0xF0, 0xFF, 0x05});        // lock inc dword ptr [count]
    put_u32(&g_atom_skip.count);
    put({0x0F, 0xB7, 0x03});  // movzx eax, word ptr [ebx]
    put({0xA3});              // mov [type], eax
    put_u32(&g_atom_skip.type);
    put({0x89, 0x1D});  // mov [atom], ebx
    put_u32(&g_atom_skip.atom);
    put({0x8B, 0x45, 0x08});  // mov eax, [ebp+8]: atoms
    put({0xA3});
    put_u32(&g_atom_skip.atoms);
    put({0x8B, 0x45, 0x0C});  // mov eax, [ebp+0xC]: size
    put({0xA3});
    put_u32(&g_atom_skip.size);
    put({0x8B, 0x75, 0x08, 0x83, 0xE6, 0xF0});  // mov esi, [ebp+8]; and esi, -16
    put({0xBF});                                // mov edi, atoms_bytes
    put_u32(g_atom_skip.atoms_bytes);
    put({0xB9, 0x04, 0x00, 0x00, 0x00, 0xF3, 0xA5});  // mov ecx, 4; rep movsd
    put({0x89, 0xDE, 0x83, 0xE6, 0xF0});              // mov esi, ebx; and esi, -16
    put({0xBF});                                      // mov edi, atom_bytes
    put_u32(g_atom_skip.atom_bytes);
    put({0xB9, 0x04, 0x00, 0x00, 0x00, 0xF3, 0xA5});  // mov ecx, 4; rep movsd
    put({0x5F, 0x5E, 0x59, 0x58});                    // pop edi, esi, ecx, eax
    put({0x8B, 0x9C, 0x24, 0x3C, 0x01, 0x00, 0x00});  // mov ebx, [esp+0x13C]: the end of the atoms
    jump({0xE9}, end_check);                          // jmp end_check (fstp st(0); the loop ends)
    // Padding: to the next 16-byte boundary as the game does, unless it is on one or reaches the end.
    uint8_t* const pad = p;
    put({0x50, 0x89, 0xD8});                          // push eax; mov eax, ebx
    put({0x83, 0xC3, 0x0F, 0x83, 0xE3, 0xF0});        // add ebx, 0xF; and ebx, -16
    put({0x39, 0xC3, 0x58});                          // cmp ebx, eax; pop eax
    jump({0x0F, 0x84}, unknown);                      // je unknown: on a boundary, nothing follows it
    put({0x3B, 0x9C, 0x24, 0x3C, 0x01, 0x00, 0x00});  // cmp ebx, [esp+0x13C]
    jump({0x0F, 0x83}, end_check);                    // jae end_check: the atoms end there
    jump({0xE9}, redispatch);                         // the next atom, as the game reads it
    FlushInstructionCache(GetCurrentProcess(), stub, size_t(p - stub));

    set_rel32(ja, 2, unknown);
    set_rel32(jmp, 1, unknown);
    auto set_case = [&](uint32_t type, const uint8_t* target) {
        DWORD old;
        VirtualProtect(&table[type], 4, PAGE_EXECUTE_READWRITE, &old);
        table[type] = reinterpret_cast<uint32_t>(target);
        VirtualProtect(&table[type], 4, old, &old);
    };
    for (uint32_t type : kAtomTypesWithoutCase) set_case(type, unknown);
    set_case(0, pad);
    return true;
}

// Test.SolverGuard: atom lists the solver setup has no way through go through it on a thread of their own (without the
// guard that thread would loop for good): a bare contact, a type past the last, padding on a 16-byte boundary, and
// padding followed by a type past the last.
DWORD WINAPI solver_guard_test(void* arg) {
    auto* came_back = static_cast<volatile LONG*>(arg);
    alignas(16) static uint8_t atoms[64], block[64], schemas[64], in[0x80];
    *reinterpret_cast<void**>(in + 0x38) = block;  // hkpConstraintQueryIn: the bodies' data the setup copies
    *reinterpret_cast<void**>(in + 0x3C) = block;
    void* out[2] = {};  // hkpConstraintQueryOut: where the schemas go
    const auto build = rfg::fn<void(__cdecl*)(const void*, int, void*, void*)>(
        rfg::rva::hkSolverBuildJacobianFromAtomsNotContact);
    struct Atom {
        uint32_t at;
        uint16_t type;
    };
    auto run = [&](uint32_t start, int size, std::initializer_list<Atom> types) {
        memset(atoms, 0, sizeof(atoms));
        for (const Atom& a : types) memcpy(atoms + a.at, &a.type, 2);
        out[0] = schemas;
        build(atoms + start, size, in, out);
        InterlockedIncrement(came_back);
    };
    run(0, 16, {{0, 30}});
    run(0, 16, {{0, 40}});
    run(0, 16, {{0, 0}});
    run(4, 28, {{4, 0}, {16, 40}});
    return 0;
}

void test_solver_guard() {
    static volatile LONG came_back = 0;
    const LONG skips = g_atom_skip.count;
    HANDLE thread = CreateThread(nullptr, 0, &solver_guard_test, const_cast<LONG*>(&came_back), 0, nullptr);
    if (!thread) return;
    const bool done = WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
    CloseHandle(thread);
    LOG("Test.SolverGuard: %s: %ld of 4 atom lists came back through the solver setup, %ld atoms skipped",
        done && came_back == 4 && g_atom_skip.count - skips == 4 ? "passed" : "FAILED", came_back,
        g_atom_skip.count - skips);
}

// Sim thread, every frame.
void log_atom_skips() {
    const LONG n = g_atom_skip.count;
    if (n == g_atom_skips_logged) return;
    if (g_atom_skip_logs < 10 || n / 1000 != g_atom_skips_logged / 1000) {
        ++g_atom_skip_logs;
        const uint32_t* a = g_atom_skip.atoms_bytes;
        const uint32_t* b = g_atom_skip.atom_bytes;
        LOG("Havok solver: the solver setup met a constraint atom it cannot get past (type %u) and would have looped on "
            "it for good; the rest of the constraint was skipped (%ld times so far). Atom %p in the constraint's atoms "
            "%p (%d bytes); there: %08X %08X %08X %08X, at the atom: %08X %08X %08X %08X",
            g_atom_skip.type, n, g_atom_skip.atom, g_atom_skip.atoms, g_atom_skip.size, a[0], a[1], a[2], a[3], b[0], b[1],
            b[2], b[3]);
    }
    g_atom_skips_logged = n;
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
    log_atom_skips();
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
    // Runs before the game's start-up code (InitializeASI), so havok_memory_init has not created the buffer yet. The
    // buffer is the game's 1 MB unless SolverBufferMB asks for more, and it grows for an island that needs it.
    if (cfg.exp_solver_buffer_mb > 1) g_solver_size = uint32_t(cfg.exp_solver_buffer_mb) << 20;
    if (!hooks::create(rfg::rva::hkSolverAllocator__setBuffer, &d_set_buffer, &o_set_buffer,
                       "hkSolverAllocator::setBuffer") ||
        !hooks::create(rfg::rva::hkFreeListMemorySystem__mainQuit, &d_main_quit, &o_main_quit,
                       "hkFreeListMemorySystem::mainQuit") ||
        !hooks::create(rfg::rva::havok_memory_watch_dog_free_island, &d_watch_dog, &o_watch_dog,
                       "havok_memory_watch_dog_free_memory(island)"))
        return false;
    if (guard_jacobian_atoms()) {
        LOG("experiment DebrisCollide: the solver setup ends a constraint at an atom it cannot get past (Havok would "
            "loop on it for good)");
        if (cfg.test_solver_guard) test_solver_guard();
    } else {
        LOG("experiment DebrisCollide: unexpected code in hkSolverBuildJacobianFromAtomsNotContact; its unknown atoms "
            "are not guarded");
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
