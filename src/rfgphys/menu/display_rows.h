#pragma once
#include <string>

// The display menu's rows for the plugin's settings, hooked into the game's unmodified menu (OpenRFG64 VOM has its
// PHYSICS and KEEP DEBRIS rows in its own source). After the game's rows (and the path tracer's, when it adds its own)
// on the general page:
//   PHYSICS (NEXT START)      HAVOK (the game's debris) or PHYSX: rfgphys.ini's Experiment.PhysX (0, or 1: the GPU,
//                             the CPU without CUDA; a 2 already set, the CPU, is kept while the row is not changed).
//   KEEP DEBRIS (NEXT START)  the game no longer removes debris: Experiment.KeepDebris. Not in OpenRFG64, whose source
//                             raises those limits itself (the plugin's keep_debris stays off there).
// Both are read at the game's start (the PhysX host starts with the plugin, KeepDebris sizes the game's pools), hence
// "next start" in their captions; changed from the main menu only (greyed out in game). Accept writes them to
// rfgphys.ini; back asks before dropping changes, as for the game's own rows (the game's changes_made). The captions and
// values go into the game's localization table, whose data has none of them.
struct Config;

namespace display_rows {

// The hooks (Menu.Rows). `ini`: the rfgphys.ini Accept writes to. False when the game lacks a name the rows need.
bool install(const Config& cfg, const std::wstring& ini);

}  // namespace display_rows
