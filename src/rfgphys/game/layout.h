#pragma once
// OpenRFG64: the struct offsets and sizes rfgphys reads from the game's memory, by name.
//
// Upstream they are constants of the 32-bit rfg.exe (kObjFlags = 0x54, player + 0x04, ...). Here each is a variable
// that starts as the 32-bit value (so an x86 build is unchanged) and becomes OpenRFG64's at start-up: rfg::select_build
// looks every entry of game/layout.inc up in OpenRFG64's PDB by type and member path (src/shared/openrfg64_column).
// The paths were found with tools/offset_map.py: the 32-bit offset's member in the GOG rfg.pdb, the same member in
// OpenRFG64. tools/check_layout.py checks the table against a build's PDB offline.
//
// RFG_LAYOUT_M: a member's offset; RFG_LAYOUT_S: a type's size. Both are required: the plugin does not run on
// OpenRFG64 when one is not found (its x86 value would be read). RFG_LAYOUT_O: a member the build may not have; it is
// 0 there, and its users check for that.
#include <cstddef>
#include <cstdint>

namespace openrfg64 {
struct Name;
}

namespace layout {

#define RFG_LAYOUT_M(var, x86, type, path, addend) extern uint32_t var;
#define RFG_LAYOUT_S(var, x86, type) extern uint32_t var;
#define RFG_LAYOUT_O(var, x86, type, path, addend) extern uint32_t var;
#include "game/layout.inc"
#undef RFG_LAYOUT_M
#undef RFG_LAYOUT_S
#undef RFG_LAYOUT_O

// The required entries select_build fills for OpenRFG64.
openrfg64::Name* names();
size_t name_count();
// The optional entries: zero_optional() first, then the ones found are filled.
openrfg64::Name* optional_names();
size_t optional_count();
void zero_optional();

}  // namespace layout
