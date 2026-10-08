# Changelog

## 0.3.12 (2026-10-08): new launcher, settings and controls editor

- Launcher: large PLAY button that becomes SET UP (opening Game data) when no
  game data is found; status tiles for game data, renderer, GPU driver,
  performance and controls, each opening its settings page; quick settings for
  the performance preset, frame rate cap, touch controls and the fps counter.
  Landscape shows everything side by side; portrait stacks it.
- Settings: navigation rail in landscape (tabs in portrait) with Performance,
  Display, Controls, GPU driver, Game data, Advanced and About pages. Options
  are segmented choices instead of drop-down lists, and preset tiles show which
  preset is active. Xenos-only options are greyed out unless that renderer is
  selected. New Advanced switches: precise GPU barriers, clear with load
  operations, skip redundant resolves and detailed profiling, plus a reset to
  default settings that keeps game data, driver and touch layouts.
- Controls editor: full screen, with the canvas at the phone's own aspect ratio
  below a toolbar strip (no control is ever hidden under the toolbar), drawn with
  the overlay's real size and opacity. Drag to move, pinch to resize, snap to grid
  with centre guides, undo, and per-layout reset. The inspector beside the
  selected control has size steps and a button grid for remapping (driving
  labels such as NOS and E-BRAKE follow the mapping). Overlay size, opacity and
  stick deadzone can be adjusted from the editor toolbar. The Controls page
  shows a live preview of the starting layout.
- Screenshots: `docs/screenshots/`.

## 0.3.12 (2026-10-08): renderer bookkeeping, clears and barriers

First batch of `docs/further-optimization-plan.md`. Not yet measured against
0.3.11 in a controlled scene; the 5 s stats lines report the new counters.

- Detailed profiling is behind `carbon_gpu_profile` (default off, read at
  startup): CPU draw/resolve timers, GPU timestamp queries, per-pass and
  per-resolve-destination breakdowns and slow-frame reports. The per-draw
  timers are one measurement instead of two. The FPS overlay, the 5 s fps /
  pacing / frame-time percentile lines and the watchdog are unchanged. Watchdog
  packet and draw counters use single-writer stores instead of atomic adds.
- Resolve clears use `LOAD_OP_CLEAR` with the render area set to the cleared
  rectangle, instead of a pass that loads and stores the whole target and then
  clears the rectangle. Most clears are small rectangles at the top of large
  targets (256x256 of 280x2080, 640x360 of 640x3728). Pixels outside the
  rectangle are not loaded or stored. Clears with an empty rectangle no longer
  start a pass. `carbon_gpu_clear_load_op = false` restores the old path.
- Image barriers wait only for the stages and accesses of the old layout
  (attachment output, depth tests, transfer, vertex/fragment shader reads)
  instead of `ALL_COMMANDS`. A render target written by one rendering pass and
  attached again in the same layout now gets an attachment dependency, where
  the old code issued no barrier at all. The presenter's output image keeps the
  conservative barrier. `carbon_gpu_precise_barriers = false` restores the old
  barriers.

## 0.3.11 (2026-10-08): phone minimap position

- Move the Android minimap to the upper left, below the layout/reset toolbar
  and above the steering controls. Map, border, mask, player markers, route and
  heat meter share the new position. Existing touch layouts are preserved.
- Apply the layout when the game creates its minimap, including recreation
  after menus. No additional rendering passes or per-frame layout work.
- Set `carbon_phone_minimap_top = false` in the Android TOML to use retail placement.

## 0.3.10 (2026-10-08): texture decode and frame pacing hitches

- Copy unconverted texture rows and contiguous 2D microtile runs directly,
  using NEON byte-order conversion on ARM64. Native BC blocks avoid per-byte
  scratch copies. Packed mips, 3D/cube layouts and format conversions retain
  their existing addressing and pixel values.
- When guest vsync already paces the game at or below the selected FPS cap,
  omit the additional host-cap sleep that can delay command processing past
  the next vblank. Lower caps and vsync-disabled runs retain host limiting.
- Android auto affinity now lets Carbon's actual `MainThread` use all cores,
  including the prime cores, instead of restricting it to the remaining cores.
  Explicit user affinity rules retain their existing behavior.
- Slow-frame diagnostics separate texture decoding from staging allocation.
  See `docs/hitch-reduction.md` for tests, measurements and remaining limits.

## 0.3.9 (2026-10-08): reduce constant processing and resolve loads

