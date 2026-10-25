#pragma once

struct Config;

// Test.RestoreBuilding=N: a building the game restores (as an activity does when it starts over) takes its kept debris
// along, unattended. In the campaign, 15 s into gameplay the biggest standing building 10-60 m from the player is
// blown up (the log says what ties the new debris to it: original_object or parent); 12 s later, its pieces in PhysX
// or still in Havok, the game restores it (world::restore_object), and 5 s later the log says how many pieces from it
// are left and whether the building stands again. N times in all (a
// demolition replayed over and over): the general movers and the free render instances after each cycle show whether
// anything piles up.
namespace restore_building {

bool install(const Config& cfg);

}  // namespace restore_building
