# Further optimization plan

Date: 2026-10-08  
Baseline version: 0.3.11

## Objective

Improve frame-time consistency, reduce CPU and GPU work, and prevent audio
underruns while preserving graphics quality and game behaviour. This document
describes proposed changes; their benefits require measurement.

## Existing improvements

- Android event-loop busy spinning has been fixed.
- Contiguous shader-constant writes are batched, and unchanged values do not
  unnecessarily invalidate constant blocks.
- Full-overwrite resolves avoid loading old destination pixels.
- Texture conversion uses bulk copies and ARM64 NEON endian conversion where
  applicable.
- Duplicate host frame limiting is avoided when guest vsync already provides
  suitable pacing. The previously captured uncapped session did not use this
  host limiter.
- Android auto affinity allows the actual game `MainThread` onto faster cores.
- Shader/pipeline caching and background pipeline compilation already exist.

## Implementation order

### 1. Establish a current baseline

Profile v0.3.11 in repeatable free-roam driving, racing and pursuit scenarios.
Measure game logic, graphics command translation, texture uploads, audio mixing,
GPU execution and scheduling delays separately.

Record both cold-cache and warm-cache runs, and repeat after the phone reaches
a comparable operating temperature. Existing profiles include changing scenes
and transitions; their percentages identify candidates but do not establish
current steady-state costs or expected speedups.

**Acceptance:** a reproducible baseline with frame-time distributions, thread
CPU samples, texture-upload timing and audio-underrun totals.

### 2. Remove renderer bookkeeping overhead

Locations: `renderer/src/renderer.cpp`, `renderer/src/command_processor.cpp`.

- Combine the duplicate per-draw timers into one measurement.
- Gate detailed timestamps, histograms, destination aggregation and GPU timing
  queries behind the profiling option. Preserve lightweight FPS reporting and
  diagnostics needed by the watchdog.
- Batch command-queue publication and counter updates where safe. Publish
  promptly at queue pressure, waits and frame boundaries; do not wait for an
  entire large indirect buffer before making work visible.

**Expected benefit:** less CPU work for each draw and graphics packet.

**Validation:** compare profiling on/off, packet-processing CPU time, wake
counts, queue waits and audio underruns. Verify lifecycle and shutdown behaviour.

### 3. Reduce unnecessary GPU passes and synchronization

Locations: `renderer/src/render_targets.cpp`, `renderer/src/renderer.cpp`.

- Use attachment clear load operations for full-target clears. Preserve the
  existing rectangle handling for partial clears.
- Subsequently defer eligible full clears into the next rendering pass. Track
  pending clears and materialize them before any sampling, resolve, target
  resize or other read that requires their contents.
- Replace broad `ALL_COMMANDS` barriers with dependencies based on actual
  producer and consumer accesses. Account for attachment, transfer, vertex
  shader and fragment shader usage, including hazards without a layout change.

**Expected benefit:** fewer rendering passes, less attachment memory traffic
and fewer GPU stalls.

**Validation:** use synchronization validation where available; compare colour,
depth, stencil, shadows, mirrors and post-processing. Measure GPU frame time,
clear counts and rendering-pass counts.

References:

