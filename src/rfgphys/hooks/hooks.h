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

template <typename Fn>
bool create(uint32_t rva, Fn detour, Fn* original, const char* name) {
    return create(rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(original), name);
}

}  // namespace hooks
