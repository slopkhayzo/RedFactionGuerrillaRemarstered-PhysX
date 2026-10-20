#include "core/gameloop.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "game/rfg.h"
#include "hooks/hooks.h"

namespace gameloop {
namespace {

std::vector<void (*)()> g_begin;
std::vector<void (*)(int)> g_end;
float (*g_fixed_rate)() = nullptr;
void (*g_after_frametime)() = nullptr;
std::atomic<unsigned long> g_sim_tid{0};
std::atomic<unsigned long long> g_frames{0};
std::atomic<int> g_last_state{-1};

bool(__cdecl* o_do_frame)();

bool __cdecl d_do_frame() {
    if (!g_sim_tid.load(std::memory_order_relaxed)) {
        g_sim_tid = GetCurrentThreadId();
        LOG("main loop runs on thread %lu", GetCurrentThreadId());
    }
    for (auto fn : g_begin) fn();
    const bool result = o_do_frame();
    const int state = rfg::gameseq_get_state();
    g_last_state.store(state, std::memory_order_relaxed);
    g_frames.fetch_add(1, std::memory_order_release);
    for (auto fn : g_end) fn(state);
    return result;
}

// rfg_do_frame+0x91: call frametime_calculate(false, 0.f, 0.f). The call is redirected here; the function itself
// stays free for other hooks (telemetry times it).
constexpr uint32_t kFrametimeCallOffset = 0x91;

void __cdecl frametime_call(bool quantize, float quantum, float fixed_rate) {
    if (g_fixed_rate) fixed_rate = g_fixed_rate();
    rfg::fn<void(__cdecl*)(bool, float, float)>(rfg::rva::frametime_calculate)(quantize, quantum, fixed_rate);
    if (g_after_frametime) g_after_frametime();
}

bool patch_frametime_call() {
    auto* site = static_cast<uint8_t*>(rfg::addr(rfg::rva::rfg_do_frame + kFrametimeCallOffset));
    int32_t rel;
    memcpy(&rel, site + 1, 4);
    if (site[0] != 0xE8 || static_cast<void*>(site + 5 + rel) != rfg::addr(rfg::rva::frametime_calculate)) {
        LOG("gameloop: unexpected code at the frametime_calculate call in rfg_do_frame");
        return false;
    }
    const int32_t new_rel = int32_t(reinterpret_cast<uintptr_t>(&frametime_call) - reinterpret_cast<uintptr_t>(site + 5));
    DWORD old;
    if (!VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(site + 1, &new_rel, 4);
    VirtualProtect(site + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    return true;
}

}  // namespace

bool install() {
    bool ok = hooks::create(rfg::rva::rfg_do_frame, &d_do_frame, &o_do_frame, "rfg_do_frame");
    ok &= patch_frametime_call();
    return ok;
}

void add_begin(void (*fn)()) { g_begin.push_back(fn); }
void add_end(void (*fn)(int)) { g_end.push_back(fn); }

void set_frametime_control(float (*fixed_rate)(), void (*after)()) {
    g_fixed_rate = fixed_rate;
    g_after_frametime = after;
}

unsigned long sim_thread_id() { return g_sim_tid.load(std::memory_order_relaxed); }
bool on_sim_thread() { return GetCurrentThreadId() == g_sim_tid.load(std::memory_order_relaxed); }

unsigned long long frames_ended() { return g_frames.load(std::memory_order_acquire); }
int last_state() { return g_last_state.load(std::memory_order_relaxed); }

}  // namespace gameloop
