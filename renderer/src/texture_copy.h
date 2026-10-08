// Copy guest texture bytes with the Xbox byte/halfword ordering.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace carbon::gpu {

inline void CopyTextureBytes(const uint8_t* mem, uint32_t addr, uint32_t size,
                             uint32_t xor_mask, uint8_t* dst) {
  addr &= 0x1FFFFFFF;
  // Packed sub-word texels and physical-memory wrap need address-relative XOR.
  if (size > 0x20000000u - addr || (xor_mask && (addr & 3))) {
    for (uint32_t i = 0; i < size; ++i) dst[i] = mem[((addr + i) ^ xor_mask) & 0x1FFFFFFF];
    return;
  }
  if (!xor_mask) {
    std::memcpy(dst, mem + addr, size);
    return;
  }
  uint32_t i = 0;
#if defined(__aarch64__)
  for (; i + 16 <= size; i += 16) {
    uint8x16_t v = vld1q_u8(mem + addr + i);
    if (xor_mask == 1) v = vrev16q_u8(v);
    else if (xor_mask == 3) v = vrev32q_u8(v);
    else v = vrev32q_u8(vrev16q_u8(v));  // Swap 16-bit halves of each dword.
    vst1q_u8(dst + i, v);
  }
#endif
  for (; i + 4 <= size; i += 4) {
    uint32_t v;
    std::memcpy(&v, mem + addr + i, 4);
    if (xor_mask == 1) v = ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    else if (xor_mask == 3) v = std::byteswap(v);
    else v = (v << 16) | (v >> 16);
    std::memcpy(dst + i, &v, 4);
  }
  for (; i < size; ++i) dst[i] = mem[(addr + i) ^ xor_mask];
}

}  // namespace carbon::gpu
