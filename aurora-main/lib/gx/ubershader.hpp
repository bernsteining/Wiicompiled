#pragma once
// The GX ubershader: one shader module for the GX draws a specialised shader would otherwise be
// generated for (shader.cpp). It reads the draw's ShaderConfig, packed into a few dozen words,
// from the end of the draw's uniform data, so GX pipelines differ only by their fixed-function
// state: the ~2700 configurations Mario Kart Wii produced map to ~100 pipelines. That matters in
// browsers whose GPU process compiles every new pipeline with the page stalled (Safari: ~100-300 ms
// each), so a specialised shader first seen mid-race is a visible hitch.
//
// Modes (set_ubershader_mode):
// - Hybrid: a draw uses its specialised pipeline once that exists and the ubershader until then, so
//   a configuration seen for the first time is neither skipped (missing geometry) nor a stall. Its
//   specialised pipeline compiles in the background; where compiling stalls the page (Safari), only
//   on frames with few draws, such as loading screens (defer_specialised_pipelines).
// - All: every covered draw uses the ubershader. Fewest pipelines, but measured on an iPad its GPU
//   time per race frame was ~2.5x the specialised shaders' (16.5 vs 6.3 ms).
// Configurations it does not cover (line and point expansion, z textures, TEV compare ops, bump
// texgens) always use their specialised shader.

#include "gx.hpp"

#include <array>

namespace aurora::gx {

struct PipelineConfig;

inline constexpr u32 MaxUberConfigWords = 128;

struct UberConfig {
  u32 wordCount = 0; // a multiple of 4: the block is read as vec4u
  std::array<u32, MaxUberConfigWords> words{};
};

enum class UbershaderMode : u8 { Off, Hybrid, All };

UbershaderMode ubershader_mode() noexcept;
void set_ubershader_mode(UbershaderMode mode) noexcept;
inline bool ubershader_enabled() noexcept { return ubershader_mode() != UbershaderMode::Off; }
// Hybrid mode only: queue missing specialised pipelines behind everything else and compile them only
// on quiet frames (gfx::find_gx_pipeline), because the platform stalls while compiling.
bool defer_specialised_pipelines() noexcept;

// The packed configuration for `config`, or wordCount 0 when the ubershader does not cover it.
UberConfig pack_ubershader_config(const ShaderConfig& config, const ShaderInfo& info) noexcept;

// GX pipelines drawn with the ubershader are keyed by this shader config instead of their own.
ShaderConfig ubershader_marker_config() noexcept;
bool is_ubershader_marker(const ShaderConfig& config) noexcept;

// The ubershader's module, built on first use.
wgpu::ShaderModule ubershader_module() noexcept;

} // namespace aurora::gx
