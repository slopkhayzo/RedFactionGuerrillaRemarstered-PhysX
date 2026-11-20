// OpenRFG64: the layout variables (game/layout.h) and the names OpenRFG64's values are looked up by.
#include "game/layout.h"

#include "openrfg64_column.h"

namespace layout {

#define RFG_LAYOUT_M(var, x86, type, path, addend) uint32_t var = x86;
#define RFG_LAYOUT_S(var, x86, type) uint32_t var = x86;
#define RFG_LAYOUT_O(var, x86, type, path, addend) uint32_t var = x86;
#include "game/layout.inc"
#undef RFG_LAYOUT_M
#undef RFG_LAYOUT_S
#undef RFG_LAYOUT_O

namespace {

openrfg64::Name g_names[] = {
#define RFG_LAYOUT_M(var, x86, type, path, addend) {&var, 'm', type, path, nullptr, nullptr, addend},
#define RFG_LAYOUT_S(var, x86, type) {&var, 's', type, nullptr, nullptr, nullptr, 0},
#define RFG_LAYOUT_O(var, x86, type, path, addend)
#include "game/layout.inc"
#undef RFG_LAYOUT_M
#undef RFG_LAYOUT_S
#undef RFG_LAYOUT_O
};

openrfg64::Name g_optional[] = {
#define RFG_LAYOUT_M(var, x86, type, path, addend)
#define RFG_LAYOUT_S(var, x86, type)
#define RFG_LAYOUT_O(var, x86, type, path, addend) {&var, 'm', type, path, nullptr, nullptr, addend},
#include "game/layout.inc"
#undef RFG_LAYOUT_M
#undef RFG_LAYOUT_S
#undef RFG_LAYOUT_O
};

}  // namespace

openrfg64::Name* names() { return g_names; }
size_t name_count() { return sizeof(g_names) / sizeof(g_names[0]); }

openrfg64::Name* optional_names() { return g_optional; }
size_t optional_count() { return sizeof(g_optional) / sizeof(g_optional[0]); }
void zero_optional() {
    for (openrfg64::Name& n : g_optional) *n.value = 0;
}

}  // namespace layout
