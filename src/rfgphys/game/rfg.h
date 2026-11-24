#pragma once
#include <cstdint>

#include "game/rfg_symbols.generated.h"

namespace openrfg64 {
struct Name;
}

// Access to rfg.exe code and data by RVA. The exe is ASLR-enabled, so everything is relative to its load address.
// The plugin knows two builds (kBuilds: GOG and Steam, compiled from the same source); every address in rfg::rva is
// the running build's once select_build() has run, at start-up before anything else uses them. So code takes
// addresses from rfg::rva when it runs, never into a constant or a static initializer.
namespace rfg {

uintptr_t base();
uint32_t image_size();  // SizeOfImage of the running rfg.exe

// OpenRFG64: OpenRFG64 (a 64-bit OpenRFG) as one more build, after kBuilds; its values come from its PDB.
inline constexpr int kBuildOpenRFG64 = kBuildCount;
inline bool is_openrfg64();

// The build running (index into kBuilds), -1 until select_build().
int build();
// OpenRFG64: how many struct layout values (game/layout.inc) its PDB did not give (0 elsewhere).
int layout_missing();
const char* build_name();
// Switches every address in rfg::rva to that build's.
void select_build(int index);
// A code RVA of the running build as the GOG build's, whose PDB names it (diagnostics); 0 if the GOG build has no
// such code.
uint32_t gog_rva(uint32_t rva);
// A call site's RVA as the record/replay harness keys it: the GOG build's, so every rfg.exe build replays alike;
// OpenRFG64's own (its code has no GOG address), so its recordings replay alike in the same build.
uint32_t site_rva(uint32_t rva);
// OpenRFG64: the entry of the names table that resolved a function to `rva` (its name and parameters in OpenRFG.pdb);
// null when none did (or the build is not OpenRFG64).
const ::openrfg64::Name* symbol_of(uint32_t rva);
// An int the game's program exports for rfgphys (OpenRFG64 builds: openrfg_object_index_size, ...); 0 without it.
int game_export_int(const char* name);

inline void* addr(uint32_t rva) { return reinterpret_cast<void*>(base() + rva); }

// OpenRFG64: the return address of a call inside a game function, found in its code instead of written as a
// fixed offset into the 32-bit game's code: the address after the nth direct call (E8 rel32) in [caller, caller +
// caller_size) whose target is `callee`, directly or through one jump thunk (E9 rel32, incremental linking). Null
// when there is none.
const void* call_return(uint32_t caller, uint32_t caller_size, uint32_t callee, int nth = 0);

// OpenRFG64: the code a pointer into the game leads to: through one jump thunk (E9 rel32) when it is one.
// An incrementally linked build's vtables and function pointers point at such thunks, its PDB at the functions.
const void* code_target(const void* p);

// OpenRFG64: the target of one of the game's self-relative 32-bit offset fields (et_ptr_offset: owners, link
// lists). In OpenRFG64 such a field may hold an index into its far table (a target more than 2 GB away), which only the
// program can resolve (it exports openrfg_et_ptr_target); elsewhere it is field + offset.
const uint8_t* rel_ptr(const uint8_t* field);
// The i-th entry of one of the world's object index lists (world::all_objects indices: subtype_objects[].elt and the
// like). rfg.exe's indices are 16-bit; a game build that widens them (OpenRFG64 VOM: 32-bit) says so through an export
// of the program, openrfg_object_index_size.
uint32_t object_index(const void* list, int i);

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

inline bool is_openrfg64() { return build() == kBuildOpenRFG64; }

}  // namespace rfg

// OpenRFG64: the __thiscall idiom. On x86 a __thiscall function is hooked and called as __fastcall with an
// unused EDX parameter after `this`; on x64 there is one calling convention and no such parameter, so these expand to
// nothing there. (tools/convert_edx.py put them in place of the explicit ", void* edx" / ", nullptr" / ", edx".)
#if defined(_WIN64)
#define RFG_EDX_PARAM  // a detour's parameter list, after `this`
#define RFG_EDX_TYPE   // a function pointer type's parameter list, after `this`
#define RFG_EDX_ARG    // a call, after `this`
#define RFG_EDX_PASS   // a detour calling the original, after `this`
#else
#define RFG_EDX_PARAM , void* edx
#define RFG_EDX_TYPE , void*
#define RFG_EDX_ARG , nullptr
#define RFG_EDX_PASS , edx
#endif

namespace rfg {

}  // namespace rfg
