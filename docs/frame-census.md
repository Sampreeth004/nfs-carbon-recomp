# Carbon frame census

Probe: `gpu_census` (bool, hot-reload) and `gpu_census_interval` (frames, default 120) in the
xenos plugin, `src/graphics/vulkan/command_processor.cpp`. Log tag `[census]`, averaged per interval.
Enable with `gpu_census = true` in the game TOML.

Fields: draws/f, passes/f (framebuffer changes), copies/f (IssueCopy resolves), pairs/f (unique VS/PS per
frame), pairs_seen (cumulative), placeholder/f (draws skipped on async pipeline placeholders),
frame_ms (CP time between swaps), draw_ms/f, copy_ms/f, us/draw with the split
prim / xlat / tex / rt / pipe / state / rec.

Not covered yet: SET_BIN_MASK/predicated draw counts (lives in the base `command_processor.cpp`),
and per-frame shader translation count (xlat time is the proxy).

## Results

Run 1, 2026-10-05, OnePlus CPH2723 (thermal 0), Turnip_Gen8_V36 driver, `gpu_sin_msaa=true`, 1080p output,
`gpu_census_interval=120`. Scene was not controlled (launched with `am start`, ~60 s from boot; the game
reached a 3D scene on its own). Raw: `files/logs/nfscarbon_002.log` on the device.

| phase (by draws/f) | draws/f | passes/f | copies/f | pairs/f | frame_ms (CP) | us/draw |
|---|---|---|---|---|---|---|
| boot/logos | 53-59 | 2-2.6 | 2-4.7 | 5 | 31-37 | n/a |
| front-end 3D | 1160-1200 | 10 | 21 | 13-14 | 18-19 | n/a |
| heavy 3D scene | 1800-3360 | 15-17.6 | 24-28 | 25-29 | 25-36 | 6.1-9.0 |

Per-draw split in the heavy scene (us/draw): prim 1.9-2.9, state 1.4-1.5, rec 2.0-3.4, tex 0.4-0.7,
rt 0.19-0.23, xlat 0.17, pipe 0.09-0.11. Total 6.1-9.0 us/draw; MW measured about 8 us/draw. CP draw time is
14-22 ms of a 25-36 ms frame, so the CP thread is the bottleneck. No placeholder draws. pairs_seen stays at 30-35,
so the shader set is small (as in MW: 31 pairs).

Caveats: scene not identified as menu vs race; MSAA was already off, so the MSAA-on rows and the race
census with a fixed track are still missing. Simpleperf thread split not captured yet.

### Planned rows

| stage | setting | scene | draws/f | passes/f | copies/f | pairs/f | frame_ms | us/draw | notes |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 720p MSAA on | race | | | | | | | TODO |
| 1 | 720p MSAA off | race | | | | | | | TODO |

## Run 2: in race (2026-10-05, thermal status 0-1, same settings as run 1: MSAA off, 1080p output)

Census (8 consecutive 120-frame windows): draws/f 1380-3190, passes/f 16-17, copies/f 24-27, pairs/f 27-30,
CP frame_ms 27-48 (about 21-37 fps), draw_ms/f 16-32, **10-12 us/draw** (prim 3.1-4.2, rec 3.7-4.9, state 1.6-2.1,
tex 0.4-0.9, rt 0.25, xlat 0.2, pipe 0.12), placeholder 0, pairs_seen 35. Heaviest window: 3189 draws, 48 ms.
Raw `[census]` lines: device `files/logs/nfscarbon_002.log`.

Thread CPU (simpleperf, 20 s, 216k samples, `perf_race.data` on device, race scene):

| thread | share of samples |
|---|---|
| GPU Commands (CP) | 39.6% |
| SDLThread | 14.6% |
| RWAudioCore Dac | 12.1% |
| MainThread | 10.6% |
| Main XThread | 10.3% |
| SDLActivity | 3.5% |

`top -H` snapshot: GPU Commands 86%, MainThread 32%, SDLThread 29%, Main XThread 25%, audio 21%.

CP thread symbols: **`mprotect` 18.5%** (libc), Adreno driver 7.6%, `memmove` 5.1%, three adjacent unsymbolized
hot spots in `librexgpu-xenos.so` at +0x307ea8 / +0x307eb8 / +0x307ec8 (4.6-5.3% each, need the unstripped lib
to name), `syscall` 3%.
Main XThread symbols: **`mprotect` 17.9%**, `sub_827780F8` 9.2%, `__imp__sub_824694C0` 8.8%, `syscall` 8.2%,
`sub_824E9720` 6.5%, `__savegprlr_29`/`__restgprlr_29` 9%.

