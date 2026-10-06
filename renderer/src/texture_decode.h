// Carbon native renderer: guest texture layout (tiling, mips, packed mip
// tails) and conversion to host Vulkan formats, on the CPU.
//
// Layout rules follow the Xbox 360 Direct3D texture layout as documented in
// the SDK's texture util header (Xenia, BSD): 32x32-block tiles, 4 KiB
// subresource alignment, power-of-two mip strides, packed mip tails for
// levels of 16 texels or less.
#pragma once

#include <cstdint>
#include <vector>

#include <rex/ui/vulkan/api.h>

#include "gpu_common.h"

namespace carbon::gpu {

enum class TextureConversion : uint8_t {
  kNone,          // Copy blocks (after the endian swap).
  kR5G6B5,        // -> RGBA8
  kR6G5B5,        // -> RGBA8
  kR5G5B5A1,      // -> RGBA8
  kR4G4B4A4,      // -> RGBA8
  kR10G11B11,     // -> RGBA16 unorm
  kR11G11B10,     // -> RGBA16 unorm
  kDepth24,       // 24-bit unorm depth (high bits) -> R32 float
  kDepth24Float,  // 20e4 float depth -> R32 float
  kCtx1,          // CTX1 -> RG8
  kDxt3a,         // 4-bit alpha blocks -> R8
  kBc1ToRgba8,    // Software BC fallbacks (devices without BC support).
  kBc2ToRgba8,
  kBc3ToRgba8,
  kBc4ToR8,
  kBc5ToRg8,
  kUnsupported,
};

struct TextureFormatInfo {
  uint8_t block_width = 1;
  uint8_t block_height = 1;
  uint8_t bytes_per_block = 4;  // Guest.
  uint8_t host_bytes_per_block = 4;
  uint8_t host_block_width = 1;  // 4 for BC formats copied as-is.
  uint8_t host_block_height = 1;
  VkFormat host_format = VK_FORMAT_UNDEFINED;
  VkFormat host_format_signed = VK_FORMAT_UNDEFINED;  // SNORM view, if any.
  TextureConversion conversion = TextureConversion::kUnsupported;
};

// Picks the host representation of a guest format. `bc_supported` decides
// whether DXT formats are uploaded compressed or decompressed on the CPU.
TextureFormatInfo GetTextureFormatInfo(xenos::TextureFormat format, bool bc_supported);

// Everything about a guest texture that determines its host image.
struct GuestTexture {
  uint32_t base_address = 0;  // bytes, 0 if no base level
  uint32_t mip_address = 0;   // bytes, 0 if no mips
  xenos::TextureFormat format = xenos::TextureFormat::k_8_8_8_8;
  xenos::DataDimension dimension = xenos::DataDimension::k2DOrStacked;
  xenos::Endian endian = xenos::Endian::kNone;
  uint32_t width = 1, height = 1, depth = 1;  // depth = layers for stacked/cube
  uint32_t pitch_texels = 0;                  // base level row pitch
  uint32_t mip_min_level = 0, mip_max_level = 0;
  bool tiled = false;
  bool packed_mips = false;

  bool operator==(const GuestTexture&) const = default;
};

bool GuestTextureFromFetch(const xenos::xe_gpu_texture_fetch_t& fetch, GuestTexture& out);

// Byte ranges of guest memory the texture reads, for hashing / watching.
struct GuestTextureExtent {
  uint32_t base_start = 0, base_size = 0;
  uint32_t mip_start = 0, mip_size = 0;
};
GuestTextureExtent GetGuestTextureExtent(const GuestTexture& t);

// One host upload region per (level, layer); offsets relative to the start of
// the destination buffer.
struct HostTextureRegion {
  uint32_t level = 0;
  uint32_t layer = 0;
  uint32_t width = 1, height = 1, depth = 1;  // texels, host
  uint64_t buffer_offset = 0;
  uint32_t row_length_texels = 0;             // for VkBufferImageCopy::bufferRowLength
};

// Size of the host staging data for all levels the host image holds
// (levels mip_min_level..mip_max_level, stored as host levels 0..n).
uint64_t GetHostTextureSize(const GuestTexture& t, const TextureFormatInfo& info,
                            std::vector<HostTextureRegion>& regions);

// Untiles, endian-swaps and converts all regions into `dst`.
void DecodeGuestTexture(const uint8_t* guest_memory_base, const GuestTexture& t,
                        const TextureFormatInfo& info, const std::vector<HostTextureRegion>& regions,
                        uint8_t* dst);

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2);
int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                      uint32_t bytes_per_block_log2);

}  // namespace carbon::gpu
