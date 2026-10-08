# CPU and GPU optimization audit — 2026-10-08

This is a code review of the Carbon renderer, command processor, Android audio,
thread scheduling and selected recompiled game hot functions after v0.3.8.
Items 1 and 2 below are implemented in v0.3.9. The remaining opportunities
still require implementation and measurement. Source line references below
describe the original audit snapshot, before the v0.3.9 changes.

## v0.3.9 implementation and validation

- Contiguous constant packets now convert/compare/write one shader stage at a
  time, with four-word NEON batches on ARM64. Repeated bit patterns do not dirty
  the stage. Packets crossing the VS/PS boundary split there; other registers
  retain ordered scalar side effects, and wrapping readers retain their scalar
  path. Frame/chunk invalidation still forces fresh uniform allocations.
- Shader resolves use `DONT_CARE` only when their final host scissor exactly
  covers the whole destination image. Partial rectangles and atlas updates keep
  `LOAD`. The resolve pipeline writes every color channel without blending,
  depth testing or discard, so old destination pixels are unused. Stores remain
  enabled because later draws sample the result.
- ARM64 and x86_64 release native builds and the Android debug APK build passed.
  `tests/constant_write_test.cpp` passed on Windows and on the phone's actual
  ARM64 NEON path: all lengths 0–2048, four dword offsets relative to vector
  alignment, guard words, repeated writes, signed zero and distinct NaN bits.
- Installed versionCode 12 / versionName 0.3.9. Foreground game screenshots
  showed the intro movie and subsequent driving rendering correctly; this is
  a visual smoke check, not a pixel-identical GPU validation test. The observed
  driving windows ran at 59.8–60 FPS.
- In `nfscarbon_059.log` at 09:29:08, 09:29:13 and 09:29:18, constant writes
  numbered 112,895 / 102,813 / 98,764 dwords per frame, of which
  82,182 / 74,271 / 70,616 were unchanged (about 71–73%). Stage-block reuse was
  1,070 / 886 / 760 blocks per frame; some reuse existed before this change,
  so these are total reuse counters, not newly avoided allocations alone.
  All three windows used 17.5 full-overwrite resolves per frame, avoiding their
  old destination attachment loads. GPU resolve time remained about 1 ms/frame;
  a measurable GPU-time saving has not been established.
- The two ten-second CPU-clock profiles collected 17,141 samples (v0.3.8)
  and 16,397 (v0.3.9). GPU Commands represented 25.39% and 22.33% respectively.
  Both runs included changing scenes and user-controlled driving, so these
  samples cannot establish a controlled CPU/FPS improvement. The updated log
  recorded 38 startup audio underruns / 6,912 missing frames, with no later
  counter increase through the last checked driving window. Audible glitch
  elimination has not been verified.

Local evidence: `out/renderer-optimization-{baseline,updated}.log`,
`out/renderer-optimization-{baseline,updated}-profile.txt` and the guarded game
screenshots under `out/renderer-*.png`. APK: `nfs-carbon-0.3.9-renderer-debug.apk`.

## Evidence and limits

The existing v0.3.8 ten-second CPU-clock profile contains 11,324 samples:
GPU Commands 37.79%, MainThread 30.60%, GPU Fetch 9.51%, RWAudioCore Dac 7.23%,
and no SDLThread samples. It includes startup/scene transitions, so its
percentages are useful for locating work, not predicting steady driving FPS.
The phone was outside the game during this audit; no new gameplay profile or
restart was performed.

The last active rendering windows in `nfscarbon_057.log` reported:

- About 1,626 draw calls reaching the renderer, with 704 skipped there.
- About 52 rendering passes and 27 resolve requests per frame.
- About 20.5 copied resolves, with zero skipped as redundant.
- CPU draw time 3.34–5.06 ms/frame; CPU resolve time 0.33–0.46 ms/frame.
- GPU time 9.6–11.2 ms/frame; resolves 1.0–1.2 ms/frame.
- About 10.44 MiB/frame of upload **allocations**. `Upload` counts reserved
  bytes, including partially populated uniform blocks; this is not measured
  memcpy traffic or physical memory bandwidth.
