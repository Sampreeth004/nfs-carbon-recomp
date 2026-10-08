// Carbon native renderer: guest texture layout and CPU-side conversion.

#include "texture_decode.h"
#include "texture_copy.h"

#include <algorithm>
#include <cmath>

namespace carbon::gpu {

namespace {

uint32_t NextPow2(uint32_t v) {
  if (v <= 1) return 1;
  return uint32_t(1) << Log2Ceil(v);
}

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v >>= 1) ++r;
  return r;
}

uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
  uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Offset of a packed mip within the tail, in texels (converted to blocks by
// the caller). Returns false if the level is not packed.
bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t depth, uint32_t mip,
                     uint32_t& x, uint32_t& y, uint32_t& z) {
  uint32_t log2_w = Log2Ceil(width);
  uint32_t log2_h = Log2Ceil(height);
  uint32_t log2_size = std::min(log2_w, log2_h);
  x = y = z = 0;
  if (log2_size > 4 + mip) {
    return false;
  }
  uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
  uint32_t packed_mip = mip - packed_mip_base;
  if (packed_mip < 3) {
    if (log2_w > log2_h) {
      y = 16 >> packed_mip;
    } else {
      x = 16 >> packed_mip;
    }
  } else {
    uint32_t offset;
    if (log2_w > log2_h) {
      offset = (uint32_t(1) << (log2_w - packed_mip_base)) >> (packed_mip - 2);
      x = offset;
    } else {
      offset = (uint32_t(1) << (log2_h - packed_mip_base)) >> (packed_mip - 2);
      y = offset;
    }
    if (offset < 4) {
      uint32_t log2_d = Log2Ceil(depth);
      z = log2_d > 1 + packed_mip ? (log2_d - packed_mip) * 4 : 4;
    }
  }
  return true;
}

struct LevelLocation {
  uint32_t storage_base = 0;      // bytes
  uint32_t row_pitch_bytes = 0;   // linear
  uint32_t row_pitch_blocks = 0;  // tiled, 32-aligned
  uint32_t z_rows_blocks = 0;     // block rows per depth slice, 32-aligned
  uint32_t slice_stride = 0;      // array layer stride (bytes)
  uint32_t x_blocks = 0, y_blocks = 0, z = 0;  // packed tail offset
};

LevelLocation LocateLevel(const GuestTexture& t, const TextureFormatInfo& info, uint32_t level) {
  LevelLocation loc;
  const uint32_t bw = info.block_width, bh = info.block_height, bpb = info.bytes_per_block;
  const bool is_3d = t.dimension == xenos::DataDimension::k3D;
  const uint32_t layers = is_3d ? 1 : t.depth;
  const uint32_t packed_level = t.packed_mips ? PackedMipLevel(t.width, t.height) : UINT32_MAX;
  const bool is_base = level == 0;
  uint32_t storage_level = std::min(level, packed_level);

  auto strides = [&](bool base, uint32_t lvl, uint32_t& row_pitch_bytes, uint32_t& row_pitch_blocks,
                     uint32_t& z_rows, uint32_t& slice) {
    uint32_t row_texels, z_texels;
    if (base) {
      row_texels = t.pitch_texels;
      z_texels = t.height;
    } else {
      row_texels = std::max(NextPow2(t.width) >> lvl, uint32_t(1));
      z_texels = std::max(NextPow2(t.height) >> lvl, uint32_t(1));
    }
    row_pitch_blocks = AlignUp((row_texels + bw - 1) / bw, 32);
    row_pitch_bytes = row_pitch_blocks * bpb;
    if (!t.tiled && !base) {
      row_pitch_bytes = AlignUp(row_pitch_bytes, 256);
    }
    z_rows = t.dimension == xenos::DataDimension::k1D ? 1 : AlignUp((z_texels + bh - 1) / bh, 32);
    slice = row_pitch_bytes * z_rows;
    if (is_3d) {
      slice *= AlignUp(t.depth, 4);
    }
    slice = AlignUp(slice, 4096);
  };

  if (is_base) {
    loc.storage_base = t.base_address;
    strides(true, 0, loc.row_pitch_bytes, loc.row_pitch_blocks, loc.z_rows_blocks,
            loc.slice_stride);
  } else {
    // When the whole texture is a packed tail, the mips' tail is stored like
    // a level 0 (with mip strides) under mip_address.
    uint32_t mip_storage_level = packed_level == 0 ? 0 : storage_level;
    uint32_t offset = 0;
    for (uint32_t l = 1; l < mip_storage_level; ++l) {
      uint32_t rpb, rpk, zr, sl;
      strides(false, l, rpb, rpk, zr, sl);
      offset += sl * layers;
    }
    loc.storage_base = t.mip_address + offset;
    strides(false, mip_storage_level, loc.row_pitch_bytes, loc.row_pitch_blocks, loc.z_rows_blocks,
            loc.slice_stride);
  }
  if (level >= packed_level) {
    uint32_t x, y, z;
    PackedMipOffset(t.width, t.height, is_3d ? t.depth : 1, level, x, y, z);
    loc.x_blocks = x / bw;
    loc.y_blocks = y / bh;
    loc.z = z;
  }
  return loc;
}

