#if defined(__ANDROID__)

#include "android_aaudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include <rex/logging.h>

namespace carbon::audio {

namespace {

constexpr int32_t kSampleRate = 48000;

// Fold weights for the guest's fl fr fc lf bl br. Louder than the usual
// -3 dB / unit-sum fold: phone speakers are small and dialogue sits in the
// center channel, so the center stays at full level and the limiter keeps the
// sum from clipping.
constexpr float kCenter = 1.0f;
constexpr float kSurround = 0.7f;
constexpr float kLfe = 0.35f;
constexpr float kMaster = 0.85f;
constexpr float kCeiling = 0.97f;
// Limiter release: about 40 ms at 48 kHz.
constexpr float kRelease = 0.0005f;

std::atomic<AAudioDriver*> g_driver{nullptr};
std::atomic<bool> g_output_paused{false};

inline float BigEndianFloat(float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  bits = __builtin_bswap32(bits);
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

}  // namespace

AAudioDriver::AAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore), ring_(new float[kRingFloats]()) {}

AAudioDriver::~AAudioDriver() {
  Shutdown();
}

bool AAudioDriver::Initialize() {
  if (!OpenStream()) {
    return false;
  }
  g_driver.store(this);
  if (g_output_paused.load()) {
    SetPaused(true);
  }
  return true;
}

void AAudioDriver::SetPaused(bool paused) {
  std::lock_guard<std::mutex> lock(control_);
  if (!stream_) {
    return;
  }
  if (paused) {
    AAudioStream_requestPause(stream_);
  } else {
    AAudioStream_requestStart(stream_);
  }
}

void SetOutputPaused(bool paused) {
  g_output_paused.store(paused);
  if (AAudioDriver* driver = g_driver.load()) {
    driver->SetPaused(paused);
  }
}

void AAudioDriver::Shutdown() {
  AAudioDriver* self = this;
  g_driver.compare_exchange_strong(self, nullptr);
  shutting_down_ = true;
  if (reconnect_.joinable()) {
    reconnect_.join();
  }
  CloseStream();
}

bool AAudioDriver::OpenStream() {
  std::lock_guard<std::mutex> lock(control_);
  if (stream_) {
    return true;
  }
  AAudioStreamBuilder* builder = nullptr;
  if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
    REXAPU_ERROR("AAudio: could not create a stream builder");
    return false;
  }
  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(builder, 2);
  AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_GAME);
  AAudioStreamBuilder_setDataCallback(builder, &AAudioDriver::OnData, this);
  AAudioStreamBuilder_setErrorCallback(builder, &AAudioDriver::OnError, this);

  AAudioStream* stream = nullptr;
  aaudio_result_t result = AAudioStreamBuilder_openStream(builder, &stream);
  AAudioStreamBuilder_delete(builder);
  if (result != AAUDIO_OK || !stream) {
    REXAPU_ERROR("AAudio: openStream failed: {}", AAudio_convertResultToText(result));
    return false;
  }
  // Headroom for scheduling hiccups: the game threads are busy.
  int32_t burst = AAudioStream_getFramesPerBurst(stream);
  AAudioStream_setBufferSizeInFrames(stream, std::max<int32_t>(burst * 4, 2048));
  REXAPU_INFO("AAudio: {} Hz, {} ch, burst {} frames, buffer {} frames, mode {}",
              AAudioStream_getSampleRate(stream), AAudioStream_getChannelCount(stream), burst,
              AAudioStream_getBufferSizeInFrames(stream),
              static_cast<int>(AAudioStream_getPerformanceMode(stream)));

  write_.store(0, std::memory_order_relaxed);
  read_.store(0, std::memory_order_relaxed);
  consumed_floats_ = 0;
  result = AAudioStream_requestStart(stream);
  if (result != AAUDIO_OK) {
    REXAPU_ERROR("AAudio: requestStart failed: {}", AAudio_convertResultToText(result));
    AAudioStream_close(stream);
    return false;
  }
  stream_ = stream;
  return true;
}

void AAudioDriver::CloseStream() {
  std::lock_guard<std::mutex> lock(control_);
  if (!stream_) {
    return;
  }
  AAudioStream_requestStop(stream_);
  AAudioStream_close(stream_);
  stream_ = nullptr;
}

void AAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const float* in = memory_->TranslateVirtual<const float*>(frame_ptr);
  float block[kBlockFloats];
  float gain = limiter_gain_;
  for (size_t i = 0; i < kBlockFrames; ++i) {
    const float fl = BigEndianFloat(in[0 * kBlockFrames + i]);
    const float fr = BigEndianFloat(in[1 * kBlockFrames + i]);
    const float fc = BigEndianFloat(in[2 * kBlockFrames + i]);
    const float lf = BigEndianFloat(in[3 * kBlockFrames + i]);
    const float bl = BigEndianFloat(in[4 * kBlockFrames + i]);
    const float br = BigEndianFloat(in[5 * kBlockFrames + i]);
    const float mid = fc * kCenter + lf * kLfe;
    const float l = (fl + mid + bl * kSurround) * kMaster;
    const float r = (fr + mid + br * kSurround) * kMaster;
    const float peak = std::max(std::fabs(l), std::fabs(r));
    const float target = peak > kCeiling ? kCeiling / peak : 1.0f;
    gain = target < gain ? target : gain + (target - gain) * kRelease;
    block[i * 2] = std::clamp(l * gain, -1.0f, 1.0f);
    block[i * 2 + 1] = std::clamp(r * gain, -1.0f, 1.0f);
  }
  limiter_gain_ = gain;

  const size_t w = write_.load(std::memory_order_relaxed);
  const size_t r = read_.load(std::memory_order_acquire);
  if (w - r + kBlockFloats > kRingFloats) {
    // Cannot happen with the semaphore flow control; give the permit back
    // instead of stalling the audio worker if it ever does.
    semaphore_->Release(1, nullptr);
    return;
  }
  const size_t start = w & kRingMask;
  const size_t first = std::min(kBlockFloats, kRingFloats - start);
  std::memcpy(&ring_[start], block, first * sizeof(float));
  if (first < kBlockFloats) {
    std::memcpy(&ring_[0], block + first, (kBlockFloats - first) * sizeof(float));
  }
  write_.store(w + kBlockFloats, std::memory_order_release);
}

aaudio_data_callback_result_t AAudioDriver::OnData(AAudioStream*, void* user, void* data,
                                                   int32_t frames) {
  auto* self = static_cast<AAudioDriver*>(user);
  float* out = static_cast<float*>(data);
  const size_t need = static_cast<size_t>(frames) * 2;
  const size_t r = self->read_.load(std::memory_order_relaxed);
  const size_t w = self->write_.load(std::memory_order_acquire);
  const size_t take = std::min(need, w - r);
  if (take) {
    const size_t start = r & kRingMask;
    const size_t first = std::min(take, kRingFloats - start);
    std::memcpy(out, &self->ring_[start], first * sizeof(float));
    if (first < take) {
      std::memcpy(out + first, &self->ring_[0], (take - first) * sizeof(float));
    }
    self->read_.store(r + take, std::memory_order_release);
  }
  if (take < need) {
    std::memset(out + take, 0, (need - take) * sizeof(float));
    ++self->underruns_;
    if (self->underruns_ <= 5 || self->underruns_ % 500 == 0) {
      REXAPU_WARN("AAudio: underrun #{}", self->underruns_);
    }
  }
  // One permit back to the audio worker per guest block played.
  self->consumed_floats_ += take;
  while (self->consumed_floats_ >= kBlockFloats) {
    self->consumed_floats_ -= kBlockFloats;
    self->semaphore_->Release(1, nullptr);
  }
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void AAudioDriver::OnError(AAudioStream*, void* user, aaudio_result_t error) {
  auto* self = static_cast<AAudioDriver*>(user);
  if (error != AAUDIO_ERROR_DISCONNECTED || self->shutting_down_) {
    return;
  }
  // The stream cannot be closed from its own error callback: reopen it (the
  // headphones or the speaker route changed) from another thread.
  if (self->reconnect_.joinable()) {
    return;
  }
  self->reconnect_ = std::thread([self]() {
    self->CloseStream();
    for (int attempt = 0; attempt < 10 && !self->shutting_down_; ++attempt) {
      if (self->OpenStream()) {
        REXAPU_INFO("AAudio: reconnected");
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  });
}

AAudioSystem::AAudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

AAudioSystem::~AAudioSystem() = default;

std::unique_ptr<rex::audio::AudioSystem> AAudioSystem::Create(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<AAudioSystem>(function_dispatcher);
}

X_STATUS AAudioSystem::CreateDriver(size_t, rex::thread::Semaphore* semaphore,
                                    rex::audio::AudioDriver** out_driver) {
  auto* driver = new AAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void AAudioSystem::DestroyDriver(rex::audio::AudioDriver* driver) {
  auto* aaudio = dynamic_cast<AAudioDriver*>(driver);
  if (aaudio) {
    aaudio->Shutdown();
  }
  delete driver;
}

}  // namespace carbon::audio

#endif  // __ANDROID__