- Estimated resolve read/write traffic 86.7 MiB/frame. This is a format/pixel
  estimate, not a hardware bandwidth counter.
- GPU fence waits 0.01–0.02 ms/frame, no new pipelines or shaders, and no fetch
  waits for queue space. Increasing queue capacity is not the current priority.

## First changes to try

### 1. Avoid loading pixels that a resolve completely overwrites

Location: `renderer/src/render_targets.cpp:599`.

Every shader resolve uses `VK_ATTACHMENT_LOAD_OP_LOAD`, even when its scissor
covers the entire destination. The resolve pipeline disables blending and
writes the destination pixels directly. A full-destination color resolve can
use `DONT_CARE`; partial/tiled resolves must retain `LOAD` to preserve pixels
outside the copied rectangle.

Benefit: avoid unnecessary destination reads and tile restoration on eligible
resolves. Count eligible full-image resolves, compare GPU resolve time, and
check movies, menus, scaled rendering and tiled output. Saving is a fraction
of the measured 1.0–1.2 ms resolve budget, not that whole budget.

### 2. Batch constant register writes and ignore identical values

Locations: `renderer/src/command_processor.cpp:575`, `:592`,
`renderer/src/renderer.h:162`, `renderer/src/renderer.cpp:2302`.

Contiguous register packets call `Reader::Read` and `WriteRegister` for every
dword. Every float-constant write marks a whole shader stage dirty, including
writes that repeat the same bits. A subsequent draw then allocates a new
4 KiB stage block and copies its used constant prefix.

The existing symbol profile places several hot offsets in `WriteRegister`,
`WriteRegistersFromReader`, `Reader::Read` and byte swapping. `StagingWrite`
alone accounts for 2.34% of process samples; this is measured command-handling
work, not an estimate based solely on source size.

Add a bulk path for contiguous, side-effect-free constant ranges, byte-swap
in batches, compare old/new values, and dirty each affected stage once when
something actually changes. Keep scalar handling for ring wraps and special
registers such as scratch writeback and gamma tables. Subsequently consider
dirty ranges restricted to constants the current shader actually reads.

Measure equal versus changed writes, constant allocations and CPU packet/draw
time. Check packet ranges crossing stage/register boundaries and verify guest
register values and rendered output against the existing path.

### 3. Clear attachments through load operations where possible

Locations: `renderer/src/render_targets.cpp:684`,
`renderer/src/renderer.cpp:898`.

The resolve clear path starts a rendering instance with attachment `LOAD`,
issues `vkCmdClearAttachments`, then ends the instance. A full-target clear
does not need the previous pixels; a clear load operation can avoid loading
them and issuing the explicit clear command. Partial clears still need their
rectangle and preservation of the other pixels.

A larger follow-up is to defer full clears into the next rendering pass's
load operation, removing a separate pass. That requires pending-clear state
and materializing it before any intervening sampling, resolve, target growth
or other read. It is not safe to simply omit the current clear pass.

Measure full versus partial clear counts, rendering-instance count and GPU
pass time. Compare shadow/depth/stencil output as well as scene colors.

### 4. Narrow image barrier dependencies

Location: `renderer/src/renderer.cpp:799`.

Every layout transition uses `ALL_COMMANDS -> ALL_COMMANDS` with broad memory
access masks. That can serialize unrelated pipeline work. Track the actual
producer/consumer usage: color attachment writes, depth/stencil writes,
transfer writes and shader reads. Texture sampling may happen in either
vertex or fragment stages, so a universal fragment-only replacement is unsafe.

Use synchronization validation and image comparisons when narrowing masks.
Measure GPU frame time and bubbles rather than assuming fewer barriers means
correct or faster rendering.

Khronos documents the benefits and constraints of
[precise barriers](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html)
and [attachment load operations](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html).

## Follow-up opportunities

### 5. Make detailed instrumentation optional

Locations: `renderer/src/renderer.cpp:1722`, `:848`, `:860`,
`renderer/src/command_processor.cpp:727`.

