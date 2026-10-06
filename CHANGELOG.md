# Changelog

## Unreleased: native Vulkan renderer (`rexgpu-carbon`), work in progress

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
  `carbon_gpu_upload_chunk_mb`. Late-registered plugin cvars are set with environment variables
  (`REX_CARBON_GPU_TRACE_FRAME=900`) or the TOML; `--name=value` on the command line does not
  reach them.
- CMake preset `win-amd64-vk`: Windows build against the patched SDK built with
  `REXGLUE_USE_VULKAN=ON` (installed to `tools/rexglue-sdk-win-vk`; the prebuilt Windows SDK is
  D3D12-only).
- `patches/rexglue-windows-vulkan.patch`: guards the POSIX-only census code in the SDK's Vulkan
  command processor so the patched SDK (and its Xenos plugin, for A/B) builds on Windows.

### Changed
- `nfscarbon_app.h`: `gpu_plugin` defaults to `carbon` on Windows when not set in the config;
  Android keeps `xenos` until the plugin is packaged in the APK. Either can be forced with
  `gpu_plugin` in the TOML.
- `CMakeLists.txt`: builds `renderer/` (option `NFSCARBON_BUILD_CARBON_GPU`, on by default).

### Status (PC, RTX 5070, 2026-10-06)
- Boots with no Xenos code, runs boot screens and the 3D front end at 60 fps (about 1,200 draws
  per frame), with shaders, pipelines and textures created on the fly.
- Not correct yet:
  - Only the first predicated tile of the scene reaches the screen: each tile is resolved to a
    different address inside the same destination texture, which the resolve cache still treats
    as separate textures.
  - About 48 draws per frame are skipped with no color/depth target bound (not investigated).
  - Too many render passes per frame (about 116): render target sizes are inconsistent between
    passes (heights grow to 8192), which forces pass breaks.
  - Boot logos/movies show black.
  - Not yet built or tested on Android.
