// Carbon's camera builds perspective matrices and culling planes together in
// sub_822CEBA0. Correct their horizontal scale, leaving vertical FOV unchanged.
// No extra render-target pixels or changes to reflection/shadow cameras.
#include "widescreen.h"
#include "widescreen_math.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>

#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_BOOL(carbon_widescreen, false, "Carbon",
                    "Widen the camera and preserve HUD proportions on wide displays");
REXCVAR_DEFINE_INT32(carbon_screen_width, 0, "Carbon", "Fallback display width for widescreen");
REXCVAR_DEFINE_INT32(carbon_screen_height, 0, "Carbon", "Fallback display height for widescreen");

extern "C" void __imp__sub_822CEBA0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_824F4FC8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821A9AE8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_827B7760(PPCContext& __restrict ctx, uint8_t* base);

namespace {
std::atomic<float> horizontal_scale{1.0f};

float LoadFloat(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(REX_LOAD_U32(address));
}
void StoreFloat(uint8_t* base, uint32_t address, float value) {
  REX_STORE_U32(address, std::bit_cast<uint32_t>(value));
}
nfscarbon::Matrix LoadMatrix(uint8_t* base, uint32_t address) {
  nfscarbon::Matrix matrix;
  for (uint32_t i = 0; i < 16; ++i) matrix[i] = LoadFloat(base, address + i * 4);
  return matrix;
}
void StoreMatrix(uint8_t* base, uint32_t address, const nfscarbon::Matrix& matrix) {
  for (uint32_t i = 0; i < 16; ++i) StoreFloat(base, address + i * 4, matrix[i]);
}

bool FullScreenView(uint8_t* base, uint32_t view_id) {
  // The game has 22 view slots, 480 bytes each; +464 is the render target.
  if (view_id > 21) return false;
  const uint32_t target = REX_LOAD_U32(0x82C721D0 + view_id * 480 + 464);
  return target && REX_LOAD_U32(target + 16) == REX_LOAD_U32(0x82C651F0)
                && REX_LOAD_U32(target + 20) == REX_LOAD_U32(0x82C651F4);
}

bool MatchesBuild(uint8_t* base) {
  return REX_LOAD_U32(0x822CEBA0) == 0x7D8802A6
      && REX_LOAD_U32(0x822CEBB4) == 0x9421FF60
      && REX_LOAD_U32(0x822CEF00) == 0xD3FE0018
      && REX_LOAD_U32(0x824F4FC8) == 0x7D8802A6
      && REX_LOAD_U32(0x8250AF0C) == 0x4BFEA0BD
      && REX_LOAD_U32(0x821A9AE8) == 0x7D8802A6
      && REX_LOAD_U32(0x827B77FC) == 0xC00B03C0;
}
}

void nfscarbon::ConfigureWidescreen(uint8_t* base, uint32_t width, uint32_t height) {
  horizontal_scale.store(1.0f, std::memory_order_relaxed);
  // Disabling letterboxing tells the SDK to stretch a 16:9 frame to the whole
  // surface. Always preserve the selected aspect, including retail fallback.
  rex::cvar::SetFlagByName("present_letterbox", "true");
  bool enabled = REXCVAR_GET(carbon_widescreen);
  if (enabled && !MatchesBuild(base)) {
    REXLOG_WARN("[widescreen] guest fingerprint mismatch; using retail camera");
    enabled = false;
  }
  if (!width || !height) {
    width = std::max(0, REXCVAR_GET(carbon_screen_width));
    height = std::max(0, REXCVAR_GET(carbon_screen_height));
  }
  const double screen_aspect = width && height ? double(width) / height : 16.0 / 9.0;
  const double aspect = enabled && screen_aspect > 16.0 / 9.0 && screen_aspect <= 4.0
      ? screen_aspect : 16.0 / 9.0;
  // The presenter and the video player both consult the Xbox video mode.
  // Fit its 12-bit dimensions while keeping the selected resolution height.
  const int configured_height = REXCVAR_GET(video_mode_height);
  const uint32_t display_h = std::clamp(configured_height, 480, int(4095 / aspect));
  const uint32_t display_w = uint32_t(std::lround(display_h * aspect));
  rex::cvar::SetFlagByName("video_mode_width", std::to_string(display_w));
  rex::cvar::SetFlagByName("video_mode_height", std::to_string(display_h));
  const float scale = aspect > 16.0 / 9.0
      ? float((16.0 / 9.0) / (double(display_w) / display_h)) : 1.0f;
  horizontal_scale.store(scale, std::memory_order_relaxed);
  REXLOG_INFO("[widescreen] display {}x{}, horizontal camera/HUD scale {}",
              display_w, display_h, scale);
}

