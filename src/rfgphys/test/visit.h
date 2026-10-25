#pragma once

struct Config;

// Test.Visit=x,z,tx,tz: takes the player to (x, z) in the campaign, facing (tx, tz), so an input script can act there
// (a structure far from any save, such as the tower of the first mission). From 3 s into gameplay the player hops
// through loaded zones towards (x, z), as Test.ZoneCycle does (a teleport needs ground that has streamed in), then
// stands at the nearest free place there; the log lists the buildings within 40 m with their size. Further stops
// (|x,z,tx,tz@s, each at s seconds of gameplay) are near the first: the player is put there directly.
namespace visit {

bool install(const Config& cfg);

}  // namespace visit