Findings: the CP thread is the bottleneck (about 40% of all samples, 86% of a core). `mprotect` is the top symbol on
both the CP and the main guest thread, which points at the shared-memory write watch (page protection toggling)
rather than the draw path itself. Check this before Stage 4.

### Symbolized CP hot spots (run 2)

`librexgpu-xenos.so` +0x307ea8/+0x307eb8/+0x307ec8 (4.6-5.3% of CP samples each) resolve via llvm-addr2line to
`std::__rotate_gcd<uint64_t*>` (libc++ `rotate.h:114-117`). Caller: the local "subidas adelantadas" patch,
`VulkanSharedMemory::RequestRanges` -> `DeferredCommandBuffer::NfsmwMoverCola` (`std::rotate` of the recorded command
stream from the pass start to the end, on every shared-memory upload that is hoisted before an open pass). Cost scales with the
size of the pass recorded so far, so it grows with draws per pass. With `memmove` (5.1%) the rotate path is
about 20% of CP-thread samples (about 8% of all samples). Gated by cvar `vulkan_adelantar_subidas`: A/B it on device.
`mprotect` (18.5%) is separate: guest-memory write watch, see `src/core/memory_posix.cpp:417-444`.

Shader cache pulled to `shaders/collected/` (title 454107EC): `.xsh` 30,320 B, `.fbo.vk.xpso` 14,796 B. Use `adb exec-out`
(not `adb shell`) to pull binaries; `adb shell` adds CR bytes.

## Run 3: A/B `vulkan_adelantar_subidas = false` (race, 2026-10-05)

Caveats: thermal status was 2 during the capture (protocol wants <=1), the race moment is not identical to run 2, and each
run is one session. Treat as indicative.

| setting | draws/f | passes/f | copies/f | CP frame_ms | draw_ms/f | us/draw | prim | rec |
|---|---|---|---|---|---|---|---|---|
| early uploads ON (run 2) | 1380-3190 | 16-17 | 24-27 | 27-48 | 16-32 | 10-12 | 3.1-4.2 | 3.7-4.9 |
| early uploads OFF (run 3) | 1960-2950 | 15-16 | 24-27 | 37-53 | 14-19 | 6-7 | 1.6-2.3 | 1.7-2.3 |

Result: turning it off cuts the per-draw cost by about 40% (rotate gone) and does not change the pass count
(15-16 vs 16-17), but CP frame time went **up** (about 10 ms of non-draw time in run 2 vs about 25-30 ms here: frame_ms - draw_ms - copy_ms).
So the rotate is expensive but the feature pays for itself elsewhere (uploads that cut a pass cost more).
Keep it ON (reverted in the device TOML). The better fix is to make the move cheaper (see below), not to disable it.

## Run 4: Stage 2 `carbon_single_pass = true`, 1280x720 (race, 2026-10-05, thermal 1-2)

Log: `[render_mode] mode 2: 1280x720, 1 tile, MSAA off` (fingerprints matched, no mismatch). Screenshot of the race: renders
correctly (scene, HUD, mirror, minimap, no black/half-drawn frames), launcher overlay showed 25 fps / 39 ms.

| setting | draws/f | passes/f | copies/f | pairs/f | CP frame_ms | us/draw |
|---|---|---|---|---|---|---|
| single_pass off (run 2) | 1380-3190 | 16-17 | 24-27 | 27-30 | 27-48 | 10-12 |
| single_pass on (run 4) | 1990-2700 | 16-17 | 24-27 | 29-30 | 34-44 | 8.6-10.4 |

Result: **no measurable change** in draws or passes per frame. These runs already had `gpu_sin_msaa = true` (MSAA off) and
`nfsmw_una_pasada = false`, and Carbon's scene was not being drawn 2-3 times, so there is nothing for mode 2 to remove here. The
16 passes / 25 resolves per frame are Carbon's own post/shadow/reflection passes. Leave `carbon_single_pass` default off; it
only matters if the MSAA-on config is used (not yet measured). Not verified: that the mode select / output-size hooks
actually changed the active mode (only the constructor log was seen); would need a log line in `sub_8250B200`.
