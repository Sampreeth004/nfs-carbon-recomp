// Android audio output: a stereo AAudio stream fed by the guest's 5.1 mix.
//
// The phone advertises its speaker as a 6 channel device, and the SDK's SDL
// path then submits 5.1 and leaves the downmix to the system, which tends to
// drop the center channel where the dialogue lives. This backend opens a stereo
// stream directly and folds 5.1 itself, with a limiter so a louder fold cannot
// clip.
#pragma once

#if defined(__ANDROID__)

#include <aaudio/AAudio.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/thread.h>

namespace carbon::audio {

using rex::X_STATUS;

class AAudioDriver final : public rex::audio::AudioDriver {
 public:
  AAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~AAudioDriver() override;

  bool Initialize();
  void Shutdown();
  void SubmitFrame(uint32_t frame_ptr) override;

 private:
  static aaudio_data_callback_result_t OnData(AAudioStream* stream, void* user, void* data,
                                              int32_t frames);
  static void OnError(AAudioStream* stream, void* user, aaudio_result_t error);

  bool OpenStream();
  void CloseStream();

 public:
  void SetPaused(bool paused);

 private:

  // One guest block: 256 samples per channel, folded to stereo.
  static constexpr size_t kBlockFrames = 256;
  static constexpr size_t kBlockFloats = kBlockFrames * 2;
  // The audio system never has more than kMaximumQueuedFrames (64) blocks in
  // flight, so a ring of that many blocks cannot overflow.
  static constexpr size_t kRingFloats = 64 * kBlockFloats;
  static_assert((kRingFloats & (kRingFloats - 1)) == 0, "ring size must be a power of two");
  static constexpr size_t kRingMask = kRingFloats - 1;

  rex::thread::Semaphore* semaphore_ = nullptr;
  AAudioStream* stream_ = nullptr;
  std::mutex control_;  // open/close/reconnect only, never taken by the data callback
  std::thread reconnect_;
  std::atomic<bool> shutting_down_{false};

  // Single producer (SubmitFrame) and single consumer (OnData), lock free: the
  // real-time callback must never wait for the game thread.
  std::unique_ptr<float[]> ring_;
  alignas(64) std::atomic<size_t> write_{0};
  alignas(64) std::atomic<size_t> read_{0};
  size_t consumed_floats_ = 0;  // callback only

  float limiter_gain_ = 1.0f;  // producer only
  // Callback only increments counters; formatting/file logging happens on
  // the producer so a missed audio deadline cannot block the callback again.
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> missing_frames_{0};
  uint64_t reported_underruns_ = 0;  // producer only
  std::chrono::steady_clock::time_point next_diagnostic_{};
};

// Pauses or resumes the audio output (app sent to the background and back).
void SetOutputPaused(bool paused);

class AAudioSystem final : public rex::audio::AudioSystem {
 public:
  explicit AAudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher);
  ~AAudioSystem() override;

  static bool IsAvailable() { return true; }
  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};

}  // namespace carbon::audio

#endif  // __ANDROID__
