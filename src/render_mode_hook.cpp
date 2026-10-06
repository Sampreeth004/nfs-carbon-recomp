// Android: force Carbon's retail "mode 2" scene render (one tile, no MSAA).
//
// Carbon's D3D renderer keeps a small table of render modes (renderer pointer in
// the global kRendererGlobal). Each mode has a tile count, an MSAA type, a scene
// width/height and two rectangles (render tile and resolve). The default mode
// uses predicated tiling and MSAA, so the Xenos plugin replays the whole scene
// several times. Mode 2 already exists in retail; here we select it and give it
// the chosen resolution, which cuts draws per frame by the tile factor.
//
// Table layout (arrays of 6 u32 indexed by mode, read in the renderer constructor
// sub_824FFBE0): +4 tile count, +28 width, +52 height, +76 MSAA; rect tables at
// +100 and +484, 64 bytes per mode as {x0, y0, x1, y1, ...}.
//
// Addresses were re-derived for this XEX by tools/find_fingerprints.py (docs/
// render-mode-addresses.md). Before touching anything the hooks check the code at
// those addresses; on a mismatch they log once and do nothing.
//
// Cvars: carbon_single_pass (off by default until verified on device),
// carbon_scene_resolution ("WxH", default 1280x720; height rounded up to 32 for
// the tile).

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_BOOL(carbon_single_pass, false, "Carbon",
                    "Render the scene in retail mode 2 (one tile, no MSAA)");
REXCVAR_DEFINE_STRING(carbon_scene_resolution, "1280x720", "Carbon",
                      "Scene resolution for carbon_single_pass: 1024x576, 1280x720 or WxH");

extern "C" void __imp__sub_824FFBE0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8250B200(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82500298(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kRendererGlobal = 0x82C65304;
constexpr uint32_t kOutputWidth0 = 0x82C651E8;
constexpr uint32_t kOutputHeight0 = 0x82C651EC;
constexpr uint32_t kOutputWidth1 = 0x82C651F0;
constexpr uint32_t kOutputHeight1 = 0x82C651F4;
constexpr uint32_t kMode = 2;

struct Word {
  uint32_t offset;
  uint32_t value;
  uint32_t mask;
};

// Code at the re-derived addresses (instruction shape; branch displacements masked).
constexpr uint32_t kBranchMask = 0xFC000003;
constexpr uint32_t kBranchLink = 0x48000001;
constexpr Word kCtorWords[] = {{0x00, 0x7D8802A6, ~0u}, {0x08, 0x38E301E4, ~0u},
                               {0x0C, 0x39430064, ~0u}, {0x10, 0x38C300A4, ~0u}};
constexpr Word kSelectWords[] = {{0x00, 0x7D8802A6, ~0u}, {0x10, kBranchLink, kBranchMask}};
constexpr Word kSizeWords[] = {{0x00, 0x54AB063E, ~0u}, {0x28, 0x2F040000, ~0u}};

template <size_t N>
bool Matches(uint8_t* base, uint32_t address, const Word (&words)[N]) {
  for (const Word& w : words) {
    if ((REX_LOAD_U32(address + w.offset) & w.mask) != w.value) {
      return false;
    }
  }
  return true;
}

std::pair<uint32_t, uint32_t> SceneSize() {
  unsigned width = 0, height = 0;
  if (std::sscanf(REXCVAR_GET(carbon_scene_resolution).c_str(), "%ux%u", &width, &height) != 2 ||
      width < 320 || width > 3840 || height < 180 || height > 2160) {
    return {1280, 720};
  }
  return {width, height};
}

// Verified once, lazily, because guest memory is loaded by the first call.
bool Enabled(uint8_t* base) {
  static const bool enabled = [base]() {
    if (!REXCVAR_GET(carbon_single_pass)) {
      return false;
    }
    if (!Matches(base, 0x824FFBE0, kCtorWords) || !Matches(base, 0x8250B200, kSelectWords) ||
        !Matches(base, 0x82500298, kSizeWords)) {
      REXLOG_WARN("[render_mode] fingerprint mismatch, single-pass disabled");
      return false;
    }
    return true;
  }();
  return enabled;
}

void WriteOutputSize(uint8_t* base) {
  auto [width, height] = SceneSize();
  REX_STORE_U32(kOutputWidth0, width);
  REX_STORE_U32(kOutputHeight0, height);
  REX_STORE_U32(kOutputWidth1, width);
  REX_STORE_U32(kOutputHeight1, height);
}

}  // namespace

// Renderer constructor: after the original fills the table, rewrite mode 2.
extern "C" REX_FUNC(sub_824FFBE0) {
  const uint32_t renderer = ctx.r3.u32;
  __imp__sub_824FFBE0(ctx, base);
  if (!Enabled(base)) {
    return;
  }
  auto [width, height] = SceneSize();
  const uint32_t tile_height = (height + 31) & ~31u;
  REX_STORE_U32(renderer + 4 + kMode * 4, 1);
  REX_STORE_U32(renderer + 28 + kMode * 4, width);
  REX_STORE_U32(renderer + 52 + kMode * 4, tile_height);
  REX_STORE_U32(renderer + 76 + kMode * 4, 0);
  for (uint32_t table : {100u, 484u}) {
    const uint32_t rect = renderer + table + kMode * 64;
    REX_STORE_U32(rect + 0, 0);
    REX_STORE_U32(rect + 4, 0);
    REX_STORE_U32(rect + 8, width);
    REX_STORE_U32(rect + 12, height);
  }
  REXLOG_WARN("[render_mode] mode 2: {}x{}, 1 tile, MSAA off", width, height);
}

// Mode select: after the original picks a mode, force mode 2.
extern "C" REX_FUNC(sub_8250B200) {
  __imp__sub_8250B200(ctx, base);
  if (!Enabled(base)) {
    return;
  }
  WriteOutputSize(base);
  const uint32_t renderer = REX_LOAD_U32(kRendererGlobal);
  if (renderer) {
    REX_STORE_U32(renderer, kMode);
  }
}

// Output size: ask for mode 2 (r4 = 2, r5 = 1, as the retail caller does for it).
extern "C" REX_FUNC(sub_82500298) {
  if (Enabled(base)) {
    ctx.r4.u64 = kMode;
    ctx.r5.u64 = 1;
  }
  __imp__sub_82500298(ctx, base);
  if (Enabled(base)) {
    WriteOutputSize(base);
  }
}
