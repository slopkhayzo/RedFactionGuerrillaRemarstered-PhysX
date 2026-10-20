#include "core/game_lock.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace game_lock {
namespace {

std::atomic<CRITICAL_SECTION*> g_cs{nullptr};

}  // namespace

void set(void* critical_section) { g_cs.store(static_cast<CRITICAL_SECTION*>(critical_section)); }

bool enabled() { return g_cs.load(std::memory_order_relaxed) != nullptr; }

unsigned long owner_thread() {
    const CRITICAL_SECTION* cs = g_cs.load(std::memory_order_relaxed);
    return cs ? static_cast<unsigned long>(reinterpret_cast<uintptr_t>(cs->OwningThread)) : 0;
}

Guard::Guard() : cs_(g_cs.load(std::memory_order_acquire)) {
    if (cs_) EnterCriticalSection(static_cast<CRITICAL_SECTION*>(cs_));
}

Guard::~Guard() {
    if (cs_) LeaveCriticalSection(static_cast<CRITICAL_SECTION*>(cs_));
}

}  // namespace game_lock
