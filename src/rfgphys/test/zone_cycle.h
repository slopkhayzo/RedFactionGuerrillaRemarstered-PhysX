#pragma once

struct Config;

// Test.ZoneCycle: zones streaming out under debris that lives in PhysX, unattended (the pieces' chunk references and
// their release with their zone, physx/debris_offload.cpp). In the campaign, from where the save put the player:
// buildings near the player are blown up, the player is teleported away in two hops (to loaded zones) until the start's
// zone streams out, then back in two hops, and the buildings there are blown up again. The log tells what streamed out
// and what the pieces did. (Cutting the zone streaming radius instead crashed the game's terrain renderer at once.)
// Test.ZoneTour=N instead hops N times across the open world, every 15 s, to a loaded zone 450-800 m away (picked at
// random, the same sequence every run), blowing up the buildings near each landing: a long, destructive stay in the
// open world. With Experiment.PhysXWorldCheck the mirror is checked after every landing.
namespace zone_cycle {

bool install(const Config& cfg);

}  // namespace zone_cycle
