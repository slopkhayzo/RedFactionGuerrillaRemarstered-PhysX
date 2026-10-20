#include "game/rfg.h"

#include <windows.h>

namespace rfg {

uintptr_t base() {
    static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

const char* game_state_name(int value) {
    for (const auto& s : game_state_names)
        if (s.value == value) return s.name;
    return "?";
}

}  // namespace rfg
