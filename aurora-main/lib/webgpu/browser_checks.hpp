#pragma once
// Browser build only: checks WebGPU's rules for a copy before it is recorded, and logs what is
// wrong. Safari (WebKit) reports an invalid command only as "encoder state is not valid" on the
// next pass, without naming the command, and then drops the whole command buffer. The checks cover
// what aurora records: usage flags, sample counts, format compatibility, alignment and bounds.

#ifdef AURORA_WEBGPU_BROWSER

#include "../logging.hpp"
#include "wgpu.hpp"

#include <cstdint>
#include <string>

namespace aurora::webgpu::browser_checks {

inline Module& log() {
  static Module module("aurora::webgpu::checks");
  return module;
}

// Bytes per texel block for the formats aurora uses; 0 when unknown (size checks are skipped).
inline uint32_t block_bytes(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::R8Unorm:
    return 1;
  case wgpu::TextureFormat::RG8Unorm:
  case wgpu::TextureFormat::R16Float:
  case wgpu::TextureFormat::Depth16Unorm:
    return 2;
  case wgpu::TextureFormat::RGBA8Unorm:
  case wgpu::TextureFormat::RGBA8UnormSrgb:
  case wgpu::TextureFormat::BGRA8Unorm:
  case wgpu::TextureFormat::BGRA8UnormSrgb:
  case wgpu::TextureFormat::R32Float:
  case wgpu::TextureFormat::R32Uint:
  case wgpu::TextureFormat::RG16Float:
  case wgpu::TextureFormat::Depth32Float:
    return 4;
  case wgpu::TextureFormat::RGBA16Float:
  case wgpu::TextureFormat::RG32Float:
  case wgpu::TextureFormat::BC1RGBAUnorm:
    return 8;
  case wgpu::TextureFormat::RGBA32Float:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnorm:
    return 16;
  default:
    return 0;
  }
}

inline uint32_t block_width(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnorm:
    return 4;
  default:
    return 1;
  }
}

inline bool is_depth_or_stencil(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::Depth16Unorm:
  case wgpu::TextureFormat::Depth24Plus:
  case wgpu::TextureFormat::Depth24PlusStencil8:
  case wgpu::TextureFormat::Depth32Float:
  case wgpu::TextureFormat::Depth32FloatStencil8:
  case wgpu::TextureFormat::Stencil8:
    return true;
  default:
    return false;
  }
}

inline std::string describe(const wgpu::Texture& texture) {
  if (!texture) return "null texture";
  return fmt::format("format {} usage 0x{:x} {}x{}x{} mips {} samples {}", static_cast<uint32_t>(texture.GetFormat()),
                     static_cast<uint64_t>(texture.GetUsage()), texture.GetWidth(), texture.GetHeight(),
                     texture.GetDepthOrArrayLayers(), texture.GetMipLevelCount(), texture.GetSampleCount());
}

inline bool report(const char* where, const std::string& problem, const std::string& details) {
  static int s_reports = 0;
  if (s_reports < 40) {
    ++s_reports;
    log().error("{}: {} ({})", where, problem, details);
  }
  return false;
}

// The texture region [origin, origin + size) must lie inside the mip level.
inline std::string region_problem(const wgpu::TexelCopyTextureInfo& info, const wgpu::Extent3D& size) {
  const auto& t = info.texture;
  const uint32_t w = std::max(1u, t.GetWidth() >> info.mipLevel);
  const uint32_t h = std::max(1u, t.GetHeight() >> info.mipLevel);
  if (info.mipLevel >= t.GetMipLevelCount()) return "mip level out of range";
  if (info.origin.x + size.width > w || info.origin.y + size.height > h ||
      info.origin.z + size.depthOrArrayLayers > t.GetDepthOrArrayLayers()) {
    return fmt::format("region {},{},{} +{}x{}x{} outside mip {} ({}x{})", info.origin.x, info.origin.y,
                       info.origin.z, size.width, size.height, size.depthOrArrayLayers, info.mipLevel, w, h);
  }
  const uint32_t bw = block_width(t.GetFormat());
  if (bw > 1 && (info.origin.x % bw != 0 || info.origin.y % bw != 0 ||
                 (size.width % bw != 0 && info.origin.x + size.width != w) ||
                 (size.height % bw != 0 && info.origin.y + size.height != h))) {
    return "compressed copy not aligned to blocks";
  }
  return {};
}

