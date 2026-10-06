# Changelog

## 0.2.0 (2026-10-06): native Vulkan renderer (`rexgpu-carbon`)

A new GPU plugin, `renderer/`, that draws the game with Vulkan directly instead of emulating the
Xenos GPU. It loads through the SDK's existing plugin seam (`gpu_plugin = "carbon"`), links only
`rexruntime` and glslang, and builds from the same sources as `rexgpu-carbon.dll` (Windows) and
`librexgpu-carbon.so` (Android). The Xenos plugin is not loaded when it is selected. It is written
from scratch; the Xenos register, microcode and texture-layout definitions come from the SDK's
BSD-licensed headers and Xenia documentation. No code from nfsmw-android is used.

### Added
- `renderer/src/command_processor.*`: own PM4 ring reader and register mirror. Keeps the contract
  the game's Direct3D relies on: read-pointer write-back while the ring drains, interrupts,
  scratch/fence writes (`MEM_WRITE`, `EVENT_WRITE_*`, `REG_TO_MEM`, `COND_WRITE`), `WAIT_REG_MEM`,
  predicated packets (bin mask/select), gamma ramp registers, occlusion query faking. Exports
  `nfsmw_cp_rptr_seq` so `ring_wait_hook.cpp` can sleep on it as with Xenos.
- `renderer/src/gpu_system.*`: `IGraphicsSystem` implementation: Vulkan provider and presenter,
  MMIO at `0x7FC80000`, 60 Hz vblank thread, interrupt dispatch, a 3-second watchdog log line.
- `renderer/src/shader_translator.*`: Xenos microcode -> GLSL 4.50. Covers all ALU vector/scalar
  opcodes (Direct3D 9 `0 * x = 0` multiply rule, predicates, `a0`/`aL` relative addressing,
  kill), vertex fetch of every vertex format (read from a storage buffer with in-shader endian
  swap), texture fetch (1D/2D/3D/stacked/cube, register LOD and gradients, LOD queries), exports,
  and control flow (straight-line code, or a `switch (pc)` state machine for jumps, calls and
  loops). Variants per draw: interpolator count, flat shading mask, pixel parameter generation,
  rectangle lists expanded in the vertex shader (no geometry shader needed).
- `renderer/src/shader_compiler.*`: glslang GLSL -> SPIR-V with an on-disk cache
  (`<cache>/carbon_gpu/spirv/`).
- `renderer/src/renderer.*`: frames in flight (3), per-frame upload chunks (vertices, indices,
  constants), dynamic rendering, pipeline cache keyed by full draw state and saved to disk,
  index conversion for quad lists, triangle fans and line loops, primitive restart, viewport and
  window-offset transform folded into the vertex shader, alpha test, blend/depth/stencil/polygon
  offset state, presentation through the SDK presenter with the guest gamma ramp.
- `renderer/src/render_targets.cpp`: real Vulkan images per (EDRAM base, pitch, format) instead
  of EDRAM emulation; render targets grow on demand. Resolves copy a rectangle into the image that
  stands for the destination texture (format conversion, exponent bias, red/blue swap, depth to
  `R32F`), then apply color/depth clears.
- `renderer/src/textures.cpp`, `texture_decode.*`: texture cache keyed by guest layout, untiling
  (2D/3D), packed mip tails, endian swap, host format mapping (BC formats native or decoded on the
  CPU when unsupported), re-upload when the guest memory fingerprint changes (checked less often
  for stable textures), guest swizzles as image view swizzles, signed formats, samplers.
- Diagnostics cvars: `carbon_gpu_stats` (fps, draws, passes, resolves, uploads and skipped-draw
  reasons every 5 s), `carbon_gpu_trace_frame=N` (logs every pass, draw and resolve of frame N),
  `carbon_gpu_dump_shaders`, `carbon_gpu_watchdog`, `carbon_gpu_vsync`,
  `carbon_gpu_upload_chunk_mb`, `carbon_gpu_dump_frame_interval=N` (saves the presented frame to
  `carbon_frames/frame_<n>.bmp` every N frames, read back from the GPU, so it works with the
  window in the background). Late-registered plugin cvars are set with environment variables
  (`REX_CARBON_GPU_TRACE_FRAME=900`) or the TOML; `--name=value` on the command line does not
  reach them.
