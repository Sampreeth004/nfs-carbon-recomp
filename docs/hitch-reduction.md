# Texture and frame pacing hitches (0.3.10)

## Evidence

In v0.3.9's `nfscarbon_059.log`, a driving frame at 09:30:04.310 took
50.0 ms, including 31.3 ms in five texture uploads. No pipelines were built
inline. Many other slow frames lasted approximately 33 ms, with no uploads or
pipeline builds and a long guest synchronization wait. That identifies a
  texture stall and missed frame deadlines, but the wait duration alone does
not prove whether a particular deadline was missed by CPU work, scheduling,
  or the independent host cap.

The configuration fetched before this update had `carbon_gpu_fps_cap = 0`.
Therefore the duplicate host limiter was inactive in that session; removing it
helps capped configurations but cannot explain or fix that session's hitches.
The original auto-affinity policy also excluded Carbon's `MainThread` from the
prime cores, despite this thread accounting for about 22% of the earlier
process CPU samples. Frame production and CPU scheduling remain relevant.

The phone's driver supports native BC textures. These hitches do not require
software DXT decompression: the original decoder still walked every guest byte
to untile/endian-swap native blocks and uncompressed images.

## Changes

- `texture_copy.h` copies whole spans, with NEON endian conversion on ARM64.
  Unaligned sub-word texels and physical-memory wrap keep address-relative
  scalar handling.
- Unconverted textures copy entire linear rows or contiguous 2D microtile
  runs directly into staging memory. Runs stop at microtile boundaries,
  including packed mip offsets. Tiled 3D blocks keep their individual address
  lookup. Format conversions keep their pixel conversion logic.
- The guest vblank clock already paces Carbon at 60 Hz. A 60 FPS host cap
  previously inserted another independently scheduled sleep on the command
  thread after swap. That sleep is now omitted when guest vsync is enabled
  and the requested cap is at least the guest refresh rate. A lower cap,
  such as 30 FPS, and vsync-disabled runs retain the host cap. This removes
  the duplicate limiter without reducing or skipping guest synchronization.
- Slow-frame logs now split texture upload time into decode and staging
  portions, helping distinguish conversion from allocation stalls.
- Android auto affinity allows `MainThread` to use every core, including prime
  cores. The two existing prime-pinned threads keep their masks, and other
  threads keep the remaining cores. This permits the scheduler to place Carbon's
  frame-producing worker on fast cores without forcing three busy threads onto
  the prime-only mask. Explicit user rules and equal-capacity CPUs are unchanged.

## Correctness and decode measurements

`texture_copy_test.cpp` compares copies against byte-address XOR for all four
endian modes, source offsets 0–31, lengths 0–4096 and output guard bytes. It
also checks host-cap decisions for 30/60/120 FPS, 59.94/60/120 Hz and vsync
on/off. It passed on Windows and on the phone's ARM64 NEON implementation.
An additional Linux/Android test passed copies across the physical 512 MiB
address boundary for all endian modes, reserving virtual memory and touching
only the first and last pages.

`texture_decode_test.cpp` passed **8,448** byte-for-byte comparisons with the
pre-change decoder on the phone: native and software BC, uncompressed and
converted formats, all endian modes, linear/tiled storage, packed mips,
non-power-of-two sizes, 2D/3D/cube images and nonzero minimum mip levels.
Destination padding/guard bytes were included in the comparisons.

Median of 15 same-process decodes per path on the CPH2723 (ARM64, `-O3`),
1024×1024, tiled, `k8in16`, native BC enabled:

| Texture | Original decode | Updated decode |
| --- | ---: | ---: |
| RGBA8 | 13.412 ms | 2.237 ms |
| BC1 / DXT1 | 0.864 ms | 0.279 ms |
| BC3 / DXT5 | 1.168 ms | 0.554 ms |

These measure CPU decoding only. They do not measure Vulkan allocation,
driver upload time, total CPU usage or end-to-end FPS. Source texture data
and final image quality are preserved; there is no deferred upload or blank
texture substitution.

## Reproducing the differential test

The original decoder is in commit
`be6e704b1769869922377236f0e860cd02d0c35b:renderer/src/texture_decode.cpp`.
Create `out/texture_decode_reference.cpp` from that source, prepending macros
that rename these exported functions to their names with `Reference` appended:
`GetTextureFormatInfo`, `GuestTextureFromFetch`, `GetGuestTextureExtent`,
`GetHostTextureSize`, `DecodeGuestTexture`, `TiledOffset2D`, `TiledOffset3D`.
The macros also rename their declarations and internal calls. Do not rename
the structures or namespace.

Compile `tests/texture_decode_test.cpp`, `renderer/src/texture_decode.cpp` and
the reference source together with the NDK's Clang++,
`--target=aarch64-linux-android28 -std=c++23 -O3 -static-libstdc++`,
`-DREX_HAS_VULKAN=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL`,
and include paths `renderer/src` and `../tools/rexglue-sdk-android/include`.
Push the executable to `/data/local/tmp`, make it executable and run it.
The copy/pacing test needs only C++23 and the project root include path.

Local test outputs are `out/texture_copy_test_arm64-result.txt` and
`out/texture_decode_test_arm64-result.txt`.

## Installed build and limits

Both ARM64 and x86_64 native builds and the debug APK build passed. Installed
versionCode 13 / versionName 0.3.10; artifact
`nfs-carbon-0.3.10-hitch-debug.apk`. The final startup log is `nfscarbon_061.log`,
saved as `out/hitch-fixed-final.log`.

The log confirms cap 0, no additional host sleep, and the new auto-affinity
rule. Reading the actual thread status after installation confirmed
`MainThread (F80` allowed cores 0–7, with `GPU Commands` and `Main XThread`
still allowed cores 6–7. This verifies the policy is applied, not that a
particular frame was scheduled on a prime core.

The final run reached the 3D scene and reported up to 60 FPS. Some slow frames
remained, including 33–50 ms frames during the scene transition. The user
switched away from the game during checking; no screenshot of that app or
further game restart was taken. These logs are not a controlled driving
comparison. The large texture-decode improvement is measured directly, but
elimination of all intermittent CPU/scheduling hitches is not established.
