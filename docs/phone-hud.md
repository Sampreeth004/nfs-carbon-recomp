# Android minimap placement

Version 0.3.11 places the minimap in the upper left, below the layout/reset
toolbar and above the steering buttons. This is enabled by default on Android
and works independently of the widescreen option. Desktop builds retain the
game's layout. Custom touch layouts are not migrated or reset.

For retail placement, set `carbon_phone_minimap_top = false` in `nfscarbon.toml`
and restart the game.

## Implementation

`src/phone_hud.cpp` hooks minimap construction at `sub_82359448`. The hook checks
the supported guest build before changing any layout data. It subtracts 240 HUD
units from the cached centre at controller offset 184, which the game's icon
updates and route mesh rendering use.

The map's parent group, north indicator, mask, player pulse and car markers
receive the same offset. The map tiles retain their local coordinates. The
`RADAR_RING` and `HEAT_METER_GROUP` objects are looked up in the current HUD
package and moved with the map. The game's relative-position setter updates
animation states and invalidates transforms, avoiding position resets when
animations change. Calls use a copied guest context and preserved stack scratch
so the constructor's return registers remain intact.

The hook runs once for each new minimap controller, including controllers
created after leaving menus. It adds no per-frame scanning, renderer passes or
texture work.

## Validation

- ARM64 and x86_64 native builds passed.
- Android debug APK assembled and installed as version 0.3.11 / code 14.
- OnePlus CPH2723, 2640 x 1216 landscape: visually verified during free-roam
  driving. Roads, radar ring, player marker, north indicator and heat meter
  appear together below the top controls, with no steering-button overlap.
  Screenshots: `out/minimap-driving-before.png` and
  `out/minimap-after-v311-gameplay.png`.
- Race, pursuit and active GPS-route layouts were not separately exercised.