inline std::string layout_problem(const wgpu::TexelCopyBufferLayout& layout, const wgpu::Buffer& buffer,
                                  wgpu::TextureFormat format, const wgpu::Extent3D& size) {
  const uint32_t bytes = block_bytes(format);
  const uint32_t bw = block_width(format);
  const uint32_t rows = (size.height + bw - 1) / bw;
  const uint64_t rowBytes = static_cast<uint64_t>((size.width + bw - 1) / bw) * bytes;
  if ((rows > 1 || size.depthOrArrayLayers > 1) && layout.bytesPerRow % 256 != 0) {
    return fmt::format("bytesPerRow {} not a multiple of 256", layout.bytesPerRow);
  }
  if (bytes == 0) return {};
  if (layout.offset % bytes != 0 || (is_depth_or_stencil(format) && layout.offset % 4 != 0)) {
    return fmt::format("offset {} not a multiple of the texel block size {}", layout.offset, bytes);
  }
  if ((rows > 1 || size.depthOrArrayLayers > 1) && layout.bytesPerRow < rowBytes) {
    return fmt::format("bytesPerRow {} smaller than a row ({} bytes)", layout.bytesPerRow, rowBytes);
  }
  const uint64_t rowsPerImage = layout.rowsPerImage == WGPU_COPY_STRIDE_UNDEFINED ? rows : layout.rowsPerImage;
  if (size.depthOrArrayLayers > 1 && rowsPerImage < rows) return "rowsPerImage smaller than the copy height";
  const uint64_t required = size.width == 0 || size.height == 0 || size.depthOrArrayLayers == 0
                                ? 0
                                : layout.offset +
                                      static_cast<uint64_t>(layout.bytesPerRow) * rowsPerImage *
                                          (size.depthOrArrayLayers - 1) +
                                      static_cast<uint64_t>(layout.bytesPerRow) * (rows - 1) + rowBytes;
  if (buffer && required > buffer.GetSize()) {
    return fmt::format("needs {} bytes of a {}-byte buffer", required, buffer.GetSize());
  }
  return {};
}

inline bool buffer_to_texture(const char* where, const wgpu::TexelCopyBufferInfo& src,
                              const wgpu::TexelCopyTextureInfo& dst, const wgpu::Extent3D& size) {
  if (!dst.texture || !src.buffer) return report(where, "null resource", "");
  const auto format = dst.texture.GetFormat();
  const auto details = fmt::format("dst {}", describe(dst.texture));
  if (!(src.buffer.GetUsage() & wgpu::BufferUsage::CopySrc)) return report(where, "buffer lacks CopySrc", details);
  if (!(dst.texture.GetUsage() & wgpu::TextureUsage::CopyDst)) return report(where, "texture lacks CopyDst", details);
  if (dst.texture.GetSampleCount() != 1) return report(where, "multisampled destination", details);
  if (is_depth_or_stencil(format) && format != wgpu::TextureFormat::Depth16Unorm)
    return report(where, "this depth format cannot be a buffer copy destination", details);
  if (auto problem = region_problem(dst, size); !problem.empty()) return report(where, problem, details);
  if (auto problem = layout_problem(src.layout, src.buffer, format, size); !problem.empty())
    return report(where, problem, details);
  return true;
}

