#include <array>
#include <bit>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "../generated/default/nfscarbon_pch.h"

extern "C" void __imp__sub_82359448(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_822F7BA0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8238D680(PPCContext& __restrict ctx, uint8_t* base);

REXCVAR_DEFINE_BOOL(carbon_phone_minimap_top, true, "Carbon",
                    "Place the Android minimap above the steering controls");

namespace {
// Carbon's HUD uses centred coordinates. Moving up 240 units puts the radar
// below the phone's layout/reset toolbar and above the steering buttons.
constexpr float kMoveUp = -240.0f;

uint32_t FindObject(PPCContext ctx, uint8_t* base, uint32_t package, uint32_t hash) {
  ctx.r3.u64 = package;
  ctx.r4.u64 = hash;
  __imp__sub_822F7BA0(ctx, base);
  return ctx.r3.u32;
}

void MoveObject(PPCContext ctx, uint8_t* base, uint32_t object) {
  if (!object) return;
  // Use the game's relative-position setter: it also offsets animation states
  // and marks transforms dirty. Child positions remain local to their group.
  // Give the call its own aligned guest stack frame and preserve the scratch.
  const uint32_t scratch = ctx.r1.u32 - 128;
  std::array<uint32_t, 32> saved;
  for (uint32_t i = 0; i < saved.size(); ++i)
    saved[i] = REX_LOAD_U32(scratch + i * 4);
  const uint32_t delta = scratch + 96;
  REX_STORE_U32(delta, 0);
  REX_STORE_U32(delta + 4, std::bit_cast<uint32_t>(kMoveUp));
  REX_STORE_U32(delta + 8, 0);
  ctx.r1.u64 = scratch;
  ctx.r3.u64 = object;
  ctx.r4.u64 = delta;
  ctx.r5.u64 = 1;
  __imp__sub_8238D680(ctx, base);
  for (uint32_t i = 0; i < saved.size(); ++i)
    REX_STORE_U32(scratch + i * 4, saved[i]);
}
}

extern "C" REX_FUNC(sub_82359448) {
  __imp__sub_82359448(ctx, base);
  const uint32_t map = ctx.r3.u32;
  if (!REXCVAR_GET(carbon_phone_minimap_top) || !map) return;
  if (REX_LOAD_U32(0x82359448) != 0x7D8802A6 ||
      REX_LOAD_U32(0x82359454) != 0x9421FEB0 ||
      REX_LOAD_U32(0x822F7BA0) != 0x7D8802A6 ||
      REX_LOAD_U32(0x8238D680) != 0x7D8802A6 ||
      REX_LOAD_U32(0x8238D6FC) != 0x7FC5F378 ||
      REX_LOAD_U32(map) != 0x8208543C) {
    REXLOG_WARN("[phone-hud] minimap fingerprint mismatch; retaining retail layout");
    return;
  }
  const uint32_t anchor = REX_LOAD_U32(map + 184);
  REX_STORE_U32(map + 184,
      std::bit_cast<uint32_t>(std::bit_cast<float>(anchor) + kMoveUp));
  // Map tiles are children of TRACK_MAP; moving each tile would apply the
  // offset twice. Runtime icons and the route mesh use the cached centre above.
  for (uint32_t offset : {48u, 132u, 136u, 140u, 144u, 148u, 152u})
    MoveObject(ctx, base, REX_LOAD_U32(map + offset));
  const uint32_t package = REX_LOAD_U32(map + 16);
  MoveObject(ctx, base, FindObject(ctx, base, package, 0x6CD0C4B8)); // RADAR_RING
  MoveObject(ctx, base, FindObject(ctx, base, package, 0xC46A80A9)); // HEAT_METER_GROUP
  REXLOG_INFO("[phone-hud] minimap moved to upper left (Y offset {})", kMoveUp);
}