uint32_t EndianXor(xenos::Endian e) {
  switch (e) {
    case xenos::Endian::k8in16: return 1;
    case xenos::Endian::k8in32: return 3;
    case xenos::Endian::k16in32: return 2;
    default: return 0;
  }
}

inline uint8_t Expand(uint32_t v, uint32_t bits) {
  return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

float Float20e4To32(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    // Denormal.
    uint32_t shift = 20 - Log2Floor(mantissa);
    exponent = 1 - shift;
    mantissa = (mantissa << shift) & 0xFFFFF;
  }
  uint32_t bits = ((exponent + 112) << 23) | (mantissa << 3);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// ---- Software BC decoding (only used when the device lacks BC support) ----

void DecodeBc1Colors(const uint8_t* b, uint8_t out[16][4], bool bc1_alpha) {
  uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
  uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
  uint8_t pal[4][4];
  auto unpack = [](uint16_t c, uint8_t* o) {
    o[0] = Expand((c >> 11) & 31, 5);
    o[1] = Expand((c >> 5) & 63, 6);
    o[2] = Expand(c & 31, 5);
    o[3] = 255;
  };
  unpack(c0, pal[0]);
  unpack(c1, pal[1]);
  if (c0 > c1 || !bc1_alpha) {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i] + 1) / 3);
      pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i] + 1) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
      pal[3][i] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
  uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) |
                 (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    std::memcpy(out[i], pal[(idx >> (i * 2)) & 3], 4);
  }
}

void DecodeBc4(const uint8_t* b, uint8_t out[16]) {
  uint8_t a0 = b[0], a1 = b[1];
  uint8_t pal[8];
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) out[i] = pal[(bits >> (3 * i)) & 7];
}

}  // namespace

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2) {
  // Xbox 360 2D tiling (XGAddress2DTiledOffset), as in Xenia's texture util.
  pitch = AlignUp(pitch, 32);
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bpb_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                      uint32_t bpb_log2) {
  pitch = AlignUp(pitch, 32);
  height = AlignUp(height, 32);
  int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  int32_t macro = ((((x >> 5) + macro_outer) << (bpb_log2 + 6)) & 0xFFFFFFF) << 1;
  int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bpb_log2 + 6)) >> 6;
  int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << (bpb_log2 + 6)) +
                    ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

