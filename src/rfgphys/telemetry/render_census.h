#pragma once
#include <cstdint>

// Render-thread work that costs driver time per draw: dynamic constant-buffer uploads (flushDynamicVertex/
// FragmentConstantBuffer: Map(WRITE_DISCARD), copy, Unmap, bind) and GPU occlusion queries (rl_gpu_vis_render_mesh_query
// draws a bounding mesh inside a query; pollOcclusionQueryResult reads results back). Counted on the render thread and
// published once per render frame; telemetry.csv gets per-frame totals.
namespace render_census {

bool install(bool cb_redundancy);  // cb_redundancy: also detect repeated uploads (hashes every upload)
void publish();  // the render thread, at the end of each render frame

struct FrameCounts {
    uint32_t cb_uploads;    // constant-buffer uploads (vertex + fragment)
    uint32_t cb_redundant;  // of those, the same buffer at the same slot with the same bytes as its previous upload
                            // (only with cb_redundancy)
    uint32_t cb_kb;         // kilobytes uploaded
    uint32_t occl_queries;  // occlusion queries issued
    uint32_t occl_polls;    // occlusion query results polled
};
FrameCounts take();  // the sim thread, at the end of each frame

}  // namespace render_census
