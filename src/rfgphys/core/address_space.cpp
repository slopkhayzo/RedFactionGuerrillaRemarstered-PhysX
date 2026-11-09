#include "core/address_space.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cwchar>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/session.h"

namespace address_space {
namespace {

constexpr DWORD kEveryMs = 5000;  // a walk takes about a millisecond, on its own thread
constexpr uint32_t kStepMb = 32;  // a new low this far under the last one logged goes to the log
// Below these the game is close to failing: the user's sessions that crashed or stopped drawing (2026-11-07/09) ran
// out at load with 260,000 body slots, and later in long sessions with 125,000-150,000.
constexpr uint32_t kLowFreeMb = 160, kLowLargestMb = 48;
constexpr int kMainMenu = 0, kGameplay = 1, kLoad = 2, kShutdown = 56, kWcShutdown = 60;

std::atomic<uint32_t> g_free{0}, g_largest{0}, g_seq{0};
std::atomic<int> g_census_wanted{0};  // 1: at the main menu, 2: in the first level's first seconds

const wchar_t* base_name(const wchar_t* path) {
    const wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

// What holds the address space, once at the main menu and once in the first level: the totals by kind and the
// biggest allocations (images by module, mapped views by file). The same settings left 404 MB free at the main menu
// on 2026-11-02 and 176 MB on 2026-11-09, with the same modules loaded; this is to see where such a difference goes.
void census(const char* when) {
    struct Alloc {
        uintptr_t base;
        uint64_t reserved, committed;
        DWORD type;
    };
    std::vector<Alloc> allocs;
    uint64_t total[3][2] = {};  // image, mapped, private x reserved, committed
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        if (mbi.State != MEM_FREE) {
            const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            if (allocs.empty() || allocs.back().base != base) allocs.push_back({base, 0, 0, mbi.Type});
            allocs.back().reserved += mbi.RegionSize;
            const int kind = mbi.Type == MEM_IMAGE ? 0 : mbi.Type == MEM_MAPPED ? 1 : 2;
            total[kind][0] += mbi.RegionSize;
            if (mbi.State == MEM_COMMIT) {
                allocs.back().committed += mbi.RegionSize;
                total[kind][1] += mbi.RegionSize;
            }
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    LOG("address space %s, MB in use (committed): images %.0f (%.0f), mapped views %.0f (%.0f), private %.0f (%.0f), in "
        "%zu allocations", when, total[0][0] / 1048576.0, total[0][1] / 1048576.0, total[1][0] / 1048576.0,
        total[1][1] / 1048576.0, total[2][0] / 1048576.0, total[2][1] / 1048576.0, allocs.size());
    std::sort(allocs.begin(), allocs.end(), [](const Alloc& a, const Alloc& b) { return a.reserved > b.reserved; });
    std::string line;
    for (size_t i = 0; i < allocs.size() && i < 16; ++i) {
        const Alloc& a = allocs[i];
        wchar_t path[MAX_PATH] = L"";
        if (a.type == MEM_IMAGE)
            GetModuleFileNameW(reinterpret_cast<HMODULE>(a.base), path, MAX_PATH);
        else if (a.type == MEM_MAPPED)
            GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(a.base), path, MAX_PATH);
        char item[200];
        snprintf(item, sizeof(item), "%s%08X %s%s%ls %.1f MB (%.1f)", line.empty() ? "" : "; ", unsigned(a.base),
                 a.type == MEM_IMAGE ? "image" : a.type == MEM_MAPPED ? "mapped" : "private", path[0] ? " " : "",
                 base_name(path), a.reserved / 1048576.0, a.committed / 1048576.0);
        line += item;
        if (i % 4 == 3) {
            LOG("address space %s, the biggest allocations: %s", when, line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) LOG("address space %s, the biggest allocations: %s", when, line.c_str());
    // NVIDIA's driver maps its shader cache file for the game into the game whole, and the file grows in steps of 16x:
    // past 16 MB of shaders it is 256 MB (on 2026-11-09 the user's game had lost 240 MB that way since 10-28).
    static bool told = false;
    for (const Alloc& a : allocs) {
        if (told || a.type != MEM_MAPPED || a.reserved < (uint64_t(64) << 20)) continue;
        wchar_t path[MAX_PATH] = L"";
        GetMappedFileNameW(GetCurrentProcess(), reinterpret_cast<void*>(a.base), path, MAX_PATH);
        const size_t n = wcslen(path);
        if (n < 5 || _wcsicmp(path + n - 5, L".nvph") != 0) continue;
        told = true;
        LOG("address space: the NVIDIA driver's shader cache for the game (%ls) takes %.0f MB of it; Shader Cache Size "
            "set to Disabled for rfg.exe (NVIDIA Control Panel, Manage 3D settings, Program Settings) should give it back",
            base_name(path), a.reserved / 1048576.0);
    }
}

DWORD WINAPI watch(void*) {
    for (;;) {
        if (const int c = g_census_wanted.exchange(0)) census(c == 1 ? "at the main menu" : "in the first level");
        const Measure m = measure();
        g_free.store(m.free_mb, std::memory_order_relaxed);
        g_largest.store(m.largest_mb, std::memory_order_relaxed);
        g_seq.fetch_add(1, std::memory_order_release);
        Sleep(kEveryMs);
    }
}

uint32_t g_seen = 0;
bool g_menu_logged = false, g_in_level = false, g_warned = false;
uint32_t g_level_samples = 0, g_logged_free = 0, g_low_free = UINT_MAX, g_low_largest = UINT_MAX;

void end_level() {
    if (g_level_samples)
        LOG("address space this level: at least %u MB free, the largest free block at least %u MB", g_low_free,
            g_low_largest);
    g_in_level = g_warned = false;
    g_level_samples = 0;
    g_low_free = g_low_largest = UINT_MAX;
}

void on_frame(int game_state) {
    if (g_in_level && (game_state == kMainMenu || game_state == kLoad || game_state == kShutdown ||
                       game_state == kWcShutdown))
        end_level();
    const uint32_t seq = g_seq.load(std::memory_order_acquire);
    if (seq == g_seen) return;
    g_seen = seq;
    const uint32_t free_mb = g_free.load(std::memory_order_relaxed);
    const uint32_t largest = g_largest.load(std::memory_order_relaxed);
    if (game_state == kMainMenu && !g_menu_logged) {
        g_menu_logged = true;
        LOG("address space at the main menu: %u MB free, the largest free block %u MB", free_mb, largest);
        g_census_wanted.store(1);
    }
    if (game_state != kGameplay) return;
    g_in_level = true;
    g_low_free = std::min(g_low_free, free_mb);
    g_low_largest = std::min(g_low_largest, largest);
    if (g_level_samples++ == 0) {
        LOG("address space in the level's first seconds: %u MB free, the largest free block %u MB", free_mb, largest);
        g_logged_free = free_mb;
        static bool first_level = true;
        if (first_level) g_census_wanted.store(2);
        first_level = false;
    } else if (free_mb + kStepMb <= g_logged_free) {
        LOG("address space: %u MB free, the largest free block %u MB", free_mb, largest);
        g_logged_free = free_mb;
    }
    if (!g_warned && (free_mb < kLowFreeMb || largest < kLowLargestMb)) {
        g_warned = true;
        LOG("address space is running low (%u MB free, the largest free block %u MB): the game may stop drawing or "
            "crash. Experiment.PhysXMaxBodies costs about 0.4 KB of it a slot, Experiment.HavokHeapMB its megabytes",
            free_mb, largest);
    }
}

void on_exit(bool) {
    if (g_in_level) end_level();
}

}  // namespace

Measure measure() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    uint64_t free_total = 0, largest = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (p < end && VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) {
        if (mbi.State == MEM_FREE) {
            free_total += mbi.RegionSize;
            largest = std::max<uint64_t>(largest, mbi.RegionSize);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    return Measure{uint32_t(free_total >> 20), uint32_t(largest >> 20)};
}

void install(const Config&) {
    if (HANDLE t = CreateThread(nullptr, 64 << 10, &watch, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr)) {
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
    session::add_frame_callback(&on_frame);
    session::add_flush_handler(&on_exit);
}

}  // namespace address_space
