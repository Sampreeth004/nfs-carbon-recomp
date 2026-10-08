#include "renderer/src/texture_copy.h"
#include "renderer/src/frame_pacing.h"

#include <array>
#include <cstdio>
#include <cstdlib>

#if defined(__linux__)
#include <sys/mman.h>
#endif

static void Check(bool ok) {
  if (!ok) { std::fputs("texture copy/pacing test failed\n", stderr); std::exit(1); }
}

int main() {
  std::array<uint8_t, 8192> mem;
  for (uint32_t i = 0; i < mem.size(); ++i) mem[i] = uint8_t((i * 167) ^ (i >> 7));
  std::array<uint8_t, 4128> actual, expected;
  for (uint32_t addr = 0; addr < 32; ++addr) {
    for (uint32_t size = 0; size <= 4096; ++size) {
      for (uint32_t mask = 0; mask < 4; ++mask) {
        actual.fill(0xD7); expected.fill(0xD7);
        for (uint32_t i = 0; i < size; ++i) expected[i + 13] = mem[(addr + i) ^ mask];
        carbon::gpu::CopyTextureBytes(mem.data(), addr, size, mask, actual.data() + 13);
        Check(actual == expected);
      }
    }
  }
#if defined(__linux__)
  // Reserve the physical address window, touching only its first and last
  // pages, to verify wrap without committing 512 MiB of test memory.
  constexpr uint32_t window = 0x20000000;
  auto* wrapped = static_cast<uint8_t*>(mmap(nullptr, window, PROT_READ | PROT_WRITE,
                                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  Check(wrapped != MAP_FAILED);
  for (uint32_t i = 0; i < 64; ++i) {
    wrapped[i] = uint8_t(i * 73);
    wrapped[window - 64 + i] = uint8_t(i * 29 + 7);
  }
  for (uint32_t mask = 0; mask < 4; ++mask) {
    for (uint32_t addr = window - 32; addr < window; ++addr) {
      actual.fill(0xD7); expected.fill(0xD7);
      for (uint32_t i = 0; i < 48; ++i) expected[i + 13] = wrapped[((addr + i) ^ mask) & (window - 1)];
      carbon::gpu::CopyTextureBytes(wrapped, addr, 48, mask, actual.data() + 13);
      Check(actual == expected);
    }
  }
  Check(munmap(wrapped, window) == 0);
#endif
  using carbon::gpu::NeedsHostFrameCap;
  Check(!NeedsHostFrameCap(60, true, 60.0));
  Check(!NeedsHostFrameCap(60, true, 59.94));
  Check(!NeedsHostFrameCap(120, true, 60.0));
  Check(NeedsHostFrameCap(30, true, 60.0));
  Check(NeedsHostFrameCap(60, true, 120.0));
  Check(NeedsHostFrameCap(60, false, 60.0));
  Check(!NeedsHostFrameCap(0, false, 60.0));
  Check(!NeedsHostFrameCap(0, true, 60.0));
  std::puts("texture copy and frame pacing tests passed");
}
