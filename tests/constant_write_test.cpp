#include "renderer/src/constant_write.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

static void Check(bool condition) {
  if (!condition) {
    std::fputs("constant write test failed\n", stderr);
    std::exit(1);
  }
}

// Independent scalar reference, operating on arbitrary float bit patterns.
static uint32_t Swap(uint32_t x) {
  return (x << 24) | ((x & 0x0000FF00) << 8) | ((x & 0x00FF0000) >> 8) | (x >> 24);
}

int main() {
  std::mt19937 rng(0xCA4B04);
  constexpr uint32_t kCapacity = 2056;
  std::array<uint32_t, kCapacity> src, actual, expected;
  // Every length through both complete stage blocks, and all dword alignments
  // relative to a 16-byte vector. Guard words also detect stores past the span.
  for (uint32_t count = 0; count <= 2048; ++count) {
    for (uint32_t offset = 1; offset <= 4; ++offset) {
      for (uint32_t i = 0; i < kCapacity; ++i) {
        src[i] = rng();
        actual[i] = (i % 3 == 0) ? Swap(src[i]) : rng();
      }
      expected = actual;
      uint32_t changes = 0;
      for (uint32_t i = offset; i < offset + count; ++i) {
        uint32_t value = Swap(src[i]);
        changes += expected[i] != value;
        expected[i] = value;
      }
      Check(carbon::gpu::WriteBigEndianConstants(actual.data() + offset,
                                                src.data() + offset, count) == changes);
      Check(actual == expected);
      // Repeating any packet must report no changes.
      Check(carbon::gpu::WriteBigEndianConstants(actual.data() + offset,
                                                src.data() + offset, count) == 0);
      Check(actual == expected);
    }
  }
  // Compare bits rather than float equality: signed zero and NaN payloads matter.
  std::array<uint32_t, 4> floats = {0, 0x7FC00001, 0x7FA00002, 0x80000000};
  std::array<uint32_t, 4> guest = {Swap(0x80000000), Swap(0x7FC00002),
                                 Swap(0x7FA00002), Swap(0)};
  Check(carbon::gpu::WriteBigEndianConstants(floats.data(), guest.data(), 4) == 3);
  Check(floats[0] == 0x80000000 && floats[1] == 0x7FC00002 &&
        floats[2] == 0x7FA00002 && floats[3] == 0);
#if defined(__aarch64__)
  std::puts("constant write tests passed (ARM64 NEON)");
#else
  std::puts("constant write tests passed (scalar)");
#endif
}