TextureFormatInfo GetTextureFormatInfo(xenos::TextureFormat format, bool bc_supported) {
  using F = xenos::TextureFormat;
  TextureFormatInfo i;
  auto set = [&](uint8_t bw, uint8_t bh, uint8_t bpb, VkFormat host, uint8_t host_bpb,
                 TextureConversion conv, VkFormat snorm = VK_FORMAT_UNDEFINED) {
    i.block_width = bw;
    i.block_height = bh;
    i.bytes_per_block = bpb;
    i.host_format = host;
    i.host_bytes_per_block = host_bpb;
    i.conversion = conv;
    i.host_format_signed = snorm;
    i.host_block_width = 1;
    i.host_block_height = 1;
  };
  auto bc = [&](uint8_t bpb, VkFormat host, VkFormat snorm, TextureConversion soft,
                VkFormat soft_format, uint8_t soft_bpp) {
    if (bc_supported) {
      set(4, 4, bpb, host, bpb, TextureConversion::kNone, snorm);
      i.host_block_width = 4;
      i.host_block_height = 4;
    } else {
      set(4, 4, bpb, soft_format, soft_bpp, soft);
    }
  };
  switch (format) {
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      set(1, 1, 1, VK_FORMAT_R8_UNORM, 1, TextureConversion::kNone, VK_FORMAT_R8_SNORM);
      break;
    case F::k_1_5_5_5:
      set(1, 1, 2, VK_FORMAT_R8G8B8A8_UNORM, 4, TextureConversion::kR5G5B5A1);
      break;
    case F::k_5_6_5:
      set(1, 1, 2, VK_FORMAT_R8G8B8A8_UNORM, 4, TextureConversion::kR5G6B5);
      break;
    case F::k_6_5_5:
      set(1, 1, 2, VK_FORMAT_R8G8B8A8_UNORM, 4, TextureConversion::kR6G5B5);
      break;
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
    case F::k_8_8_8_8_GAMMA_EDRAM:
      set(1, 1, 4, VK_FORMAT_R8G8B8A8_UNORM, 4, TextureConversion::kNone,
          VK_FORMAT_R8G8B8A8_SNORM);
      break;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      set(1, 1, 4, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, TextureConversion::kNone);
      break;
    case F::k_8_8:
      set(1, 1, 2, VK_FORMAT_R8G8_UNORM, 2, TextureConversion::kNone, VK_FORMAT_R8G8_SNORM);
      break;
    case F::k_4_4_4_4:
      set(1, 1, 2, VK_FORMAT_R8G8B8A8_UNORM, 4, TextureConversion::kR4G4B4A4);
      break;
    case F::k_10_11_11:
    case F::k_10_11_11_AS_16_16_16_16:
      set(1, 1, 4, VK_FORMAT_R16G16B16A16_UNORM, 8, TextureConversion::kR10G11B11);
      break;
    case F::k_11_11_10:
    case F::k_11_11_10_AS_16_16_16_16:
      set(1, 1, 4, VK_FORMAT_R16G16B16A16_UNORM, 8, TextureConversion::kR11G11B10);
      break;
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      bc(8, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_UNDEFINED, TextureConversion::kBc1ToRgba8,
         VK_FORMAT_R8G8B8A8_UNORM, 4);
      break;
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      bc(16, VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_UNDEFINED, TextureConversion::kBc2ToRgba8,
         VK_FORMAT_R8G8B8A8_UNORM, 4);
      break;
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      bc(16, VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_UNDEFINED, TextureConversion::kBc3ToRgba8,
         VK_FORMAT_R8G8B8A8_UNORM, 4);
      break;
    case F::k_DXN:
      bc(16, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK, TextureConversion::kBc5ToRg8,
         VK_FORMAT_R8G8_UNORM, 2);
      break;
    case F::k_DXT5A:
      bc(8, VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK, TextureConversion::kBc4ToR8,
         VK_FORMAT_R8_UNORM, 1);
      break;
    case F::k_DXT3A:
    case F::k_DXT3A_AS_1_1_1_1:
      set(4, 4, 8, VK_FORMAT_R8_UNORM, 1, TextureConversion::kDxt3a);
      break;
    case F::k_CTX1:
      set(4, 4, 8, VK_FORMAT_R8G8_UNORM, 2, TextureConversion::kCtx1);
      break;
    case F::k_24_8:
    case F::k_16_16_16_16_EDRAM:
      set(1, 1, 4, VK_FORMAT_R32_SFLOAT, 4, TextureConversion::kDepth24);
      break;
    case F::k_24_8_FLOAT:
      set(1, 1, 4, VK_FORMAT_R32_SFLOAT, 4, TextureConversion::kDepth24Float);
      break;
    case F::k_16:
    case F::k_16_EXPAND:
      set(1, 1, 2, VK_FORMAT_R16_UNORM, 2, TextureConversion::kNone, VK_FORMAT_R16_SNORM);
      break;
    case F::k_16_16:
    case F::k_16_16_EXPAND:
    case F::k_16_16_EDRAM:
      set(1, 1, 4, VK_FORMAT_R16G16_UNORM, 4, TextureConversion::kNone, VK_FORMAT_R16G16_SNORM);
      break;
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_EXPAND:
      set(1, 1, 8, VK_FORMAT_R16G16B16A16_UNORM, 8, TextureConversion::kNone,
          VK_FORMAT_R16G16B16A16_SNORM);
      break;
    case F::k_16_FLOAT:
      set(1, 1, 2, VK_FORMAT_R16_SFLOAT, 2, TextureConversion::kNone);
      break;
    case F::k_16_16_FLOAT:
      set(1, 1, 4, VK_FORMAT_R16G16_SFLOAT, 4, TextureConversion::kNone);
      break;
    case F::k_16_16_16_16_FLOAT:
      set(1, 1, 8, VK_FORMAT_R16G16B16A16_SFLOAT, 8, TextureConversion::kNone);
      break;
    case F::k_32:
    case F::k_32_FLOAT:
    case F::k_32_AS_8:
    case F::k_32_AS_8_INTERLACED:
      set(1, 1, 4, VK_FORMAT_R32_SFLOAT, 4, TextureConversion::kNone);
      break;
    case F::k_32_32:
    case F::k_32_32_FLOAT:
    case F::k_32_AS_8_8:
    case F::k_32_AS_8_8_INTERLACED:
      set(1, 1, 8, VK_FORMAT_R32G32_SFLOAT, 8, TextureConversion::kNone);
      break;
    case F::k_32_32_32_32:
    case F::k_32_32_32_32_FLOAT:
      set(1, 1, 16, VK_FORMAT_R32G32B32A32_SFLOAT, 16, TextureConversion::kNone);
      break;
    case F::k_32_32_32_FLOAT:
      set(1, 1, 12, VK_FORMAT_R32G32B32_SFLOAT, 12, TextureConversion::kNone);
      break;
    case F::k_2_10_10_10_FLOAT_EDRAM:
      set(1, 1, 4, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, TextureConversion::kNone);
      break;
    default:
      set(1, 1, 4, VK_FORMAT_R8G8B8A8_UNORM, 4, TextureConversion::kUnsupported);
      break;
  }
  return i;
}

