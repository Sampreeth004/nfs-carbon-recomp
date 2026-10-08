# Phone widescreen

Settings > Graphics > Widescreen is enabled by default. Restart the game after
changing it. It takes the actual SDL drawable size at guest startup, with the
launcher's landscape screen dimensions as a fallback. Screens wider than 16:9
get a wider horizontal view; vertical FOV stays unchanged. Other screens use
16:9 with the existing letterbox presenter. Aspect-ratio protection is automatic
in both modes; there is no separate Letterbox switch.

The scene still renders into Carbon's retail buffers. Their pixels are mapped
to the wide display after correcting camera and HUD projections. This avoids
allocating wider scene, bloom and EDRAM surfaces. It also means horizontal pixel
density is lower on very wide screens. The HUD remains in a centered region.

## Configuration

- `carbon_widescreen = true`: enable camera and HUD correction on wide screens.
- `carbon_screen_width`, `carbon_screen_height`: fallback drawable dimensions,
  written by the launcher; zero uses the actual SDL window dimensions only.
- `present_letterbox = true`: enforced at startup and written by the launcher
  to preserve proportions when bars are necessary, including old installs that
  previously saved this setting as false.

These options currently apply at startup on Android. Window resizing during a
session requires a game restart. Desktop behavior is unchanged.

## Guest hooks (this project's retail XEX)

`src/widescreen.cpp` checks instruction fingerprints before enabling changes:

| Function | Purpose |
| --- | --- |
| `0x822CEBA0` | Builds perspective matrices and frustum planes. Adjust both projection matrices' X scale, multiply by the view matrix again, and regenerate all six normalized culling planes. |
| `0x824F4FC8` | Uploads camera/object matrices. Only the HUD call returning to `0x8250AF10` is corrected. |
| `0x821A9AE8` | Projects world markers to screen coordinates using retail camera FOV. Correct its output X coordinate. |
| `0x827B7760` | Initializes the movie player. Correct its pixel aspect at +140 so pre-rendered movies keep their proportions. |

The view table starts at `0x82C721D0`, with 480-byte entries. Entry +464 points
to the render target; its +16/+20 dimensions are read by the original camera
builder. Only targets matching the scene dimensions at `0x82C651F0/F4` get the
perspective correction, excluding smaller mirrors, shadow maps and cube faces.

The Xbox video mode is set to the desired display ratio (with dimensions within
the SDK's 12-bit limit). Both Carbon and Xenos presenters read that mode. The
movie player's retail 16:9 pixel aspect is adjusted separately. Carbon's scene
mode selection still chooses its retail buffers.
On a fingerprint mismatch the display returns to 16:9 and the hooks pass through.

## Validation

`tests/widescreen_math_test.cpp` checks 16:9, 18:9, 20:9 and 21:9 projection
proportions, unchanged vertical FOV, row-vector view/projection multiplication,
and culling of points visible only in the wider view. Build it as a standalone
C++ executable with the project's include directory.

Version 0.3.7 was built for arm64 and x86_64 and installed on a CPH2723 phone
with a 2640x1216 landscape display using the Carbon renderer. With Widescreen
off and a stale `present_letterbox = false` config, startup selected 2162x1216
and the front-end screenshot showed side bars. With Widescreen on, startup
selected 2640x1216 and a horizontal scale of 0.8188552; logs confirmed both
camera and HUD hooks ran, and a gameplay screenshot filled the display.

Longer visual validation remains for screen markers, pre-rendered movies,
edge culling, other phones, and the Xenos renderer.