- Process contiguous float-constant writes in stage-sized batches, using NEON
  endian conversion and bit comparisons on ARM64. Unchanged writes retain the
  current uniform block; special register side effects and ring wraps keep their
  existing handling. VS and PS are dirtied independently when values change.
- Full-destination shader resolves discard old attachment contents instead of
  loading pixels they overwrite. Partial and atlas resolves preserve other pixels.
- Renderer diagnostics report unchanged constant words, uploaded/reused stage
  blocks and full-overwrite resolves once per reporting window.

## 0.3.8 (2026-10-08): stop Android event-loop CPU spinning

- Fix SDL's Android blocking event wait: stop inserting poll sentinels inside
  the wait loop, where they generate lifecycle wakeups and keep the thread
  running even with no input. Nonblocking event polling keeps its sentinel.
  SDK change is recorded in `patches/rexglue-sdl-android-wait.patch`.
- Audio callbacks count underruns and missing frames without formatting or
  writing logs. The producer reports those counters at most once every five
  seconds, making short audio starvation visible without logging on playback.

## 0.3.7 (2026-10-08): prevent stretched fallback presentation

- Aspect-ratio protection is automatic in both display modes. Widescreen off
  now presents the original 16:9 view with bars, even when an older install
  saved `present_letterbox = false`. The separate Letterbox switch is removed.
- The launcher writes `present_letterbox = true`, and the native startup path
  enforces it for direct game launches too. A one-time HUD diagnostic confirms
  when its projection correction is reached.

## 0.3.6 (2026-10-08): phone widescreen

- Settings > Graphics > Widescreen (default on) uses the game's drawable aspect
  ratio, widens full-screen perspective cameras while preserving vertical FOV,
  and corrects the HUD's horizontal scale. Render targets retain their retail
  sizes. Updated view-projection matrices and culling planes cover the wider view.
- Turning Widescreen off restores 16:9 presentation. Keep Letterbox presentation
  on to preserve proportions. Restart the game after changing the setting.
- Both renderer paths use the Xbox video mode's corrected display aspect; the
  movie player's pixel aspect is corrected separately. Hooks check the retail instruction bytes
  before enabling the camera correction. Device visual validation is pending.


## 0.3.5 (2026-10-07): threaded command processing

- **Command fetch thread** (`carbon_gpu_threaded_cp`, Settings > Graphics > Threaded command processing,
  default on). A new "GPU Fetch" thread does what the hardware command fetcher does: it frames the PM4
  packets in the ring, expands indirect buffers, drops predicated-off packets, copies the rest into an
  8 MiB private queue and publishes the ring read pointer straight away. "GPU Commands" then executes the
  queued packets in order with the same code as before, so fences, interrupts, register state and
  WAIT_REG_MEM behave exactly as in single-thread mode. The game no longer waits for ring space while
  draws are being recorded. New log line every 5 s: peak queue use and how often fetch waited.
  Not measured on the phone yet.

## 0.3.4 (2026-10-07): car reflections off

- **Car reflections default to Off** (`carbon_gpu_reflection_faces = 0`, Settings > Graphics > Car reflections). The
  six cube-face passes and their resolves were measured as never sampled by any draw, so nothing visible
  changes. The old default (2 faces per frame) still cost about 60 draws and 2.4 passes per frame:
  on the phone, CPU draw recording 5.6 -> 4.7 ms per frame, GPU 7.25 -> 7.0 ms, GPU p99 15.5 -> 14.4 ms.
  Existing installs are moved to Off once; the other options remain and only add work.

## 0.3.3 (2026-10-07): downloaded turnip drivers, new settings, redundant resolve skipping

- **Redundant resolves are skipped** (`carbon_gpu_skip_redundant_resolves`, default on). Every render target
  carries a write stamp that changes on each recorded draw, each clear and at creation; each resolved
  texture remembers what its last resolves copied (source target, stamp, rectangle, exp bias, swap).
  A resolve with an identical signature into a region nothing has overwritten since copies the same
  texels, so it is skipped; any resolve into an overlapping region replaces the old signature. Barriers
  and layouts are untouched because skipped resolves record no commands.
  On the phone (Adreno 830, turnip, driving) this removes about 1.2-2 of ~19 resolves per frame (about
  5% of resolve pixels and bandwidth). Resolves cost only about 1.1 ms of ~7.5 ms GPU per frame, so the
  effect on frame time is below run-to-run noise.
