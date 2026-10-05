# Native renderer for Carbon: licence, MW code map, design

## Source and licence

Reference: <https://github.com/codepdbh/nfsmw-android> pinned at `5f581c6` (v0.3.5), cloned to
`reference/nfsmw-android/`, itself derived from StevensND/nfsmw-nx and madelrandel-blip/NFSMW-Recompiled.

- Licence: **GPL-3.0** (`LICENSE`; `THIRD_PARTY_NOTICES.md` says everything except the SDK/XenosRecomp/mesa parts is GPL-3.0).
  Parts under other licences: `sdk/` BSD-3-Clause (ReXGlue/Xenia), `shaders/XenosRecomp` MIT (hedge-dev), `mesa/` MIT.
- User decision (2026-10-05): reuse is approved, with credit to the repo.
- Consequence to keep in view: any file in our tree that is a copy or adaptation of the GPL code is GPL-3.0, so a
  release of the APK that contains it needs the corresponding source under GPL-3.0 as well. Credit alone is not enough.
  Files derived from it must keep the SPDX header and an attribution line naming the upstream file.
  The BSD/MIT parts (XenosRecomp, the SDK) can be reused with their notices only.

## What the MW renderer is (docs/native-renderer.md upstream, app/src/nfsmw_nativo_*, 27.6k lines)

A graphics system installed through `config.graphics` (not a plugin): it reads the PM4 ring itself, keeps its own register
mirror, and draws with Vulkan using real render targets (one image per base/format/pitch), resolves as copies or
image swaps, 3 work slots (frames in flight), a texture cache keyed by content hash, a pipeline cache plus a saved
pipeline list that is re-created at boot, and shaders from an AOT library (NFSSPV) made with XenosRecomp.

| File | Lines | Class | Notes |
|---|---|---|---|
| `nfsmw_nativo_sistema.{h,cpp}` | 4,902 | (a) generic | graphics system, ring thread, MMIO/vblank, read-pointer progress/futex, swap, presentation |
| `nfsmw_nativo_destinos.{h,cpp}` | 6,182 | (a) generic | render targets, resolve/copy/clear, resolve-without-copy swap, front buffer lookup |
| `nfsmw_nativo_dibujos.{h,cpp}` | 13,081 | (a) mostly | per-draw: pipeline key, textures, vertex/index upload, constants, pass recording; has MW special cases (sky, glare, shadows, vegetation) to find and gate |
| `nfsmw_nativo_texturas_pool.{h,cpp}` | 643 | (a) generic | slab allocator for texture images |
| `nfsmw_nativo_vertices_dedupe.h` | 200 | (a) generic | dedupe of vertex uploads |
| `nfsmw_nativo_sincronizacion.h` | 32 | (a) generic | image barriers/dependencies |
| `nfsmw_nativo_captura.{h,cpp}` | 216 | (a) generic | periodic PNG capture for A/B tests |
| `nfsmw_nativo_shaders.{h,cpp}` | 587 | (a)+(b) | NFSSPV library lookup; PS by microcode, VS by game object identity |
| `nfsmw_nativo_ganchos.{h,cpp}` | 1,249 | **(b) MW-specific** | game-thread hooks that record which VS/PS each draw used |
| other `app/src/*` | | (b) | hot-function C++ rewrites, MW settings, edition patches: not needed for the first port |

### Guest addresses the renderer depends on (21, all MW; re-derive for Carbon by fingerprint)

