// Differential test: link the pre-change decoder with its exported functions
// renamed to *Reference (see docs/hitch-reduction.md for the snapshot recipe).
#include "texture_decode.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace carbon::gpu {
void DecodeGuestTextureReference(const uint8_t*, const GuestTexture&, const TextureFormatInfo&,
                                const std::vector<HostTextureRegion>&, uint8_t*);
}

int main() {
  using namespace carbon::gpu;
  using F = xenos::TextureFormat;
  std::vector<uint8_t> mem(64u << 20);
  for (uint32_t i = 0; i < mem.size(); ++i) mem[i] = uint8_t((i * 167 + 13) ^ (i >> 9) ^ (i >> 17));
  uint32_t cases = 0;
  for (F fmt : {F::k_8, F::k_8_8, F::k_8_8_8_8, F::k_16_16_16_16,
                F::k_32_32_32_32_FLOAT, F::k_DXT1, F::k_DXT2_3, F::k_DXT4_5,
                F::k_5_6_5, F::k_CTX1, F::k_DXT3A}) {
    for (bool bc : {false, true}) for (bool tiled : {false, true}) {
      for (bool packed : {false, true}) for (uint32_t width : {13u, 32u, 73u, 257u}) {
        for (auto dim : {xenos::DataDimension::k2DOrStacked,
                         xenos::DataDimension::k3D, xenos::DataDimension::kCube}) {
          for (uint32_t endian = 0; endian < 4; ++endian) for (uint32_t min_level : {0u, 2u}) {
            GuestTexture t;
            t.base_address = 0x10000; t.mip_address = 0x200000;
            t.width = width; t.height = width / 2 + 7;
            t.pitch_texels = AlignUp(width, 32);
            t.depth = dim == xenos::DataDimension::kCube ? 6 :
                      (dim == xenos::DataDimension::k3D ? 5 : 1);
            t.dimension = dim; t.format = fmt; t.endian = xenos::Endian(endian);
            t.tiled = tiled; t.packed_mips = packed;
            t.mip_min_level = min_level; t.mip_max_level = 4;
            auto info = GetTextureFormatInfo(fmt, bc);
            std::vector<HostTextureRegion> regions;
            auto size = GetHostTextureSize(t, info, regions);
            std::vector<uint8_t> actual(size + 128, 0xD7), expected(size + 128, 0xD7);
            DecodeGuestTextureReference(mem.data(), t, info, regions, expected.data() + 64);
            DecodeGuestTexture(mem.data(), t, info, regions, actual.data() + 64);
            if (actual != expected) {
              std::fprintf(stderr, "decode mismatch fmt=%u bc=%d tiled=%d packed=%d w=%u dim=%u endian=%u min=%u\n",
                           uint32_t(fmt), bc, tiled, packed, width, uint32_t(dim), endian, min_level);
              return 1;
            }
            ++cases;
          }
        }
      }
    }
  }
  std::printf("texture decode differential tests passed: %u cases\n", cases);
  // Same memory, texture and output on this CPU; median over repeated decodes.
  // This measures decode throughput alone, not game FPS or Vulkan allocation.
  for (F fmt : {F::k_8_8_8_8, F::k_DXT1, F::k_DXT4_5}) {
    GuestTexture t;
    t.base_address = 0x10000; t.width = t.height = t.pitch_texels = 1024;
    t.format = fmt; t.tiled = true; t.endian = xenos::Endian::k8in16;
    auto info = GetTextureFormatInfo(fmt, true);
    std::vector<HostTextureRegion> regions;
    std::vector<uint8_t> output(GetHostTextureSize(t, info, regions));
    double times[2];
    for (uint32_t path = 0; path < 2; ++path) {
      std::vector<double> samples;
      for (int run = 0; run < 15; ++run) {
        auto start = std::chrono::steady_clock::now();
        if (path == 0) DecodeGuestTextureReference(mem.data(), t, info, regions, output.data());
        else DecodeGuestTexture(mem.data(), t, info, regions, output.data());
        samples.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
      }
      std::sort(samples.begin(), samples.end());
      times[path] = samples[samples.size() / 2];
    }
    std::printf("1024x1024 tiled format %u: reference %.3f ms, optimized %.3f ms\n",
                uint32_t(fmt), times[0], times[1]);
  }
}
