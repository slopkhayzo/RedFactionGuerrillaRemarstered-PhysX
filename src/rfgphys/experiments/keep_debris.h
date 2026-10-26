#pragma once
#include <cstdint>
#include <vector>

struct Config;

namespace experiments {

// Experiment.KeepDebris=1 (single player): the game stops removing debris. Every way it does that goes off:
// - shattering keeps every piece: rfgl_desired_values' delete_count (pieces deleted on the spot, all but about 20
//   per shattered chunk) becomes 0;
// - no fade-outs: object_mover_mark_for_fade is refused for pieces killed by damage, temporary shards and the load
//   governor's picks (AI vehicles still clear debris they are stuck on);
// - pieces an explosion would "vaporize" (rfg_dyn_maybe_delete) take its impulse instead (the game skips the impulse
//   for them), and an explosion pushes the 256 objects nearest to it (explosion_apply_damage can push 256, which in
//   a big pile were 256 arbitrary ones);
// - the load governor's deletion pass (rfgl_maybe_delete_objects) does not run. Its emergency cleanup when Havok
//   runs out of memory still does.
// Pieces then live until they leave the world. Two limits remain, both raised at start-up when asked:
// - Experiment.RenderInstances=N: every chunk and piece needs a destroyable-model render instance; the game has
//   2,100, and shattering creates no more pieces once they are used up;
// - Experiment.DebrisPool=N: debris pieces are general_mover objects from a fixed pool of 3,072, rebuilt with N
//   slots (up to 65,000) before the game creates any; the world's object lists that hold them (sized at start-up
//   from the class's 3,072) grow with it, within 65,535 world objects and the permanent memory pool;
// - Experiment.HavokHeapMB=N: Havok's heap is a fixed 75 MB; when colliding piles fill it the game stops in an
//   endless loop (havok_memory_listener::allocationFailure). The governor's emergency cleanup still runs before
//   that, and its fade-outs go through.
// One limit cannot be raised: each world zone hands out at most 7,280 object handles (the handle's low 16 bits;
// a Wrecking Crew level is one zone). With the zone full nothing can be created in it: no pieces, projectiles or the
// objects the level streams in by distance. Experiment.ZoneReserve=N (default 1,200) keeps N handles free: shattering
// keeps its extra pieces only while N/2 stay free, and below N the debris farthest from the camera is recycled (out
// of view: removed; in view: faded out) and the game's own removal paths run again until the zone has room.
bool install_keep_debris(const Config& cfg);

struct KeepDebrisFrame {
    uint32_t fades_blocked;    // fade marks refused this frame
    uint32_t deletes_blocked;  // pieces shattering would have deleted on the spot
    uint32_t vaporize_refused; // pieces an explosion would have vaporized; they take its impulse instead
    uint32_t explosion_hits_dropped;  // objects in explosions' reach beyond the 256 each explosion can push
    int32_t world_objects;     // objects in the world
    int32_t zone_used;         // object handles in use in the fullest world zone (a zone has 7,280)
    uint32_t recycled;         // debris pieces recycled (farthest first) to keep the zone reserve free
    uint32_t gate_deletes;     // pieces shattering deleted on the spot because the zone reserve was reached
    uint32_t handle_failures;  // objects created with no handle left in their zone (should stay 0)
    uint32_t shards_failed;    // rfg_dyn_create_shard calls that returned no piece (pool full)
    int32_t gm_pool_free;      // free general_mover slots (debris pieces)
    int32_t rm_pool_free;      // free rfg_mover slots (building chunks)
    int32_t render_used;       // rl_destroyable_model_instance objects in use (chunks and pieces being drawn)
    int32_t render_max;        // ... and the most the renderer's factory allows
};
KeepDebrisFrame keep_debris_take_frame();  // sim thread, end of frame
int32_t debris_pool_capacity();
// Diagnostics: the loose chunks the last restoration took (handles), and how many chunks were noted.
const std::vector<uint32_t>& last_restored_chunks();
uint64_t loose_chunks_noted();

}  // namespace experiments