bool GuestTextureFromFetch(const xenos::xe_gpu_texture_fetch_t& fetch, GuestTexture& out) {
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return false;
  }
  GuestTexture t;
  t.format = fetch.format;
  t.dimension = fetch.dimension;
  t.endian = fetch.endianness;
  t.tiled = fetch.tiled != 0;
  t.packed_mips = fetch.packed_mips != 0;
  t.pitch_texels = uint32_t(fetch.pitch) << 5;
  switch (fetch.dimension) {
    case xenos::DataDimension::k1D:
      t.width = fetch.size_1d.width + 1;
      t.height = 1;
      t.depth = 1;
      break;
    case xenos::DataDimension::k2DOrStacked:
      t.width = fetch.size_2d.width + 1;
      t.height = fetch.size_2d.height + 1;
      t.depth = fetch.stacked ? fetch.size_2d.stack_depth + 1 : 1;
      break;
    case xenos::DataDimension::k3D:
      t.width = fetch.size_3d.width + 1;
      t.height = fetch.size_3d.height + 1;
      t.depth = fetch.size_3d.depth + 1;
      break;
    case xenos::DataDimension::kCube:
      t.width = fetch.size_2d.width + 1;
      t.height = fetch.size_2d.height + 1;
      t.depth = 6;
      break;
  }
  uint32_t longest = std::max(t.width, t.height);
  if (fetch.dimension == xenos::DataDimension::k3D) {
    longest = std::max(longest, t.depth);
  }
  uint32_t size_max_level = Log2Floor(longest);
  uint32_t base_page = fetch.base_address & 0x1FFFF;
  uint32_t mip_page = fetch.mip_address & 0x1FFFF;
  uint32_t mip_min, mip_max;
  if (mip_page == 0) {
    mip_min = 0;
    mip_max = 0;
  } else {
    mip_min = std::min(uint32_t(fetch.mip_min_level), size_max_level);
    mip_max = std::max(std::min(uint32_t(fetch.mip_max_level), size_max_level), mip_min);
  }
  if (mip_max != 0) {
    if (base_page == 0) {
      mip_min = std::max(mip_min, uint32_t(1));
    }
    if (mip_min != 0) {
      base_page = 0;
    }
  } else {
    mip_page = 0;
  }
  if (!base_page && !mip_page) {
    return false;
  }
  t.base_address = base_page << 12;
  t.mip_address = mip_page << 12;
  t.mip_min_level = mip_min;
  t.mip_max_level = mip_max;
  out = t;
  return true;
}

