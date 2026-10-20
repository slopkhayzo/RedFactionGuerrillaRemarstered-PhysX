#pragma once
#include <cstdint>

// Where a render frame's time goes across the two threads that build it. rl_scene_renderer::render runs on the
// render thread: it builds the visibility lists, wakes the scene thread (rl_scene_renderer::runSceneRenderPart2,
// which records the main scene: SSAO, depth pre-pass, base pass, lighting, transparents, particles, post-processing),
// meanwhile records the depth pre-pass, GPU occlusion queries and shadow maps itself, then waits for the scene thread.
// Each thread records into its own D3D11 deferred context. The keen::Event waits and signals between them mark every
// step; per render frame the timeline goes to render_timeline.csv (Telemetry.RenderCensus=1).
namespace render_timeline {

bool install();
unsigned long scene_thread_id();  // the scene thread (0 until it has rendered once)
// The render thread, around do_game_render_frame; draws recorded by each of the two threads during the frame.
void begin_render_frame();
void end_render_frame(uint32_t draws_render, uint32_t draws_scene);

}  // namespace render_timeline