- New stats lines (`carbon_gpu_stats`): frame and GPU time p50/p95/p99, resolves copied/skipped per frame,
  resolved pixels and bandwidth, and resolves that were overwritten before anything sampled them, with
  the top destinations.
- Finding: about 23% of resolve pixels (7 of ~18 resolves per frame) are overwritten before any draw
  samples them: the scene and bloom targets (1-2 of 3 per frame each) and all car-reflection cube faces
  (256x256, never sampled because cube fetches do not read resolved faces). They cannot be dropped
  soundly without knowing the future of the command stream, so they are still copied.

- **Turnip / Adreno driver packages now work.** Zips downloaded from the internet (a `meta.json`
  plus `libvulkan_freedreno.so`) used to be rejected because they are ICD drivers, not a full
  `libvulkan.so`. They are now opened through
  [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause, Billy Laws; vendored in
  `third_party/libadrenotools`), which loads the driver beside the system Vulkan loader. Adreno GPUs on
  arm64 phones only; if the driver cannot be opened the game uses the system driver. Packages whose
  files sit inside a single folder are found too. Full `libvulkan.so` loaders still work as before.
- **Thermal auto-downgrade removed** (added in 0.3.2): the app no longer polls the thermal headroom or
  lowers the fps cap on its own, and the generic `GameBridge.setCvar` JNI setter it needed is gone. The
  frame rate cap is whatever you set in Settings. The ADPF performance hint and the per-frame
  re-read of the cap stay.
- **Settings redesign**: header with a Back button and category tabs (Graphics, GPU driver, Game data,
  Controls, About) over dark glass cards in the launcher's style, instead of one long plain list.
  Graphics gains one-tap presets (Battery saver, Balanced, Quality) and groups options into Display,
  Effects and quality, and Xenos-only cards, each with a short explanation; toggles are switches. The
  GPU driver page shows the active driver and its status, lists drivers as tappable rows with
  per-driver Remove, and has a single Import button. Settings values and storage are unchanged.
- SDK patch `patches/rexglue-turnip-icd.patch` (`DynamicLibrary::Adopt`, `vulkan_icd_driver` cvar, ICD
  hook). The APK must ship `libmain_hook.so`, `libhook_impl.so`, `libfile_redirect_hook.so` and
  `libgsl_alloc_hook.so` in `jniLibs/arm64-v8a` (built by the `nfscarbon` target).

## 0.3.2 (2026-10-06): ADPF performance hint and thermal auto-downgrade

- **ADPF performance hint** (Android 13+ devices): the rendering thread reports
  its actual frame time to Android's scheduler each frame. The OS can then lower
  clock speeds when frames are easy and raise them when they are not, instead of
  running maximum clocks all the time. Expected result: less heat, similar fps.
  Loaded via `dlopen` so the APK runs unchanged on older Android versions.
- **Thermal auto-downgrade**: `PowerManager.getThermalHeadroom` is polled every
  2.5 s while playing. If the phone is heading toward throttling (headroom ≤ 0.5)
  the fps cap drops to 45; at critically hot (≤ 0.15) it drops to 30. When the
  phone cools back down (headroom > 0.85) the cap is restored to the setting in
  Settings. A brief toast shows the change. This keeps frames smooth instead of
  the phone applying an uncontrolled throttle.
- **Renderer fps cap re-read each frame** so any runtime cvar change takes effect
  immediately (needed for thermal downgrade; also allows hot-changing the cap
  from the TOML without restarting).
- **Generic JNI cvar setter** (`GameBridge.setCvar`) lets Java code change any
  renderer setting at runtime.

## 0.3.1 (2026-10-06): pipeline warm-up and arrow steering

- **Pipeline warm-up**: every pipeline the game uses is recorded to `<title>.pipelines` next to the
  pipeline cache, with its shaders named by their SPIR-V cache key. On the next start they are all
  queued to the pipeline workers before the first frame, so scenes seen before no longer stall or
  pop in the first time they appear (PC: 78 pipelines queued in 5 ms). This is the same idea as the
  reference port's saved pipeline list. No game shader data ships with the app; the list is built
  on the player's own device.
- **Pipeline cache saved while playing**: written in the background (throttled) and when the app
  is paused, atomically (`.tmp` + rename). Before, it was written only at a clean exit or every 64
  new pipelines, so on Android, where the app is usually killed, it was almost never kept.