GuestTextureExtent GetGuestTextureExtent(const GuestTexture& t) {
  GuestTextureExtent e;
  TextureFormatInfo info = GetTextureFormatInfo(t.format, true);
  const uint32_t layers = t.dimension == xenos::DataDimension::k3D ? 1 : t.depth;
  if (t.base_address && t.mip_min_level == 0) {
    LevelLocation l = LocateLevel(t, info, 0);
    e.base_start = l.storage_base;
    e.base_size = l.slice_stride * layers;
  }
  if (t.mip_address && t.mip_max_level >= 1) {
    LevelLocation first = LocateLevel(t, info, std::max(t.mip_min_level, uint32_t(1)));
    LevelLocation last = LocateLevel(t, info, t.mip_max_level);
    e.mip_start = std::min(first.storage_base, t.mip_address);
    e.mip_size = last.storage_base + last.slice_stride * layers - e.mip_start;
  }
  return e;
}

uint64_t GetHostTextureSize(const GuestTexture& t, const TextureFormatInfo& info,
                            std::vector<HostTextureRegion>& regions) {
  regions.clear();
  const bool is_3d = t.dimension == xenos::DataDimension::k3D;
  const uint32_t layers = is_3d ? 1 : t.depth;
  uint64_t offset = 0;
  for (uint32_t level = t.mip_min_level; level <= t.mip_max_level; ++level) {
    uint32_t w = std::max(t.width >> level, uint32_t(1));
    uint32_t h = std::max(t.height >> level, uint32_t(1));
    uint32_t d = is_3d ? std::max(t.depth >> level, uint32_t(1)) : 1;
    // Host images of block-compressed formats need whole blocks.
    uint32_t hw = info.host_block_width > 1 ? AlignUp(w, info.host_block_width) : w;
    uint32_t hh = info.host_block_height > 1 ? AlignUp(h, info.host_block_height) : h;
    uint32_t row_bytes = (hw / info.host_block_width) * info.host_bytes_per_block;
    uint32_t rows = hh / info.host_block_height;
    for (uint32_t layer = 0; layer < layers; ++layer) {
      HostTextureRegion r;
      r.level = level - t.mip_min_level;
      r.layer = layer;
      r.width = w;
      r.height = h;
      r.depth = d;
      r.row_length_texels = hw;
      r.buffer_offset = offset;
      regions.push_back(r);
      offset += AlignUp64(uint64_t(row_bytes) * rows * d, 16);
    }
  }
  return offset;
}