- CMake preset `win-amd64-vk`: Windows build against the patched SDK built with
  `REXGLUE_USE_VULKAN=ON` (installed to `tools/rexglue-sdk-win-vk`; the prebuilt Windows SDK is
  D3D12-only).
- `patches/rexglue-windows-vulkan.patch`: guards the POSIX-only census code in the SDK's Vulkan
  command processor so the patched SDK (and its Xenos plugin, for A/B) builds on Windows.

### Changed
- `nfscarbon_app.h`: `gpu_plugin` defaults to `carbon` on Windows and Android when not set in the config.
  The Android settings screen has a renderer switch (Native Vulkan or Xenos fallback).
- `CMakeLists.txt`: builds `renderer/` (option `NFSCARBON_BUILD_CARBON_GPU`, on by default).

### Android app
- New app icon (adaptive icon on Android 8+).
- New launcher: dark racing theme, two-pane landscape layout, status cards (game data, renderer, GPU driver,
  display, controls) with ready/attention dots, a PLAY button that becomes SET UP until game data is chosen, and
  a CONTROLS shortcut that opens the touch layout editor directly.
- Touch controls redraw: colored A/B/X/Y buttons, pill-shaped bumpers/triggers/start/back (larger touch areas),
  darker backing for readability on bright scenes, d-pad arrows, ringed analog sticks, bold labels.
- New default layouts (version 4, replaces saved layouts once): gamepad with thumbsticks and the d-pad
  at the bottom, driving with a wide steering strip on the left, brake/gas on the right and the action buttons
  in the middle. Driving is the default layout.

### Performance (Android prep)
- Persistent vertex arena (`carbon_gpu_vertex_arena_mb`, default 128): guest vertex buffers are copied once and
  re-checked by 16 KiB page hash (less often while stable) instead of being re-uploaded every frame. Only the
  vertices the draw's indices reach are copied (`carbon_gpu_vertex_ranges`). Rewritten buffers get a fresh copy
  (frames in flight keep the old one); buffers that change 3 times fall back to the per-frame upload. In game the
  vertex copy went from about 42 MiB to about 0 MiB per frame.
- Resource eviction: textures are freed least recently used first when over `carbon_gpu_texture_budget_mb`
  (3072 on PC, 640 on Android) and after 30 s unused; render targets after 15 s unused. Memory use is in the stats line.

### Fixed (colors)
- Frames resolved with the Xenos red/blue swap flag showed swapped colors on screen (gold instead of cyan logo).
  The present step now undoes the swap.

### Fixed
- Wrong colors on `k_8_8_8_8_GAMMA` render targets: the format was mapped to
  `VK_FORMAT_R8G8B8A8_SRGB`, causing Vulkan to apply sRGB gamma encoding on
  every color write. The `_GAMMA` suffix is documentation only — the hardware
  stores raw values. Changed to `VK_FORMAT_R8G8B8A8_UNORM`.
- Predicated tiling: the scene is rendered in 256-row tiles, each resolved to the address of its
  first row inside one texture. Resolves that land inside an existing resolved texture of the same
  format and pitch (on a 32-row strip boundary) now write into it at that row, so the whole frame
  reaches the screen instead of the top tile.
- Render target sizes: heights came from the 8192 default scissor. They now come from the
  scissor and viewport, capped by where the next bound target starts in EDRAM. Formats that only
  differ in blending precision (`2_10_10_10_FLOAT` / `_AS_16_16_16_16`, `2_10_10_10` /
  `_AS_10_10_10_10`) share one render target. Render passes per frame went from about 116 to 48.
- Boot movies and logos were a flat color: `SQ_INTERPOLATOR_CNTL.param_shade` was used as a flat
  shading mask, which made the movie texture coordinates constant. It is ignored now (everything
  is interpolated smoothly, as in the SDK's Xenos backends).

### Status (PC, RTX 5070, 2026-10-06)
- Boot movies, title screen, loading screen, front end, the prologue drive with HUD and mirror,
  and the pause menu (blurred background) render correctly at 60 fps with no Xenos code; up to
  about 4,000 draws, 56 passes and 23 resolves per frame in game.
- 48 single-point draws per frame with color writes masked off and no depth/stencil are skipped
  (stat `no-output`); they write nothing.
- Open:
  - The Android build has not been profiled on a device with the optimizations yet; expect to tune the
    texture budget and vertex arena size per device.
  - Resolves are full-screen copy passes, which are relatively costly on tile-based mobile GPUs.
  - The APK is debug-signed.
