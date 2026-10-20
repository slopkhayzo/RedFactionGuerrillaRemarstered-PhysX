#include "game/rfg.h"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <vector>

namespace rfg {
namespace {

int g_build = -1;

}  // namespace

uintptr_t base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

uint32_t image_size() {
    auto b = reinterpret_cast<const uint8_t*>(base());
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew);
    return nt->OptionalHeader.SizeOfImage;
}

int build() { return g_build; }

const char* build_name() { return g_build >= 0 ? kBuilds[g_build].name : "unknown"; }

void select_build(int index) {
    for (const RvaEntry& e : kRvaTable) *e.rva = e.in_build[index];
    g_build = index;
}

uint32_t gog_rva(uint32_t rva) {
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

const char* game_state_name(int value) {
    for (const auto& s : game_state_names)
        if (s.value == value) return s.name;
    return "?";
}

}  // namespace rfg
