// Carbon native renderer: shared helpers.
//
// The renderer reads the game's PM4 command ring itself and draws with Vulkan
// directly: real render targets instead of EDRAM emulation, a translator from
// Xenos microcode to GLSL (compiled to SPIR-V once and cached), and a texture
// cache keyed by guest address and contents. Only rexruntime is used from the
// SDK (Vulkan provider/presenter, guest memory, kernel threads); the Xenos
// emulation plugin is not loaded. The Xenos register and microcode layouts
// come from the SDK's public BSD-licensed headers (xenos.h, registers.h,
// ucode.h).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>

namespace carbon::gpu {

namespace xenos = rex::graphics::xenos;
namespace reg = rex::graphics::reg;

constexpr uint32_t kRegisterCount = 0x5003;

// Base register indices of the shader constant blocks.
constexpr uint32_t kRegFloatConstants = 0x4000;  // 512 vec4: VS 0-255, PS 256-511
constexpr uint32_t kRegFetchConstants = 0x4800;  // 32 x 6 dwords
constexpr uint32_t kRegBoolConstants = 0x4900;   // 8 dwords = 256 bits
constexpr uint32_t kRegLoopConstants = 0x4908;   // 32 dwords

struct RegisterFile {
  uint32_t values[kRegisterCount] = {};

  uint32_t operator[](uint32_t index) const { return values[index]; }
  uint32_t& operator[](uint32_t index) { return values[index]; }

  template <typename T>
  T Get() const {
    T value;
    value.value = values[T::register_index];
    return value;
  }
  template <typename T>
  T Get(uint32_t index) const {
    T value;
    value.value = values[index];
    return value;
  }
  float GetFloat(uint32_t index) const {
    float f;
    std::memcpy(&f, &values[index], sizeof(f));
    return f;
  }
  xenos::xe_gpu_vertex_fetch_t GetVertexFetch(uint32_t index) const {
    xenos::xe_gpu_vertex_fetch_t fetch;
    std::memcpy(&fetch, &values[kRegFetchConstants + index * 2], sizeof(fetch));
    return fetch;
  }
  xenos::xe_gpu_texture_fetch_t GetTextureFetch(uint32_t index) const {
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, &values[kRegFetchConstants + index * 6], sizeof(fetch));
    return fetch;
  }
};

inline uint32_t ByteSwap32(uint32_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
  return _byteswap_ulong(v);
#else
  return __builtin_bswap32(v);
#endif
}

inline uint16_t ByteSwap16(uint16_t v) { return uint16_t((v >> 8) | (v << 8)); }

inline uint32_t GpuSwap32(uint32_t v, xenos::Endian endian) {
  switch (endian) {
    case xenos::Endian::k8in16:
      return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    case xenos::Endian::k8in32:
      return ByteSwap32(v);
    case xenos::Endian::k16in32:
      return (v >> 16) | (v << 16);
    default:
      return v;
  }
}

inline uint32_t Log2Ceil(uint32_t v) {
  uint32_t r = 0;
  while ((uint32_t(1) << r) < v) {
    ++r;
  }
  return r;
}

inline uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
inline uint64_t AlignUp64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

uint64_t HashBytes(const void* data, size_t size, uint64_t seed = 0);

}  // namespace carbon::gpu
