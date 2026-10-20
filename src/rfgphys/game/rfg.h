#pragma once
#include <cstdint>

#include "game/rfg_symbols.generated.h"

// Access to rfg.exe code and data by RVA. The exe is ASLR-enabled, so everything is relative to its load address.
// The plugin knows two builds (kBuilds: GOG and Steam, compiled from the same source); every address in rfg::rva is
// the running build's once select_build() has run, at start-up before anything else uses them. So code takes
// addresses from rfg::rva when it runs, never into a constant or a static initializer.
namespace rfg {

uintptr_t base();
uint32_t image_size();  // SizeOfImage of the running rfg.exe

// The build running (index into kBuilds), -1 until select_build().
int build();
const char* build_name();
// Switches every address in rfg::rva to that build's.
void select_build(int index);
// A code RVA of the running build as the GOG build's, whose PDB names it (diagnostics); 0 if the GOG build has no
// such code.
uint32_t gog_rva(uint32_t rva);

inline void* addr(uint32_t rva) { return reinterpret_cast<void*>(base() + rva); }

template <typename T>
inline T& global(uint32_t rva) {
    return *reinterpret_cast<T*>(base() + rva);
}

template <typename Fn>
inline Fn fn(uint32_t rva) {
    return reinterpret_cast<Fn>(base() + rva);
}

// A place inside a game function, such as a patch site: the function's RVA in the running build plus an offset
// (the function's code is the same in every build).
struct CodeSite {
    const uint32_t& function;
    uint32_t offset;
    uint32_t rva() const { return function + offset; }
};

const char* game_state_name(int value);

// Typed entry points used outside hooks.
inline int gameseq_get_state() { return fn<int(__cdecl*)()>(rva::gameseq_get_state)(); }

}  // namespace rfg
