// Bit-exact writes of guest big-endian shader constants, without PM4 side effects.
#pragma once

#include <bit>
#include <cstdint>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace carbon::gpu {

// Pointers need only dword alignment. The caller bounds the span to one shader
// stage and notifies the renderer once if any bits changed (including NaN payloads
// and signed zero). Return the number of changed dwords for diagnostics.
inline uint32_t WriteBigEndianConstants(uint32_t* dst, const uint32_t* src, uint32_t count) {
  uint32_t changed = 0;
  uint32_t i = 0;
#if defined(__aarch64__)
  uint32x4_t changes = vdupq_n_u32(0);
  for (; i + 4 <= count; i += 4) {
    uint32x4_t value = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(vld1q_u32(src + i))));
    uint32x4_t equal = vceqq_u32(vld1q_u32(dst + i), value);
    changes = vaddq_u32(changes, vshrq_n_u32(vmvnq_u32(equal), 31));
    vst1q_u32(dst + i, value);
  }
  changed = vaddvq_u32(changes);
#endif
  for (; i < count; ++i) {
    uint32_t value = std::byteswap(src[i]);
    changed += dst[i] != value;
    dst[i] = value;
  }
  return changed;
}

}  // namespace carbon::gpu
