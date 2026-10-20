#pragma once
#include <cstdint>

struct Config;

// Render-thread experiments (research/docs/01-findings.md §10.5: heavy frames are limited by the render thread).
//
// Experiment.GpuOcclusion=0 turns off the renderer's GPU occlusion queries (RL_gpu_vis_enabled, read by the
// visibility manager every frame). Each query draws a bounding mesh inside a D3D11 query and is polled later; in heavy
// frames they took about a fifth of the render thread's D3D11 and driver time. Without them objects that only the
// queries would have hidden are drawn.
namespace experiments {

void install_render_tweaks(const Config& cfg);
// The most instances one batched pass asked to draw one by one since the last call (before the cap; telemetry).
uint32_t take_unbatched_peak();
// The most entries the scene renderer's deferred sort list was asked to hold since the last call (9,216 fit).
uint32_t take_sort_peak();
// The most instances a frame asked to draw batched since the last call (24,576 fit unless Experiment.BatchedCap).
uint32_t take_batched_peak();
// Instanced batches the GPU instancing buffer could not take since the last call (they were not drawn).
uint32_t take_instancing_failed();

}  // namespace experiments