inline bool texture_to_buffer(const char* where, const wgpu::TexelCopyTextureInfo& src,
                              const wgpu::TexelCopyBufferInfo& dst, const wgpu::Extent3D& size) {
  if (!src.texture || !dst.buffer) return report(where, "null resource", "");
  const auto format = src.texture.GetFormat();
  const auto details = fmt::format("src {}", describe(src.texture));
  if (!(src.texture.GetUsage() & wgpu::TextureUsage::CopySrc)) return report(where, "texture lacks CopySrc", details);
  if (!(dst.buffer.GetUsage() & wgpu::BufferUsage::CopyDst)) return report(where, "buffer lacks CopyDst", details);
  if (src.texture.GetSampleCount() != 1) return report(where, "multisampled source", details);
  if (format == wgpu::TextureFormat::Depth24Plus || format == wgpu::TextureFormat::Depth24PlusStencil8)
    return report(where, "depth24plus cannot be copied to a buffer", details);
  if (auto problem = region_problem(src, size); !problem.empty()) return report(where, problem, details);
  if (auto problem = layout_problem(dst.layout, dst.buffer, format, size); !problem.empty())
    return report(where, problem, details);
  return true;
}

inline bool texture_to_texture(const char* where, const wgpu::TexelCopyTextureInfo& src,
                               const wgpu::TexelCopyTextureInfo& dst, const wgpu::Extent3D& size) {
  if (!src.texture || !dst.texture) return report(where, "null texture", "");
  const auto details = fmt::format("src {}; dst {}", describe(src.texture), describe(dst.texture));
  const auto srcFormat = src.texture.GetFormat();
  const auto dstFormat = dst.texture.GetFormat();
  const auto base = [](wgpu::TextureFormat f) {
    if (f == wgpu::TextureFormat::RGBA8UnormSrgb) return wgpu::TextureFormat::RGBA8Unorm;
    if (f == wgpu::TextureFormat::BGRA8UnormSrgb) return wgpu::TextureFormat::BGRA8Unorm;
    return f;
  };
  if (base(srcFormat) != base(dstFormat)) return report(where, "formats differ", details);
  if (!(src.texture.GetUsage() & wgpu::TextureUsage::CopySrc)) return report(where, "source lacks CopySrc", details);
  if (!(dst.texture.GetUsage() & wgpu::TextureUsage::CopyDst))
    return report(where, "destination lacks CopyDst", details);
  if (src.texture.GetSampleCount() != dst.texture.GetSampleCount()) return report(where, "sample counts differ", details);
  if (src.texture.Get() == dst.texture.Get() && src.mipLevel == dst.mipLevel)
    return report(where, "copy within one subresource", details);
  if (auto problem = region_problem(src, size); !problem.empty()) return report(where, "source " + problem, details);
  if (auto problem = region_problem(dst, size); !problem.empty())
    return report(where, "destination " + problem, details);
  if (is_depth_or_stencil(srcFormat) &&
      (size.width != src.texture.GetWidth() || size.height != src.texture.GetHeight()))
    return report(where, "depth copies must cover the whole texture", details);
  return true;
}

inline bool buffer_to_buffer(const char* where, const wgpu::Buffer& src, uint64_t srcOffset, const wgpu::Buffer& dst,
                             uint64_t dstOffset, uint64_t size) {
  if (!src || !dst) return report(where, "null buffer", "");
  const auto details = fmt::format("src {} bytes usage 0x{:x}; dst {} bytes usage 0x{:x}; {} bytes at {} -> {}",
                                   src.GetSize(), static_cast<uint64_t>(src.GetUsage()), dst.GetSize(),
                                   static_cast<uint64_t>(dst.GetUsage()), size, srcOffset, dstOffset);
  if (!(src.GetUsage() & wgpu::BufferUsage::CopySrc)) return report(where, "source lacks CopySrc", details);
  if (!(dst.GetUsage() & wgpu::BufferUsage::CopyDst)) return report(where, "destination lacks CopyDst", details);
  if (size % 4 != 0 || srcOffset % 4 != 0 || dstOffset % 4 != 0) return report(where, "not 4-byte aligned", details);
  if (srcOffset + size > src.GetSize() || dstOffset + size > dst.GetSize()) return report(where, "out of bounds", details);
  if (src.Get() == dst.Get()) return report(where, "copy within one buffer", details);
  return true;
}

} // namespace aurora::webgpu::browser_checks

#endif
