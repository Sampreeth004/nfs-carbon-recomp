# Android event-loop CPU usage and audio

## 0.3.8 changes

SDL's Android `SDL_WaitEventTimeoutNS` loop previously called
`SDL_PumpEventsInternal(true)` on every iteration. That inserts a
`SDL_EVENT_POLL_SENTINEL`, which goes through `SDL_PushEvent` and sends an
Android lifecycle WAKE. The next `Android_PumpEvents(delay)` consumes that wake
and returns immediately. The loop continually wakes itself instead of sleeping.

`patches/rexglue-sdl-android-wait.patch` changes only this blocking Android loop
to `SDL_PumpEventsInternal(false)`. The initial pump and `SDL_PollEvent` retain
their sentinel behavior. Real input, lifecycle events and deferred UI work
still wake the wait through SDL's existing lifecycle semaphore. There is no
added sleep or input delay.

Apply the patch to the SDK source root, rebuild `rexruntime` for both Android
architectures, and copy the new `librexruntime.so` into the matching SDK install
and APK `jniLibs` directories. Rebuilding only the game library does not apply
this SDK fix.

The AAudio callback now records underrun callback and missing-frame counts
using atomics. It no longer formats or writes underrun logs. `SubmitFrame`
reports changed totals at most once every five seconds. Audio queue size,
sample rate, mixing and frame-rate settings are unchanged.

## Phone measurements

CPH2723, Carbon renderer, 2640x1216 output, 48 kHz stereo. Both versions were
profiled using `simpleperf record -e cpu-clock -f 1000 --duration 10`.

| Measurement | 0.3.7 | 0.3.8 |
| --- | ---: | ---: |
| CPU-clock samples | 17,021 | 11,324 |
| SDLThread share of samples | 58.07% | No samples |
| SDLThread sampled CPU seconds | About 9.88 | 0 |

The game scene and startup state differed between captures, so total process
CPU and FPS are not a controlled comparison. The removal of the continuously
running SDL thread is the relevant result; a `top -H` snapshot also showed it
sleeping after the fix.

The initial audio diagnostic reported 28 callbacks with 4,992 missing frames
at startup. No additional underrun totals appeared during the first roughly
35 seconds of subsequent rendering. This establishes removal of the busy
loop, not a guarantee that all audible glitches are fixed. Decoder errors,
Android output underruns, route changes and longer gameplay still need to be
distinguished if glitches continue.
