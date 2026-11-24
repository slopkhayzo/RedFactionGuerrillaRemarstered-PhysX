#include "game/rfg.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/layout.h"
#include "game/rfg_symbols_names.generated.h"
#include "openrfg64_column.h"

namespace rfg {
namespace {

int g_build = -1;
int g_layout_missing = 0;  // OpenRFG64: layout values its PDB did not give

}  // namespace

uintptr_t base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

uint32_t image_size() {
    auto b = reinterpret_cast<const uint8_t*>(base());
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

int build() { return g_build; }

int layout_missing() { return g_layout_missing; }

const char* build_name() { return g_build == kBuildOpenRFG64 ? "OpenRFG64" : g_build >= 0 ? kBuilds[g_build].name : "unknown"; }

void select_build(int index) {
    if (index == kBuildOpenRFG64) {
        // OpenRFG64: every address and layout value from the program's own PDB, by name.
        std::string report;
        const int missing = openrfg64::load(kOpenRFG64Names, std::size(kOpenRFG64Names), base(), report);
        size_t start = 0;
        for (size_t end = report.find('\n'); end != std::string::npos; start = end + 1, end = report.find('\n', start))
            LOG("%s", report.substr(start, end - start).c_str());
        if (missing > 0) LOG("OpenRFG64: %d names missing: what uses them stays off", missing);
        // The struct layouts (game/layout.inc). A value not found keeps its x86 default, which is wrong on x64: the
        // plugin does not run then (main.cpp).
        std::string layout_report;
        g_layout_missing = openrfg64::load(layout::names(), layout::name_count(), base(), layout_report);
        start = 0;
        for (size_t end = layout_report.find('\n'); end != std::string::npos; start = end + 1, end = layout_report.find('\n', start))
            LOG("layout: %s", layout_report.substr(start, end - start).c_str());
        if (g_layout_missing > 0)
            LOG("OpenRFG64: %d layout values not found (their x86 values would be read on x64): the plugin stays inactive",
                g_layout_missing);
        // The optional ones: 0 where the build has no such member (their users check).
        layout::zero_optional();
        std::string optional_report;
        const int optional_missing = openrfg64::load(layout::optional_names(), layout::optional_count(), base(), optional_report);
        if (optional_missing > 0)
            LOG("layout: %d optional values not in this build (0: what reads them stays off)", optional_missing);
        g_build = index;
        return;
    }
    for (const RvaEntry& e : kRvaTable) *e.rva = e.in_build[index];
    g_build = index;
}

uint32_t site_rva(uint32_t rva) { return g_build == kBuildOpenRFG64 ? rva : gog_rva(rva); }

const ::openrfg64::Name* symbol_of(uint32_t rva) {
    if (g_build != kBuildOpenRFG64 || rva == 0) return nullptr;
    for (const ::openrfg64::Name& n : kOpenRFG64Names)
        if (n.kind == 'f' && n.value && *n.value == rva) return &n;
    return nullptr;
}

uint32_t gog_rva(uint32_t rva) {
    if (g_build == kBuildOpenRFG64) return 0;  // OpenRFG64: no GOG address for OpenRFG64's code
    if (g_build <= 0) return rva;  // the GOG build itself
    // The Steam build's code ranges by their Steam addresses (the profiler asks thousands of times a second).
    static const std::vector<CodeRange> by_own = [] {
        std::vector<CodeRange> v(std::begin(kSteamCode), std::end(kSteamCode));
        std::sort(v.begin(), v.end(),
                  [](const CodeRange& a, const CodeRange& b) { return a.gog_first + a.delta < b.gog_first + b.delta; });
        return v;
    }();
    auto it = std::upper_bound(by_own.begin(), by_own.end(), rva,
                               [](uint32_t a, const CodeRange& c) { return a < c.gog_first + c.delta; });
    if (it == by_own.begin()) return 0;
    --it;
    return rva <= it->gog_last + it->delta ? uint32_t(rva - it->delta) : 0;
}

const void* call_return(uint32_t caller, uint32_t caller_size, uint32_t callee, int nth) {
    if (caller == 0 || callee == 0 || caller_size < 5) return nullptr;
    const uint8_t* const code = static_cast<const uint8_t*>(addr(caller));
    const uintptr_t target_fn = base() + callee;
    auto resolve = [](uintptr_t target) {
        const uint8_t* t = reinterpret_cast<const uint8_t*>(target);
        if (t[0] == 0xE9) {  // an incremental link thunk: jmp rel32
            int32_t rel;
            memcpy(&rel, t + 1, 4);
            return target + 5 + intptr_t(rel);
        }
        return target;
    };
    for (uint32_t i = 0; i + 5 <= caller_size; ++i) {
        if (code[i] != 0xE8) continue;
        int32_t rel;
        memcpy(&rel, code + i + 1, 4);
        const uintptr_t target = reinterpret_cast<uintptr_t>(code + i + 5) + intptr_t(rel);
        if (target < base() || target >= base() + image_size()) continue;
        if (target != target_fn && resolve(target) != target_fn) continue;
        if (nth-- == 0) return code + i + 5;
    }
    return nullptr;
}

const void* code_target(const void* p) {
    const uint8_t* t = static_cast<const uint8_t*>(p);
    const uintptr_t at = reinterpret_cast<uintptr_t>(t);
    if (!t || at < base() || at + 5 > base() + image_size() || t[0] != 0xE9) return p;
    int32_t rel;
    memcpy(&rel, t + 1, 4);
    return t + 5 + rel;
}

const uint8_t* rel_ptr(const uint8_t* field) {
    using Resolve = const void* (*)(const void*);
    static const Resolve resolve =
        reinterpret_cast<Resolve>(GetProcAddress(GetModuleHandleW(nullptr), "openrfg_et_ptr_target"));
    if (resolve) return static_cast<const uint8_t*>(resolve(field));
    return field + *reinterpret_cast<const int32_t*>(field);
}

int game_export_int(const char* name) {
    const int* value = reinterpret_cast<const int*>(GetProcAddress(GetModuleHandleW(nullptr), name));
    return value ? *value : 0;
}

uint32_t object_index(const void* list, int i) {
    static const int* const wide = reinterpret_cast<const int*>(GetProcAddress(GetModuleHandleW(nullptr), "openrfg_object_index_size"));
    if (wide && *wide == 4) return static_cast<const uint32_t*>(list)[i];
    return static_cast<const uint16_t*>(list)[i];
}

const char* game_state_name(int value) {
    for (const auto& s : game_state_names)
        if (s.value == value) return s.name;
    return "?";
}

}  // namespace rfg
