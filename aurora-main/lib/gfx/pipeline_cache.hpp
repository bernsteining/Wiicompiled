#pragma once

#include "common.hpp"

#include <functional>

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::gfx {

using NewPipelineCallback = std::function<wgpu::RenderPipeline()>;

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
void begin_pipeline_frame();
void end_pipeline_frame();
void set_skip_unready_pipelines(bool enabled) noexcept;
bool skip_unready_pipelines() noexcept;
uint32_t queued_pipeline_count() noexcept;

template <typename Config>
PipelineRef find_pipeline(ShaderType type, const Config& config, NewPipelineCallback&& cb);

// GX pipelines of the ubershader's hybrid mode (gx/ubershader.hpp). `persist` records the configuration
// in the pipeline cache database (the ubershader's own pipelines are derived from the specialised
// ones, so they are not). `deferrable`: when gx::defer_specialised_pipelines() is set, the pipeline
// is queued behind all other work and compiled only on frames with few draws (loading screens),
// because compiling stalls the page there; its draws use the ubershader until then.
PipelineRef find_gx_pipeline(const gx::PipelineConfig& config, NewPipelineCallback&& cb, bool persist,
                             bool deferrable);
// Whether the pipeline has compiled (it never becomes unready again).
bool pipeline_ready(PipelineRef ref);

bool wait_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
bool try_pipeline(PipelineRef ref, wgpu::RenderPipeline& pipeline);
// Waits indefinitely for a draw about to be committed to a persistent texture with no re-issue
// path. Separate from wait_pipeline so skip-unready mode never becomes blocking for other passes.
bool wait_pipeline_for_persistent_pass(PipelineRef ref, wgpu::RenderPipeline& pipeline);

#ifdef AURORA_WEBGPU_BROWSER
// The GX and clear pipeline factories create their pipeline through this. When the pipeline cache
// is compiling a queued pipeline it starts an asynchronous compile and returns null (the cache
// inserts the pipeline when the browser finishes it); otherwise it compiles synchronously.
wgpu::RenderPipeline create_render_pipeline(const wgpu::RenderPipelineDescriptor& descriptor);
#endif

} // namespace aurora::gfx
