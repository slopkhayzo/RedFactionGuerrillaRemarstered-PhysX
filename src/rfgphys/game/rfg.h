#pragma once
#include <cstdint>

#include "game/rfg_symbols.generated.h"

// Access to rfg.exe code and data by RVA. The exe is ASLR-enabled, so everything is relative to its load address.
namespace rfg {

uintptr_t base();

inline void* addr(uint32_t rva) { return reinterpret_cast<void*>(base() + rva); }

template <typename T>
inline T& global(uint32_t rva) {
    return *reinterpret_cast<T*>(base() + rva);
}

template <typename Fn>
inline Fn fn(uint32_t rva) {
    return reinterpret_cast<Fn>(base() + rva);
}

const char* game_state_name(int value);

// Typed entry points used outside hooks.
inline int gameseq_get_state() { return fn<int(__cdecl*)()>(rva::gameseq_get_state)(); }

}  // namespace rfg
