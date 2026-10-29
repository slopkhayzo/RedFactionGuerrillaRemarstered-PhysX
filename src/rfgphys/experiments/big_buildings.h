#pragma once

struct Config;

// Fixes so the game copes with buildings of 10,000 pieces and more (the generated ones of research/docs/07, B5a; the
// largest vanilla building has 4,756). Always on.
//
// - The sim thread's scratch mempool (RFG_sim_scratch_pool, the temporary allocator of everything the sim thread does)
//   is 128 KB; sim_scratch_system_init now gives it 2 MB of its own. rl_fp_kdtree::build (the render-side tree a
//   building's instance builds over its sub-instances, 16 B each) asked for 160 KB when a 10,000-piece building was
//   restored and wrote into the null it got.
// - rl_destroyable_model_instance::release_sub_instances_internal hands sub-instances back in batches of 512 instead
//   of 4,096 (32 B each, through a scratch block): a burst released 3,927 at once, needing 125 KB of that 128 KB pool.
//   The batch cap and the loop's three steps change together.
namespace experiments {

void install_big_buildings(const Config& cfg);

}  // namespace experiments