- [Khronos: Using pipeline barriers efficiently](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_barriers/README.html)
- [Khronos: Tile-based rendering best practices](https://docs.vulkan.org/guide/latest/tile_based_rendering_best_practices.html)

### 4. Reuse converted geometry buffers

Location: `renderer/src/renderer.cpp`.

- Cache converted index buffers and their calculated minimum/maximum index
  ranges when the source data is verified unchanged.
- Include source address, count, format, endian mode and primitive-restart
  parameters in the cache key.
- Invalidate rewritten sources. Keep old host allocations alive until GPU work
  using them completes, and retain per-draw conversion for dynamic sources.

**Expected benefit:** avoid repeatedly scanning, converting and uploading static
geometry.

**Validation:** count converted/uploaded bytes and cache reuse. Exercise dynamic
geometry, address reuse, primitive restart and overlapping source ranges.

### 5. Improve texture upload handling

Locations: `renderer/src/textures.cpp`, `renderer/src/texture_decode.cpp`,
`renderer/src/renderer.cpp`.

- Separate texture conversion, staging allocation and upload costs in captures.
- Reuse staging allocations where measurements show allocation overhead.
- Investigate reliable guest-memory write tracking to avoid hashing or
  reconverting unchanged resources. Do not merely lengthen hash intervals,
  which can delay visibility of game writes.
- Consider background conversion for predictable assets. Ensure source data
  remains valid and unchanged during conversion, bound memory use, and retain
  the existing path when a texture is needed immediately.
- Present complete textures; do not substitute blank textures to hide stalls.

**Expected benefit:** smaller texture-related frame spikes and fewer repeated
conversions.

**Validation:** reuse the decoder differential tests; check streaming, animated
textures, packed mips, cube maps and rewritten/reused memory. Measure upload
spikes and peak memory use as well as average cost.

### 6. Replace measured game-code hot spots

Initial candidate: `sub_827155B8`, a vector/matrix transform identified in an
earlier profile. Reconfirm its importance in the current build before replacing
it.

- Implement an ARM64/NEON replacement through a hook in `src/`; do not edit
  regenerated C++.
- Preserve guest byte ordering, component order, floating-point behaviour and
  calling conventions.
- Retain the original implementation as the reference and fallback.

**Expected benefit:** reduce PowerPC register, endian and vector-operation
overhead in frequently called routines.

**Validation:** differential comparisons covering unaligned pointers,
overlapping inputs/output, rounding and flush modes, and special floating-point
values. Measure the routine and end-to-end frame time independently.

### 7. Optimize audio production

Locations: recompiled game audio routines, `src/android_aaudio.cpp`, and SDK
audio code where profiling identifies a bottleneck.

- Profile the game's audio mixer separately from XMA decoding and Android
  playback. Earlier captures showed more CPU time in the game audio worker
  than in the XMA decoder; this needs confirmation on the current build.
- Optimize the hottest mixing/conversion routines after proving equivalent
  output.
- Correlate underruns with decoder activity, game mixing, scheduling delays
  and output delivery before changing thread priorities or affinity.
- Adjust buffering only if measurements justify the added latency.

**Expected benefit:** fewer missed audio deadlines with controlled latency.

**Validation:** music, engine sounds, dialogue, many simultaneous voices,
background/resume and output-route changes. Track audible quality, underrun
totals and latency.

### 8. Eliminate unused rendering work earlier

Locations: game render-pass preparation functions and matching renderer skip
paths, identified through profiling and tracing.

- Trace work currently discarded by the renderer back to its game-side
  preparation.
- Bypass preparation only when the pass is proven unnecessary under the active
  renderer configuration.
- Preserve synchronization, state changes and guest-memory side effects.
- Keep required rendering, including visible reflections, mirrors and
  post-processing, intact.

**Expected benefit:** save game-thread preparation and graphics command
processing in addition to GPU work already avoided by renderer skips.

**Validation:** compare command counts and game-thread CPU time, then exercise
scene transitions, races, pursuits, mirrors and graphics settings. Retain a
fallback until equivalence is established.

## Conditional shader follow-up

Improve pipeline warm-up only if new captures show shader translation or
pipeline compilation during hitches. Caching and background compilation already
exist, and the previously investigated driving hitches did not show new inline
pipeline builds. Verify cache compatibility and warm-up coverage before adding
another compilation mechanism. Evaluate rendering completeness as well as
stall time; skipping required draws is not a quality-preserving optimization.

## First implementation batch

After establishing the baseline:

1. Remove duplicate timers and gate detailed profiling.
2. Implement full-target clear load operations.
3. Implement precise image dependencies as a separate, independently measured
   change.
4. Re-profile before proceeding to caching and native game-code replacements.

Keep queue batching and deferred clears separate from the simpler changes so
their correctness and performance can be assessed independently.

## Measurement and release criteria

For each change, compare the same scene, save, settings, driver and similar
phone temperature. Record:

- CPU time by thread and GPU execution time.
- Median, 95th-percentile and 99th-percentile frame times.
- Number of frames exceeding 33 ms.
- Audio underruns and missing audio frames.
- Sustained performance after warming up, plus memory use where applicable.

Verify menus, free roam, races, pursuits, mirrors, HUD rendering and app
background/resume. Build ARM64 and x86_64, and check the stock GPU driver and
supported alternate drivers where available. Retain changes only when they
provide measurable benefit without rendering, audio or gameplay regressions.
No FPS or percentage improvement is promised before these comparisons.

## Existing investigation records

- [CPU/GPU optimization audit](optimization-audit.md)
- [Texture and frame-pacing hitch work](hitch-reduction.md)
- [Android CPU and audio work](android-cpu-audio.md)
