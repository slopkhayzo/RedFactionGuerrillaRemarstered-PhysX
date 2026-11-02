#pragma once

struct Config;

// Test.ChunkFall=1: one building's collapse, filmed (research/docs/08, loose chunks in PhysX). Once the host has the
// static world, the tallest standing building within 90 m of the player has its ground floor blown out with vehicle
// bombs, and the view holds still 45 m from it, toward the player, while it comes down: a screenshot every 0.5 s for
// 12 s (chunk_fall_NN.png), and each 0.5 s a log line on the loose chunks (debris_offload::chunk_stats: how many the host
// simulates, their parts and mass, the fastest, how far their Havok bodies were from where they were sent, how many
// were left in Havok). Run it with Experiment.PhysXChunks 0 and 1 to compare.
// Test.ChunkFall=2: no bombs: a floor at mid height is taken out, so its top comes down on the stump still standing.
// Test.ChunkFall=3: the longest building, a floor at a third of its height and the far part of the stump taken out, so what
// comes down is held at one end (research/docs/08 §17). The log follows what stands of the buildings next to it, to the end of the
// run. Test.ChunkFallNoStress=1 turns the game's stress evaluation off.
namespace chunk_fall {

bool install(const Config& cfg);

}  // namespace chunk_fall