void DecodeGuestTexture(const uint8_t* mem, const GuestTexture& t, const TextureFormatInfo& info,
                        const std::vector<HostTextureRegion>& regions, uint8_t* dst) {
  const uint32_t bw = info.block_width, bh = info.block_height, bpb = info.bytes_per_block;
  const uint32_t bpb_log2 = Log2Floor(bpb);
  const bool is_3d = t.dimension == xenos::DataDimension::k3D;
  const uint32_t xor_mask = EndianXor(t.endian);
  const bool is_pow2_bpb = (bpb & (bpb - 1)) == 0;
  uint8_t block[16];

  for (const HostTextureRegion& r : regions) {
    uint32_t level = r.level + t.mip_min_level;
    LevelLocation loc = LocateLevel(t, info, level);
    uint32_t wb = (r.width + bw - 1) / bw;
    uint32_t hb = (r.height + bh - 1) / bh;
    uint8_t* out = dst + r.buffer_offset;
    // Host layout: rows of host blocks (or texels when converting from blocks).
    const bool host_is_blocks = info.host_block_width > 1;
    const uint32_t host_row_texels = r.row_length_texels;
    const uint32_t host_row_bytes =
        host_is_blocks ? (host_row_texels / info.host_block_width) * info.host_bytes_per_block
                       : host_row_texels * info.host_bytes_per_block;
    const uint32_t host_rows = host_is_blocks
                                   ? AlignUp(r.height, info.host_block_height) / info.host_block_height
                                   : r.height;
    for (uint32_t z = 0; z < r.depth; ++z) {
      uint8_t* out_slice = out + uint64_t(host_row_bytes) * host_rows * z;
      for (uint32_t by = 0; by < hb; ++by) {
        if (info.conversion == TextureConversion::kNone &&
            (host_is_blocks || (bw == 1 && bh == 1))) {
          // Native BC blocks and unconverted texels need only an endian copy.
          // 2D tiling has contiguous runs within each microtile; do not cross
          // their boundary. 3D tiled blocks retain individual address lookup.
          for (uint32_t bx = 0; bx < wb;) {
            uint32_t gx = bx + loc.x_blocks, gy = by + loc.y_blocks;
            uint32_t off, run;
            if (t.tiled && is_pow2_bpb) {
              if (is_3d) {
                off = uint32_t(TiledOffset3D(int32_t(gx), int32_t(gy), int32_t(z + loc.z),
                                            loc.row_pitch_blocks, loc.z_rows_blocks, bpb_log2));
                run = 1;
              } else {
                off = uint32_t(TiledOffset2D(int32_t(gx), int32_t(gy), loc.row_pitch_blocks,
                                            bpb_log2)) + r.layer * loc.slice_stride;
                uint32_t micro_run = std::min(8u, std::max(1u, 16u / bpb));
                run = std::min(wb - bx, micro_run - (gx & (micro_run - 1)));
              }
            } else {
              off = r.layer * loc.slice_stride + (z + loc.z) * loc.row_pitch_bytes *
                    loc.z_rows_blocks + gy * loc.row_pitch_bytes + gx * bpb;
              run = wb - bx;
            }
            CopyTextureBytes(mem, loc.storage_base + off, run * bpb, xor_mask,
                             out_slice + uint64_t(by) * host_row_bytes + uint64_t(bx) * bpb);
            bx += run;
          }
          continue;
        }
        for (uint32_t bx = 0; bx < wb; ++bx) {
          uint32_t gx = bx + loc.x_blocks, gy = by + loc.y_blocks, gz = z + loc.z;
          uint32_t off;
          if (t.tiled && is_pow2_bpb) {
            if (is_3d) {
              off = uint32_t(TiledOffset3D(int32_t(gx), int32_t(gy), int32_t(gz),
                                           loc.row_pitch_blocks, loc.z_rows_blocks, bpb_log2));
            } else {
              off = uint32_t(TiledOffset2D(int32_t(gx), int32_t(gy), loc.row_pitch_blocks,
                                           bpb_log2)) +
                    r.layer * loc.slice_stride;
            }
          } else {
            off = r.layer * loc.slice_stride + gz * loc.row_pitch_bytes * loc.z_rows_blocks +
                  gy * loc.row_pitch_bytes + gx * bpb;
          }
          uint32_t addr = loc.storage_base + off;
          CopyTextureBytes(mem, addr, bpb, xor_mask, block);
          // Write to the host image.
          switch (info.conversion) {
            case TextureConversion::kNone: {
              if (host_is_blocks || (bw == 1 && bh == 1)) {
                uint8_t* o = out_slice + uint64_t(by) * host_row_bytes + uint64_t(bx) * bpb;
                std::memcpy(o, block, bpb);
              }
            } break;
            case TextureConversion::kR5G6B5:
            case TextureConversion::kR6G5B5:
            case TextureConversion::kR5G5B5A1:
            case TextureConversion::kR4G4B4A4: {
              uint32_t v = uint32_t(block[0]) | (uint32_t(block[1]) << 8);
              uint8_t* o = out_slice + uint64_t(by) * host_row_bytes + uint64_t(bx) * 4;
              switch (info.conversion) {
                case TextureConversion::kR5G6B5:
                  o[0] = Expand(v & 31, 5); o[1] = Expand((v >> 5) & 63, 6);
                  o[2] = Expand((v >> 11) & 31, 5); o[3] = 255;
                  break;
                case TextureConversion::kR6G5B5:
                  o[0] = Expand(v & 63, 6); o[1] = Expand((v >> 6) & 31, 5);
                  o[2] = Expand((v >> 11) & 31, 5); o[3] = 255;
                  break;
                case TextureConversion::kR5G5B5A1:
                  o[0] = Expand(v & 31, 5); o[1] = Expand((v >> 5) & 31, 5);
                  o[2] = Expand((v >> 10) & 31, 5); o[3] = (v >> 15) ? 255 : 0;
                  break;
                default:
                  o[0] = Expand(v & 15, 4); o[1] = Expand((v >> 4) & 15, 4);
                  o[2] = Expand((v >> 8) & 15, 4); o[3] = Expand((v >> 12) & 15, 4);
                  break;
              }
            } break;
            case TextureConversion::kR10G11B11:
            case TextureConversion::kR11G11B10: {
              uint32_t v;
              std::memcpy(&v, block, 4);
              uint16_t* o = reinterpret_cast<uint16_t*>(out_slice + uint64_t(by) * host_row_bytes +
                                                        uint64_t(bx) * 8);
              auto ex16 = [](uint32_t x, uint32_t bits) {
                return uint16_t((uint64_t(x) * 65535 + ((1u << bits) - 1) / 2) /
                                ((1u << bits) - 1));
              };
              if (info.conversion == TextureConversion::kR10G11B11) {
                o[0] = ex16(v & 0x7FF, 11); o[1] = ex16((v >> 11) & 0x7FF, 11);
                o[2] = ex16(v >> 22, 10);
              } else {
                o[0] = ex16(v & 0x3FF, 10); o[1] = ex16((v >> 10) & 0x7FF, 11);
                o[2] = ex16(v >> 21, 11);
              }
              o[3] = 65535;
            } break;
            case TextureConversion::kDepth24:
            case TextureConversion::kDepth24Float: {
              uint32_t v;
              std::memcpy(&v, block, 4);
              float f = info.conversion == TextureConversion::kDepth24
                            ? float(v >> 8) * (1.0f / 16777215.0f)
                            : Float20e4To32(v >> 8);
              std::memcpy(out_slice + uint64_t(by) * host_row_bytes + uint64_t(bx) * 4, &f, 4);
            } break;
            default: {
              // Block formats expanded to texels.
              uint8_t texels[16][4] = {};
              uint32_t channels = info.host_bytes_per_block;
              switch (info.conversion) {
                case TextureConversion::kBc1ToRgba8:
                  DecodeBc1Colors(block, texels, true);
                  break;
                case TextureConversion::kBc2ToRgba8:
                  DecodeBc1Colors(block + 8, texels, false);
                  for (int i = 0; i < 16; ++i) {
                    texels[i][3] = Expand((block[i >> 1] >> ((i & 1) * 4)) & 15, 4);
                  }
                  break;
                case TextureConversion::kBc3ToRgba8: {
                  DecodeBc1Colors(block + 8, texels, false);
                  uint8_t a[16];
                  DecodeBc4(block, a);
                  for (int i = 0; i < 16; ++i) texels[i][3] = a[i];
                } break;
                case TextureConversion::kBc4ToR8: {
                  uint8_t a[16];
                  DecodeBc4(block, a);
                  for (int i = 0; i < 16; ++i) texels[i][0] = a[i];
                } break;
                case TextureConversion::kBc5ToRg8: {
                  uint8_t a[16], b[16];
                  DecodeBc4(block, a);
                  DecodeBc4(block + 8, b);
                  for (int i = 0; i < 16; ++i) {
                    texels[i][0] = a[i];
                    texels[i][1] = b[i];
                  }
                } break;
                case TextureConversion::kDxt3a:
                  for (int i = 0; i < 16; ++i) {
                    texels[i][0] = Expand((block[i >> 1] >> ((i & 1) * 4)) & 15, 4);
                  }
                  break;
                case TextureConversion::kCtx1: {
                  // Two 8:8 endpoints, 2-bit indices (like BC1 without colors).
                  uint8_t e[4][2] = {{block[0], block[1]}, {block[2], block[3]}};
                  for (int c = 0; c < 2; ++c) {
                    e[2][c] = uint8_t((2 * e[0][c] + e[1][c] + 1) / 3);
                    e[3][c] = uint8_t((e[0][c] + 2 * e[1][c] + 1) / 3);
                  }
                  uint32_t idx = uint32_t(block[4]) | (uint32_t(block[5]) << 8) |
                                 (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
                  for (int i = 0; i < 16; ++i) {
                    uint32_t k = (idx >> (i * 2)) & 3;
                    texels[i][0] = e[k][0];
                    texels[i][1] = e[k][1];
                  }
                } break;
                default:
                  break;
              }
              for (uint32_t py = 0; py < 4; ++py) {
                uint32_t ty = by * 4 + py;
                if (ty >= r.height) break;
                for (uint32_t px = 0; px < 4; ++px) {
                  uint32_t tx = bx * 4 + px;
                  if (tx >= r.width) break;
                  uint8_t* o =
                      out_slice + uint64_t(ty) * host_row_bytes + uint64_t(tx) * channels;
                  std::memcpy(o, texels[py * 4 + px], channels);
                }
              }
            } break;
          }
        }
      }
    }
  }
}

}  // namespace carbon::gpu