`sub_8259BC90` (PS constructor), `sub_8259C038` (VS constructor), `sub_8259C2A8`, `sub_8259BDC0`, `sub_825A2FB8` (11 uses),
`sub_825A37D8`, `sub_825A3AF0`, `sub_825A36A8`, `sub_825A40C0` (FlushState), `sub_82597960`, `sub_82597690`, `sub_8258F998`,
`sub_8258F810`, `sub_8258EA28`, `sub_82443B18`, `sub_824511E8`, `sub_82448168`, `sub_82442908`, `sub_824427F8`,
`sub_82442478`, `sub_82225438`, `sub_826E8EE8`.
Only about 21 distinct addresses is a small surface. Method: extend `scripts/find_fingerprints.py` with these patterns
(take instruction shape from MW's `guest_image` via `reference/NFSMW-Recompiled-Mobile`), as in `render-mode-addresses.md`.
Offsets differ between games (the D3D device ring-pointer offset is 10384 in MW and 10768 in Carbon), so each hook's struct
offsets must be re-derived too.

## Design differences from the original plan

1. **Shader path.** The plan (Stage 3) assumed microcode -> SPIR-V through the SDK translator. MW's renderer instead consumes
   the NFSSPV library (XenosRecomp: Xenos microcode -> HLSL -> SPIR-V; MIT), whose entries also carry the container's constant
   table, sampler list and vertex declaration. The draw path depends on those fields. Recommendation: adopt NFSSPV and build
   the Carbon library from Carbon's own shader containers (`shaders/nfsmw_buscar_contenedores.cpp`,
   `tools/biblioteca_shaders.mjs` show how MW's were found), not our own `.xspv` format. Stage 3 changes accordingly.
2. **Vertex shader identity.** MW's D3D patches vertex microcode before it reaches the ring, so the library cannot be keyed by
   content; MW hooks the shader constructors and each `Draw*`. Carbon needs the same hooks, found by fingerprint, and must be
   checked for the same patching (compare ring microcode against the container for a sample draw).
3. **Install point.** MW sets `config.graphics` in `OnPreSetup`; our plan said plugin (`gpu_plugin="native"`). `config.graphics`
   is simpler (no ABI, same SDK), keeps xenos selectable by cvar, and `OnPreSetup` already runs before the plugin check.
   Use `config.graphics` for the first version; revisit a plugin only if relinking the 107 MB game lib per renderer change hurts.
4. **SDK.** MW's `sdk/` is a v0.10.0 fork. Ours is v0.10.0 plus Android and local patches. The `IGraphicsSystem` interface
   used (`SetupPresentation`, `SetupGuestGpu`, ring init, read-pointer write-back) must be diffed before porting.

## Plan to port (nothing copied yet)

1. Diff `reference/nfsmw-android/sdk` against `reference/rexglue-sdk` for the headers the renderer includes.
2. Re-derive the 21 guest addresses and device offsets for Carbon.
3. Build `xenos`-independent skeleton (milestone N1) from `nfsmw_nativo_sistema` with attribution; headless run.
4. Render targets + resolves (N2), then NFSSPV library for Carbon (N3), race scene (N4), readbacks (N5), frames in flight (N6).
5. Do not port MW special-case shader hashes or hot-function rewrites until a Carbon draw census shows the need.

Expected size: about 15-20k lines of adapted code. Stage 1 data says the CP thread is the bottleneck (about 7-12 us/draw
at 1,400-3,200 draws/frame), which matches MW's own pre-native numbers (about 8 us/draw), so the same approach should apply.

## Progress log

### 2026-10-05: SDK interface diff and device patch (done)

Compared every `rex/*` header the MW renderer includes (`reference/nfsmw-android/sdk` vs `reference/rexglue-sdk`, CRs stripped):
`IGraphicsSystem` (`system/interfaces/graphics.h`), `xenos.h`, `registers.h`, `xmemory.h`, `kernel_state.h`, `video.h`, `hook.h`
and `thread.h` are **identical**. Real differences that matter:

| Area | Difference | Action |
|---|---|---|
| `ui/vulkan/device.{h,cpp}` | MW enables extra device features under cvar `vulkan_native_shader_features`: `shaderInt64`, `shaderSampledImageArrayDynamicIndexing`, `pipelineStatisticsQuery`, `bufferDeviceAddress`, `runtimeDescriptorArray`, 3x `descriptorBinding*`, and `VK_EXT_extended_dynamic_state3` (blend enable, blend equation, color write mask) | **Ported** into our SDK (header fields, cvar, feature enables, EDS3 struct + extension). Builds clean. Off by default. |
| `ui/presenter.{h,cpp}`, `ui/vulkan/presenter.{h,cpp}` | MW adds its own presenter paint thread (`present_hilo_propio`, `ShutdownPaintThread`, `PaintThreadMain`) | Not ported yet. Optional (frame = max(CPU,GPU) instead of the sum); port after N2. |
| `function_dispatcher.h`, `memory/utils.h` | Interrupt-dispatch mutex (ours) / Switch-only mapping (theirs) | None needed. |
| Switch/NVK files | Horizon platform code | Ignore. |

Our own local SDK patches (pipeline cache funcs, second present queue, custom `vulkan_loader`) are kept.

### Open: re-deriving the 21 MW guest addresses for Carbon

No MW guest image is in the repo, so MW instruction fingerprints cannot be taken from it, and the Carbon decomp has no D3D symbol names.
Method that works without MW code: find the functions by behaviour in our `guest_image.bin` and the generated C++.

- The 2005-XDK shader container signature is `0x102A0E00` (VS) / `0x102A0E01` (PS) (`shaders/nfsmw_contenedor.h:64`).
  Searching our image for `lis rX,0x102A` gives 5 sites: `0x82433840`, `0x826EB210` (D3D range, uses device offset +19772/+19776
  and reads a container at +20: strongest candidate for the shader-create path), `0x82741E80`, `0x82741EB0`, `0x82741FF8`.
- MW's constructors take the container in r3 and return the object in r3 (hook = call original, then record the object).
  Next: confirm `0x826EB210` and its VS/PS siblings by reading the generated C++ and matching that shape; then the device-state
  functions (FlushState, Draw*, IM_LOAD callers) by their register-mirror layout and the PM4 packet writers.
- Offsets inside the device differ per game (ring pointer 10384 in MW vs 10768 in Carbon), so each hook's offsets are re-derived too.