- **Shader modules shared**: identical translated shaders reuse one Vulkan module.
- **Touch steering**: the steering slider is replaced by left and right arrow buttons. Steering
  ramps in while an arrow is held, and you can slide a finger from one arrow to the other.

## 0.3.0 (2026-10-06): 60 fps on the phone, new audio and touch controls

### FPS and heat (Snapdragon 8 Elite / Adreno 830, in a race)

| | 0.2.0 | 0.3.0 |
|---|---|---|
| Frame rate | 20-40 fps, swinging, slow-motion feel | 56-60 fps (steady scenes 60) |
| GPU time per frame | 34-49 ms | about 8 ms |
| Renderer CPU per frame | 17-19 ms | about 9 ms |
| Draws recorded per frame | about 2,500-3,000 | about 1,800-2,200 (+ ~600 throttled) |

What made the difference, largest first:
- **Bloom off by default on Android** (Settings: Bloom). Its blur chain (6-8 full-screen passes of
  scattered texture reads) cost 23-38 ms of GPU per frame while driving, for a subtle glow.
- **Pass render area**: passes cover only the rows the game resolves from a target; oversized
  targets (640x3728 for a 640x360 buffer) no longer cost a full load and store per pass.
- **Car reflections and mirror on a budget** (Settings: Car reflections, Rear-view mirror at half
  rate; Android defaults 2 cube faces per frame and half rate). Throttled passes skip their draws and
  their resolve, so the textures keep the previous update. About 600 fewer draws per frame.
- **Draw recording**: redundant pipeline/viewport/scissor/stencil/descriptor calls skipped, texture
  descriptor sets shared between draws, single-pass index conversion, no per-draw allocations, and
  only the parts of the per-draw constants a shader reads are written.
- **Game code** (Android): `tools/direct_calls.py` turns 142,521 calls between unhooked game
  functions (and the PowerPC register save/restore helpers) into direct calls that can be inlined;
  built with ThinLTO and `-march=armv8.2-a`. Run the script after every codegen.
- **UI repaint**: the always-alive achievement toast kept the ImGui UI repainting at every display
  refresh (~13% of the app's CPU). New `achievement_toasts` setting, off on Android
  (`patches/rexglue-achievement-toasts.patch` for the SDK's `rex_app.cpp`).
- **Async pipelines**: new pipelines are built on worker threads; draws needing one are skipped for
  a frame or two instead of the frame stalling (`carbon_gpu_async_pipelines`).
- **Frame rate cap** (Settings: 30/45/60/unlimited), a 60 Hz display request for the game window,
  and the renderer and audio park while the app is in the background.
- **Render resolution** (Settings: 100/85/75/60/50%, `carbon_gpu_render_scale`) for weaker GPUs:
  render targets and resolved images are scaled, everything the game sees stays in guest pixels.
- Texture filtering: anisotropy capped (4x on Android) and off for render-to-texture sources.
- Vertex arena and LRU texture/render-target eviction (from 0.2.0 work) keep memory bounded.
- Diagnostics: per-pass GPU timings with shader hashes, pacing stats, a slow-frame report
  (draw time, texture uploads, pipelines, waits on the game), `carbon_gpu_debug_cycle`.

### Audio
- Android audio goes through a new AAudio stereo driver (`src/android_aaudio.*`): the phone reports
  its speaker as 6 channels and the old 5.1 path lost the center channel, where the dialogue is. The
  driver folds 5.1 to stereo itself with dialogue at full level and a limiter, uses a lock-free
  ring, and reopens on headphone/route changes.
- The runtime is built with Xenia Canary/Edge XMA decoder fixes (packets crossing input buffers, the
  frame tail left undelivered, headers split across packets, loop starts), which stopped voices
  stalling. These are applied to the local SDK tree, not part of this repository.

### Touch controls
- Reworked from scratch: controls are drawn from shapes (old PNG icons removed), dark glass style
  with a cyan highlight, pedals that fill with pressure, a steering pad with chevrons.
- Driving layout follows Carbon's Xbox 360 defaults and drops what is not needed while driving:
  steering pad, GAS/BRAKE pedals, E-BRAKE (A), NOS (B), BREAKER (X), CREW (Y), RESET (LB), VIEW (RB),
  EVENT (Back) and pause. Gamepad layout keeps every button for menus.

### Fixed
- Crash at start on Android: the installed SDK headers were older than the packaged runtime.
- Colors: red/blue swap on the presented frame and double gamma on `k_8_8_8_8_GAMMA` targets.

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
