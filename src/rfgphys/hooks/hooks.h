#pragma once
#include <cstdint>

namespace hooks {

bool init();
// Creates a MinHook detour on the function at `rva`; `original` receives the trampoline. Enabled by enable_all().
bool create(uint32_t rva, void* detour, void** original, const char* name);
// Same for an absolute address (functions outside rfg.exe, e.g. D3D11 methods); enable it with enable_at().
bool create_at(void* target, void* detour, void** original, const char* name);
bool enable_at(void* target);
bool enable_all();
void shutdown();

// Replaces rfg.exe's import of dll!function in its import address table. Returns the previous pointer, or null.
void* patch_import(const char* dll, const char* function, void* replacement);

// A game that builds rfgphys in can attach the hooks to seams in its own source instead of MinHook detours (OpenRFG64
// VOM: its src/backends/seams.h). Set before anything hooks. create() asks the binding first: true when a seam took
// the hook (`original` then runs the game's function), false for a MinHook detour. enable_all() turns the seams on.
struct Binding {
    bool (*create)(uint32_t rva, void* detour, void** original, const char* name);
    void (*enable_all)();
    const void* (*caller)(const void* return_address);  // see caller() below
};
void set_binding(const Binding& binding);
// A hook only where the binding has a seam for it: false without one (and in the plugins, which have no binding), so
// the caller attaches another way (rfgphys's vtable entries).
bool seam(uint32_t rva, void* detour, void** original, const char* name);

// The game's call site of the call a handler handles, given the handler's own return address (_ReturnAddress(), read
// first thing in the handler): a detour's own, a seam's from the seam (which calls the handler itself).
const void* caller(const void* return_address);

template <typename Fn>
bool create(uint32_t rva, Fn detour, Fn* original, const char* name) {
    return create(rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}

template <typename Fn>
bool seam(uint32_t rva, Fn detour, Fn* original, const char* name) {
    return seam(rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}

}  // namespace hooks
