#pragma once

struct Config;

// Experiment.DebrisCollide=1: every debris piece collides with every other debris piece.
//
// A collision filter info is (system group << 16) | layer, and havok_game_filter never lets two bodies of the same
// non-zero system group collide. The game gives all four debris collision types (RFG_NORMAL_FAST, RFG_LITE,
// RFG_ULTRA_LITE, RFG_WORLD_ONLY) system group 6, which it shares with other "never collide with each other" bodies
// (dead humans, attached projectiles, invisible barriers, ...), so debris never collides with debris whatever the
// layer table says. The experiment moves those four types to group 0 (process-wide, before any level loads), lets
// RFG_WORLD_ONLY collide like RFG_ULTRA_LITE, enables all debris-debris layer pairs, and clears debris pairs with
// layers only used by group-6 bodies so those keep not colliding. Off in multiplayer.
//
// DebrisCollide=1 also keeps debris-debris contacts away from the game's contact listener (core/contacts.h): the
// pieces collide physically, but the game never turns those impacts into destruction damage. DebrisCollide=2 lets
// the listener handle them, so debris also damages and breaks up debris.
//
// Colliding debris merges into a few large simulation islands, and the game deletes the debris of any island whose
// solver memory exceeds its 1 MB solver buffer (havok_sim_islands_process_post): with DebrisCollide=2 a whole debris
// field vanished in one frame. The experiment gives Havok's solver a larger buffer (Experiment.SolverBufferMB,
// default 16) and raises that limit to match (research/docs/01-findings.md §10.4). An island that outgrows even that
// gets a buffer it fits in, up to 96 MB, instead of losing all but 64 of its bodies.
//
// Havok's solver setup for constraints other than plain contacts loops for good on an atom it cannot get past; a
// tower shattered into 2,700 pieces hung the game there. Such an atom now ends its constraint's setup, and the log
// reports it (Test.SolverGuard checks the guard at start-up).
namespace experiments {

bool install_debris_collision(const Config& cfg);

}  // namespace experiments
