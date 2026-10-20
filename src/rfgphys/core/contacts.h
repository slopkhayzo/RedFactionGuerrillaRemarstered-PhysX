#pragma once
#include <cstdint>

// The game's contact listener (GeneralCollisionListener::contactPointAddedCallback): one hook shared by telemetry
// (contact counters) and experiments (skipping the game's impact logic for some pairs).
namespace contacts {

bool install();  // before hooks::enable_all; idempotent

// Called for every contact point added, inside the Havok step, with both bodies' layers: on the sim thread, or with a
// multithreaded step (experiments/havok_mt.h) also on Havok's worker threads, one call at a time (core/game_lock.h).
void add_observer(void (*fn)(uint32_t layer_a, uint32_t layer_b));

// Contacts between two debris bodies (layers 41-44) skip the game's listener: Havok still resolves the collision,
// but the game does not turn the impact into destruction damage, sounds or effects (it never sees such contacts in
// the vanilla game, where debris never collides with debris).
void set_skip_debris_pairs(bool on);

inline bool is_debris_layer(uint32_t layer) { return layer >= 41 && layer <= 44; }

}  // namespace contacts
