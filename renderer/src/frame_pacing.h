#pragma once

#include <cstdint>

namespace carbon::gpu {

// The game's vblank waits already pace it at the guest refresh rate. A second
// independent cap at that rate can delay command processing past the next tick.
inline bool NeedsHostFrameCap(int32_t cap, bool guest_vsync, double refresh_hz) {
  return cap > 0 && (!guest_vsync || double(cap) + 0.01 < refresh_hz);
}

}  // namespace carbon::gpu