extern "C" REX_FUNC(sub_822CEBA0) {
  const uint32_t matrices = ctx.r3.u32;
  const uint32_t view = ctx.r4.u32;
  const uint32_t view_id = REX_LOAD_U32(view + 8);
  __imp__sub_822CEBA0(ctx, base);
  const float scale = horizontal_scale.load(std::memory_order_relaxed);
  if (scale == 1.0f || !FullScreenView(base, view_id)) return;
  static std::atomic_flag logged = ATOMIC_FLAG_INIT;
  if (!logged.test_and_set(std::memory_order_relaxed)) {
    REXLOG_INFO("[widescreen] correcting full-screen camera view {}", view_id);
  }
  const auto camera = LoadMatrix(base, matrices);
  for (uint32_t projection_offset : {64u, 128u}) {
    auto projection = LoadMatrix(base, matrices + projection_offset);
    projection[0] *= scale;
    StoreMatrix(base, matrices + projection_offset, projection);
    const auto combined = nfscarbon::Multiply(camera, projection);
    StoreMatrix(base, matrices + projection_offset + 128, combined);
    if (projection_offset == 64) {
      const auto planes = nfscarbon::Frustum(combined);
      for (uint32_t i = 0; i < 6; ++i) {
        for (uint32_t j = 0; j < 4; ++j) {
          StoreFloat(base, matrices + 320 + i * 16 + j * 4, planes[i][j]);
        }
      }
    }
  }
  // Pixel focal length cached by the game for world-space effects.
  StoreFloat(base, view + 16, LoadFloat(base, view + 16) * scale);
}

extern "C" REX_FUNC(sub_824F4FC8) {
  const float scale = horizontal_scale.load(std::memory_order_relaxed);
  // The front-end/HUD orthographic matrix from sub_8250AD98 lives on its
  // stack and is consumed here before it is uploaded to shader constants.
  if (scale != 1.0f && ctx.lr == 0x8250AF10) {
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!logged.test_and_set(std::memory_order_relaxed)) {
      REXLOG_INFO("[widescreen] correcting HUD projection");
    }
    const uint32_t matrix = ctx.r4.u32;
    StoreFloat(base, matrix, LoadFloat(base, matrix) * scale);
    ctx.r6.u64 = 1;  // Refresh constants even if the stack pointer was reused.
  }
  __imp__sub_824F4FC8(ctx, base);
}

extern "C" REX_FUNC(sub_821A9AE8) {
  const uint32_t output = ctx.r5.u32;
  __imp__sub_821A9AE8(ctx, base);
  const float scale = horizontal_scale.load(std::memory_order_relaxed);
  // Projects through the camera's retail horizontal FOV for screen markers.
  if (scale != 1.0f) StoreFloat(base, output, LoadFloat(base, output) * scale);
}

extern "C" REX_FUNC(sub_827B7760) {
  const uint32_t player = ctx.r3.u32;
  __imp__sub_827B7760(ctx, base);
  const float scale = horizontal_scale.load(std::memory_order_relaxed);
  // Retail uses 16:9 / back-buffer aspect for the movie player's pixel aspect.
  // Supply the physical display's pixel aspect so movies retain their shape.
  if (scale != 1.0f && ctx.r3.s32 >= 0) {
    StoreFloat(base, player + 140, LoadFloat(base, player + 140) / scale);
  }
}