Every draw, including skipped draws, takes four clock readings through two
timers that measure substantially the same interval. One timer can update
both totals. At the observed draw rate this is hundreds of thousands of
clock calls per second; `clock_gettime` itself accounted for 0.54% of process
samples in the existing profile.

`carbon_gpu_stats = false` currently suppresses reports but still records
timestamps, reads query results, aggregates pass/destination maps and updates
histograms. Gate detailed profiling while retaining the lightweight FPS
overlay. Debug packet counters also use atomics on each type-3 packet and
could publish a local count periodically for the watchdog.

Measure instrumentation on/off and verify that overlay, watchdog and shutdown
remain functional. Expect a smaller gain than the removed SDL busy loop.

### 6. Cache converted static indices

Location: `renderer/src/renderer.cpp:2183`.

Vertex data has persistent caching, but ordinary indexed draws still convert
and upload their index stream each time, also recomputing its min/max range.
Cache host-endian indices and the range for verified unchanged source data.
Include address, count, format, endian and restart parameters in the key;
invalidate rewritten buffers and respect frames still using an old allocation.

Measure index bytes converted/copied and reuse first. The profile's 3.74%
`memmove` on GPU Commands includes multiple copy paths and cannot all be
attributed to indices. Keep per-draw handling for frequently rewritten data.

### 7. Reduce per-packet queue publication overhead

Locations: `renderer/src/command_processor.cpp:409`, `:453`, `:516`.

The fetch side copies and release-publishes each packet; the executor
release-publishes its tail after each packet and checks whether a fetch waiter
needs waking. Bounded batches could amortize atomics and small copies. Never
delay publication until an entire large indirect buffer is finished: the queue
could fill before the executor can see it. Retain prompt publication around
space pressure, waits and frame boundaries.

Measure batch size, wake counts, game ring waits and audio starvation. The
existing queue has adequate capacity; this is an overhead optimization.

### 8. Rewrite a measured guest vector transform

Location: `generated/default/nfscarbon_recomp.158.cpp:21458`,
`sub_827155B8`.

This small function accounted for 3.16% of process samples in the existing
profile. It loads a vector and a 4x4 matrix, performs multiple alignment/endian
permutations, constructs columns, computes four dot products and stores a
vector. An ARM native implementation is a credible candidate after proving
equivalence. Put a hook in `src/`, rather than editing regenerated C++.

Test overlapping inputs/output, unaligned guest pointers, component order,
floating-point rounding, flush mode and ABI behavior against the original.
The profile only establishes this function is worth investigating; it does
not establish a speedup for a replacement.

## Larger or lower-priority work

- Add direct image-copy resolve paths only for equivalent formats, sample
  counts and rectangles with no exponent conversion or channel swap. Keep the
  shader path for conversions. Transfers may perform worse on a particular
  mobile driver; compare them rather than assuming a copy command is cheaper.
- The redundant-resolve filter currently saves zero copies in the last active
  windows. One resolve/frame was overwritten before being sampled, but removing
  it requires proof using command look-ahead; the source may be cleared or
  overwritten immediately after the resolve. Current signatures are not enough
  to safely defer or omit it.
- Investigate whether skipped reflection/post-process work can be omitted at
  a game render-pass boundary. Skipping at the native renderer already avoids
  GPU draws, but the game still prepares command packets. Preserve any state,
  synchronization or guest-memory side effects; the 704 skipped draws are not
  all proven to be reflection work.
- The affinity rules explicitly recognize `GPU Commands` and `Main XThread`;
  the measured `MainThread` falls under the other-core rule. Compare scheduling
  it on faster cores without crowding out GPU command execution or audio.
- Texture and vertex hashes already back off for unchanged data. Do not extend
  those intervals merely to reduce CPU usage: that can delay visibility of
  guest writes. Exact dirty tracking is a separate correctness/performance task.

Start with full-resolve load elimination and unchanged/bulk constant writes,
then full clears and precise barriers. Benchmark each change independently
with the same saved scene, settings and comparable phone temperature. Preserve
graphics quality, and track audio missing-frame totals alongside CPU/GPU time.
