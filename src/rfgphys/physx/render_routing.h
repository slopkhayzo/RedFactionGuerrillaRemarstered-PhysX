#pragma once
#include <vector>

struct Config;

// Diagnostic (Telemetry.RenderRouting=1): which way the renderer draws destroyable models (building chunks and
// debris pieces). Counts the calls of their render entry points:
// - rl_compound_mesh_instance::add_to_batch: instanced in a batched pass (the scene renderer sends entities whose
//   flags have bit 7 there; a destroyable instance's constructor sets it);
// - rl_compound_mesh_instance::add_to_deferred: queued in the pass's list of instances drawn one by one (4,096 at
//   most, research/docs/06 §15);
// - rl_destroyable_model_instance::render and rl_compound_mesh_instance::render_for_deferred: draws of their own;
// separately for the PhysX host's debris pieces (physx/debris_offload.h) and every other destroyable instance, with
// the call sites and the flag bits that route an instance; logged every 5 s of gameplay.
namespace render_routing {

bool install(const Config& cfg);
bool enabled();
// The render instances the debris offload owns, sorted (sim thread; the render threads search a copy).
void publish_pieces(std::vector<const void*>&& sorted);

}  // namespace render_routing
