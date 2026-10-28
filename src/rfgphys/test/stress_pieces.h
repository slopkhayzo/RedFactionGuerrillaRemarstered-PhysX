#pragma once

struct Config;

// Test.StressPieces=N (single player, PhysX debris on): rubble piles up past what any base map holds, to show where
// the design stops before a custom map with 260,000-500,000 pieces exists (research/docs/03, phase 7). A levelled base
// map tops out at about 32,000 pieces; here the same buildings come down again and again.
// - Test.StressStartS (15) into gameplay, the standing buildings within Test.StressRadiusM (120 m) of the player are
//   noted (the player at least 4 m outside each one's box: a restored building comes back where it stood).
// - A cycle: each building is blown up by vehicle bombs throughout its box (up to three rounds while much of it still
//   stands); its rubble settles into PhysX and its piles; the view holds over the area for 4 s, measured; then the game
//   restores the buildings (world::restore_object) and their rubble stays (KeepDebris and debris_offload keep it).
//   The bombs do not kick the pieces in PhysX, and a building coming down again wakes none of the rubble it was
//   restored over (rfgphysx::kTestNoUnsupportedWakes): the rubble of the cycles before stays at rest in its piles.
// - Each cycle's log line ("stress: cycle N") has the pieces, the piles, the render instances left, Havok's heap, the
//   address space and the overview's frame times.
// - It stops at N pieces, or before a limit: PhysX slots for one more cycle (Experiment.PhysXMaxBodies), Havok's heap,
//   render instances, address space, the run's time (ExitAfterGameplaySeconds). Then the player's view is measured at
//   rest, the viewpoints follow with Test.Viewpoints=1, and an unattended run ends.
namespace stress_pieces {

bool install(const Config& cfg);

}  // namespace stress_pieces
