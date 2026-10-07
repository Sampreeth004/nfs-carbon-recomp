// Carbon native renderer: PM4 ring reader.
//
// Packet semantics follow the Xenos documentation in the SDK headers and the
// behaviour the game's Direct3D expects (the same contract the SDK's
// command_processor.cpp implements, BSD-licensed Xenia code). Everything that
// draws goes to the Renderer; everything the guest can observe (memory
// writes, interrupts, the read pointer) is done here at parse time.

#include "command_processor.h"

#include <algorithm>
#include <chrono>
#include <climits>

#include <rex/cvar.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xmemory.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "gpu_system.h"
#include "renderer.h"

// The game-side ring wait hook (src/ring_wait_hook.cpp) sleeps on this
// counter instead of spinning while D3D waits for ring space. Same name as
// the SDK plugin exports, so the hook finds either renderer.
extern "C" REX_GPU_PLUGIN_EXPORT std::atomic<uint32_t> nfsmw_cp_rptr_seq{0};

REXCVAR_DEFINE_BOOL(carbon_gpu_threaded_cp, true, "CarbonGPU",
                    "Fetch PM4 packets on a separate thread so the game never waits for ring "
                    "space while draws are recorded (false = single command thread)");

namespace carbon::gpu {

using xenos::Type3Opcode;

namespace {

void WakeRptrWaiters() {
  nfsmw_cp_rptr_seq.fetch_add(1, std::memory_order_release);
#if defined(__linux__)
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&nfsmw_cp_rptr_seq), FUTEX_WAKE_PRIVATE, INT_MAX,
          nullptr, nullptr, 0);
#endif
}

}  // namespace

CommandProcessor::CommandProcessor(GpuSystem* system, Renderer* renderer)
    : system_(system), renderer_(renderer), memory_(system->memory()) {
  write_ptr_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  vblank_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  exec_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  space_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  // Linear gamma ramps until the game writes its own.
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t v = i * 0x3FF / 0xFF;
    gamma_ramp_.table[i] = v | (v << 10) | (v << 20);
  }
  for (uint32_t i = 0; i < 128; ++i) {
    uint32_t base = (i * 0xFFFF / 0x7F) & ~uint32_t(0x3F);
    uint32_t delta = i < 0x7F ? 0x200 : 0;
    for (uint32_t c = 0; c < 3; ++c) {
      gamma_ramp_.pwl[i][c] = base | (delta << 16);
    }
  }
}

CommandProcessor::~CommandProcessor() { Stop(); }

bool CommandProcessor::Start() {
  running_ = true;
  threaded_ = REXCVAR_GET(carbon_gpu_threaded_cp);
  if (threaded_) {
    staging_ = std::make_unique_for_overwrite<uint32_t[]>(kStagingDwords);
    staging_head_.store(0);
    staging_tail_.store(0);
    fetch_head_ = 0;
  }
  REXGPU_INFO("[carbon-gpu] command processing: {}",
              threaded_ ? "fetch thread + executor thread" : "single thread");
  // The executor keeps the name the CPU affinity rules pin to the fast cores.
  worker_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
      system_->kernel_state(), 256 * 1024, 0, [this]() {
        if (threaded_) {
          ExecutorMain();
        } else {
          WorkerMain();
        }
        return 0;
      }));
  worker_->set_name("GPU Commands");
  worker_->Create();
  if (threaded_) {
    fetch_worker_ = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
        system_->kernel_state(), 256 * 1024, 0, [this]() {
          FetchMain();
          return 0;
        }));
    fetch_worker_->set_name("GPU Fetch");
    fetch_worker_->Create();
  }
  return true;
}

void CommandProcessor::Stop() {
  if (!worker_) {
    return;
  }
  running_ = false;
  write_ptr_event_->Set();
  vblank_event_->Set();
  exec_event_->Set();
  space_event_->Set();
  if (fetch_worker_) {
    fetch_worker_->Wait(0, 0, 0, nullptr);
    fetch_worker_.reset();
  }
  worker_->Wait(0, 0, 0, nullptr);
  worker_.reset();
  staging_.reset();
}

void CommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  REXGPU_INFO("[carbon-gpu] ring buffer at {:08X}, size_log2 {}", ptr, size_log2);
  read_ptr_index_ = 0;
  primary_buffer_ptr_ = ptr;
  // size_log2 is in quadwords.
  primary_buffer_size_dwords_ = uint32_t(1) << (size_log2 + 1);
}

void CommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  read_ptr_writeback_ptr_ = ptr;
  // RB_BLKSZ is log2 of quadwords read between read pointer updates.
  read_ptr_update_dwords_ = (uint32_t(1) << std::min(block_size_log2, uint32_t(19))) * 2;
}

void CommandProcessor::UpdateWritePointer(uint32_t value) {
  write_ptr_index_.store(value, std::memory_order_release);
  write_ptr_event_->Set();
}

void CommandProcessor::NotifyVblank() { vblank_event_->Set(); }

void CommandProcessor::CallInThread(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.push_back(std::move(fn));
  }
  has_pending_.store(true, std::memory_order_release);
  write_ptr_event_->Set();
  exec_event_->Set();
}

void CommandProcessor::WorkerMain() {
  REXGPU_INFO("[carbon-gpu] command processor thread started");
  renderer_->OnCommandThreadStart();
  while (running_) {
    if (has_pending_.load(std::memory_order_acquire)) {
      std::vector<std::function<void()>> fns;
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        fns.swap(pending_);
        has_pending_.store(false, std::memory_order_relaxed);
      }
      for (auto& fn : fns) {
        fn();
      }
    }
    uint32_t write_index = write_ptr_index_.load(std::memory_order_acquire);
    if (write_index == 0xBAADF00Du || write_index == read_ptr_index_ || !primary_buffer_size_dwords_) {
      // Nothing to do: a short spin, then sleep on the write pointer event.
      uint32_t spins = 0;
      while (running_ && !has_pending_.load(std::memory_order_acquire)) {
        write_index = write_ptr_index_.load(std::memory_order_acquire);
        if (write_index != 0xBAADF00Du && write_index != read_ptr_index_ &&
            primary_buffer_size_dwords_) {
          break;
        }
        if (++spins > 16) {
          auto idle_start = std::chrono::steady_clock::now();
          rex::thread::Wait(write_ptr_event_.get(), false, std::chrono::milliseconds(2));
          renderer_->NoteCpIdle(std::chrono::duration<double>(
              std::chrono::steady_clock::now() - idle_start).count());
        } else {
          rex::thread::MaybeYield();
        }
      }
      continue;
    }
    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_index);
    PublishReadPointer(read_ptr_index_);
  }
  renderer_->OnCommandThreadStop();
}

void CommandProcessor::PublishReadPointer(uint32_t read_index) {
  const uint32_t writeback = read_ptr_writeback_ptr_;
  if (writeback) {
    std::atomic_thread_fence(std::memory_order_release);
    uint32_t* dst = memory_->TranslatePhysical<uint32_t*>(writeback);
    *dst = ByteSwap32(read_index);
  }
  WakeRptrWaiters();
}

uint32_t CommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  Reader r;
  r.base = memory_->TranslatePhysical<const uint32_t*>(primary_buffer_ptr_);
  r.capacity = primary_buffer_size_dwords_;
  r.read = read_index & (primary_buffer_size_dwords_ - 1);
  r.end = write_index & (primary_buffer_size_dwords_ - 1);
  r.ring = true;
  const uint32_t step = read_ptr_update_dwords_;
  uint32_t published_remaining = r.remaining();
  while (r.remaining()) {
    if (!ExecutePacket(r)) {
      REXGPU_ERROR("[carbon-gpu] bad packet in the primary ring at {:08X}, skipping the rest",
                   primary_buffer_ptr_ + r.read * 4);
      break;
    }
    // Publish the read pointer as the ring drains, not only at the end: the
    // game watches it for free space.
    const uint32_t left = r.remaining();
    if (step && left <= published_remaining && published_remaining - left >= step) {
      PublishReadPointer(r.read);
      published_remaining = left;
    }
  }
  renderer_->OnPrimaryBufferEnd();
  return write_index;
}

void CommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
  Reader r;
  r.base = memory_->TranslatePhysical<const uint32_t*>(ptr);
  r.capacity = count;
  r.read = 0;
  r.end = count;
  r.ring = false;
  while (r.remaining()) {
    if (!ExecutePacket(r)) {
      REXGPU_ERROR("[carbon-gpu] bad packet in an indirect buffer at {:08X}", ptr);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Threaded mode: fetch (stage packets) and execute
// ---------------------------------------------------------------------------

void CommandProcessor::FetchMain() {
  REXGPU_INFO("[carbon-gpu] command fetch thread started");
  while (running_) {
    uint32_t write_index = write_ptr_index_.load(std::memory_order_acquire);
    if (write_index == 0xBAADF00Du || write_index == read_ptr_index_ ||
        !primary_buffer_size_dwords_) {
      uint32_t spins = 0;
      while (running_) {
        write_index = write_ptr_index_.load(std::memory_order_acquire);
        if (write_index != 0xBAADF00Du && write_index != read_ptr_index_ &&
            primary_buffer_size_dwords_) {
          break;
        }
        if (++spins > 16) {
          rex::thread::Wait(write_ptr_event_.get(), false, std::chrono::milliseconds(2));
        } else {
          rex::thread::MaybeYield();
        }
      }
      continue;
    }
    read_ptr_index_ = FetchPrimaryBuffer(read_ptr_index_, write_index);
    PublishReadPointer(read_ptr_index_);
  }
}

uint32_t CommandProcessor::FetchPrimaryBuffer(uint32_t read_index, uint32_t write_index) {
  Reader r;
  r.base = memory_->TranslatePhysical<const uint32_t*>(primary_buffer_ptr_);
  r.capacity = primary_buffer_size_dwords_;
  r.read = read_index & (primary_buffer_size_dwords_ - 1);
  r.end = write_index & (primary_buffer_size_dwords_ - 1);
  r.ring = true;
  const uint32_t step = read_ptr_update_dwords_;
  uint32_t published_remaining = r.remaining();
  while (r.remaining()) {
    if (!StagePacket(r, 0)) {
      if (running_) {
        REXGPU_ERROR("[carbon-gpu] bad packet in the primary ring at {:08X}, skipping the rest",
                     primary_buffer_ptr_ + r.read * 4);
      }
      break;
    }
    // The packets are copied into the queue, so the ring space can be given back
    // right away (the hardware read pointer is a fetch pointer too).
    const uint32_t left = r.remaining();
    if (step && left <= published_remaining && published_remaining - left >= step) {
      PublishReadPointer(r.read);
      published_remaining = left;
    }
  }
  exec_event_->Set();
  return write_index;
}

void CommandProcessor::FetchIndirectBuffer(uint32_t ptr, uint32_t count, uint32_t depth) {
  Reader r;
  r.base = memory_->TranslatePhysical<const uint32_t*>(ptr);
  r.capacity = count;
  r.read = 0;
  r.end = count;
  r.ring = false;
  while (r.remaining()) {
    if (!StagePacket(r, depth)) {
      if (running_) {
        REXGPU_ERROR("[carbon-gpu] bad packet in an indirect buffer at {:08X}", ptr);
      }
      break;
    }
  }
}

// Frames one packet at the reader and stages it for the executor. Mirrors the
// framing, predication and bin-state rules of ExecutePacket/ExecuteType3; indirect
// buffers are expanded here, so the executor never sees INDIRECT_BUFFER packets.
bool CommandProcessor::StagePacket(Reader& r, uint32_t depth) {
  auto raw_index = [&](uint32_t i) {
    return r.ring ? ((r.read + i) & (r.capacity - 1)) : r.read + i;
  };
  const uint32_t packet = ByteSwap32(r.base[r.read]);
  if (packet == 0) {
    r.Skip(1);
    return true;
  }
  switch (packet >> 30) {
    case 0: {
      const uint32_t total = ((packet >> 16) & 0x3FFF) + 2;
      if (r.remaining() < total) {
        return false;
      }
      return StagingWrite(r, total);
    }
    case 1:
      if (r.remaining() < 3) {
        return false;
      }
      return StagingWrite(r, 3);
    case 2:
      r.Skip(1);
      return true;
    default:
      break;
  }
  const uint32_t total = ((packet >> 16) & 0x3FFF) + 2;
  if (r.remaining() < total) {
    return false;
  }
  auto peek = [&](uint32_t i) { return i < total ? ByteSwap32(r.base[raw_index(i)]) : 0u; };
  const uint32_t opcode = (packet >> 8) & 0x7F;
  if (packet & 1) {
    if (!(fe_bin_select_ & fe_bin_mask_) || opcode == xenos::PM4_XE_SWAP) {
      r.Skip(total);
      return true;
    }
  }
  switch (opcode) {
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      const uint32_t list_ptr = peek(1) & 0x1FFFFFFF;
      const uint32_t list_length = peek(2) & 0xFFFFF;
      r.Skip(total);
      if (depth < 8) {
        FetchIndirectBuffer(list_ptr, list_length, depth + 1);
      }
      return true;
    }
    case xenos::PM4_SET_BIN_MASK_LO:
      fe_bin_mask_ = (fe_bin_mask_ & 0xFFFFFFFF00000000ull) | peek(1);
      break;
    case xenos::PM4_SET_BIN_MASK_HI:
      fe_bin_mask_ = (fe_bin_mask_ & 0xFFFFFFFFull) | (uint64_t(peek(1)) << 32);
      break;
    case xenos::PM4_SET_BIN_SELECT_LO:
      fe_bin_select_ = (fe_bin_select_ & 0xFFFFFFFF00000000ull) | peek(1);
      break;
    case xenos::PM4_SET_BIN_SELECT_HI:
      fe_bin_select_ = (fe_bin_select_ & 0xFFFFFFFFull) | (uint64_t(peek(1)) << 32);
      break;
    case xenos::PM4_SET_BIN_MASK:
      fe_bin_mask_ = (uint64_t(peek(1)) << 32) | peek(2);
      break;
    case xenos::PM4_SET_BIN_SELECT:
      fe_bin_select_ = (uint64_t(peek(1)) << 32) | peek(2);
      break;
    default:
      break;
  }
  return StagingWrite(r, total);
}

// Copies `total` raw dwords from the reader into the queue (waiting for space) and
// publishes them to the executor.
bool CommandProcessor::StagingWrite(Reader& r, uint32_t total) {
  const uint32_t mask = kStagingDwords - 1;
  bool counted = false;
  while (true) {
    const uint32_t used = fetch_head_ - staging_tail_.load(std::memory_order_acquire);
    if (used + total + 1 <= kStagingDwords) {
      if (used + total > staged_max_.load(std::memory_order_relaxed)) {
        staged_max_.store(used + total, std::memory_order_relaxed);
      }
      break;
    }
    if (!running_) {
      return false;
    }
    if (!counted) {
      counted = true;
      fetch_full_waits_.fetch_add(1, std::memory_order_relaxed);
    }
    exec_event_->Set();
    fetch_waiting_.store(true, std::memory_order_release);
    if (fetch_head_ - staging_tail_.load(std::memory_order_acquire) + total + 1 <=
        kStagingDwords) {
      fetch_waiting_.store(false, std::memory_order_relaxed);
      continue;
    }
    rex::thread::Wait(space_event_.get(), false, std::chrono::milliseconds(1));
    fetch_waiting_.store(false, std::memory_order_relaxed);
  }
  uint32_t dst = fetch_head_ & mask;
  uint32_t src = r.read;
  uint32_t done = 0;
  while (done < total) {
    uint32_t run = total - done;
    if (r.ring) {
      run = std::min(run, r.capacity - src);
    }
    run = std::min(run, kStagingDwords - dst);
    std::memcpy(staging_.get() + dst, r.base + src, size_t(run) * 4);
    done += run;
    src = r.ring ? ((src + run) & (r.capacity - 1)) : src + run;
    dst = (dst + run) & mask;
  }
  r.Skip(total);
  fetch_head_ += total;
  staging_head_.store(fetch_head_, std::memory_order_release);
  if (exec_sleeping_.load(std::memory_order_acquire)) {
    exec_event_->Set();
  }
  return true;
}

void CommandProcessor::ExecutorMain() {
  REXGPU_INFO("[carbon-gpu] command processor thread started (executor)");
  renderer_->OnCommandThreadStart();
  const uint32_t mask = kStagingDwords - 1;
  uint32_t tail = 0;
  auto last_report = std::chrono::steady_clock::now();
  while (running_) {
    if (has_pending_.load(std::memory_order_acquire)) {
      std::vector<std::function<void()>> fns;
      {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        fns.swap(pending_);
        has_pending_.store(false, std::memory_order_relaxed);
      }
      for (auto& fn : fns) {
        fn();
      }
    }
    uint32_t head = staging_head_.load(std::memory_order_acquire);
    if (head == tail) {
      uint32_t spins = 0;
      while (running_ && !has_pending_.load(std::memory_order_acquire)) {
        head = staging_head_.load(std::memory_order_acquire);
        if (head != tail) {
          break;
        }
        if (++spins > 16) {
          auto idle_start = std::chrono::steady_clock::now();
          exec_sleeping_.store(true, std::memory_order_release);
          if (staging_head_.load(std::memory_order_acquire) == tail) {
            rex::thread::Wait(exec_event_.get(), false, std::chrono::milliseconds(2));
          }
          exec_sleeping_.store(false, std::memory_order_relaxed);
          renderer_->NoteCpIdle(std::chrono::duration<double>(
              std::chrono::steady_clock::now() - idle_start).count());
        } else {
          rex::thread::MaybeYield();
        }
      }
      continue;
    }
    Reader r;
    r.base = staging_.get();
    r.capacity = kStagingDwords;
    r.read = tail & mask;
    r.end = head & mask;
    r.ring = true;
    while (r.remaining()) {
      const uint32_t before = r.read;
      if (!ExecutePacket(r)) {
        if (running_) {
          REXGPU_ERROR("[carbon-gpu] bad packet in the command queue, skipping the rest");
        }
        r.read = r.end;
      }
      tail += (r.read - before) & mask;
      staging_tail_.store(tail, std::memory_order_release);
      if (fetch_waiting_.load(std::memory_order_relaxed)) {
        space_event_->Set();
      }
      if (!running_) {
        break;
      }
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_report >= std::chrono::seconds(5)) {
      last_report = now;
      REXGPU_INFO(
          "[carbon-gpu] command queue: peak {} of {} KiB used, fetch waited for space {} times",
          (staged_max_.exchange(0, std::memory_order_relaxed) * 4) >> 10,
          (kStagingDwords * 4) >> 10, fetch_full_waits_.exchange(0, std::memory_order_relaxed));
    }
  }
  renderer_->OnCommandThreadStop();
}

bool CommandProcessor::ExecutePacket(Reader& r) {
  const uint32_t packet = r.Read();
  if (packet == 0) {
    return true;
  }
  switch (packet >> 30) {
    case 0: {
      uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
      if (r.remaining() < count) {
        return false;
      }
      uint32_t base = packet & 0x7FFF;
      bool one_reg = (packet >> 15) & 1;
      if (one_reg) {
        for (uint32_t i = 0; i < count; ++i) {
          WriteRegister(base, r.Read());
        }
      } else {
        WriteRegistersFromReader(r, base, count);
      }
      return true;
    }
    case 1: {
      uint32_t reg1 = packet & 0x7FF;
      uint32_t reg2 = (packet >> 11) & 0x7FF;
      uint32_t v1 = r.Read();
      uint32_t v2 = r.Read();
      WriteRegister(reg1, v1);
      WriteRegister(reg2, v2);
      return true;
    }
    case 2:
      return true;
    case 3:
      return ExecuteType3(r, packet);
  }
  return false;
}

void CommandProcessor::WriteRegistersFromReader(Reader& r, uint32_t base_index, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    WriteRegister(base_index + i, r.Read());
  }
}

void CommandProcessor::WriteRegistersFromGuest(uint32_t base_index, const uint32_t* guest_be,
                                               uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    WriteRegister(base_index + i, ByteSwap32(guest_be[i]));
  }
}

uint32_t CommandProcessor::ReadRegisterValue(uint32_t index) const {
  return index < kRegisterCount ? regs_.values[index] : 0;
}

void CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  if (index >= kRegisterCount) {
    return;
  }
  // Fast path for shader constants (the bulk of register traffic).
  if (index >= kRegFloatConstants) {
    regs_.values[index] = value;
    if (index < kRegFetchConstants) {
      renderer_->OnFloatConstantWritten(index - kRegFloatConstants);
    }
    return;
  }
  const_cast<volatile uint32_t&>(regs_.values[index]) = value;
  using namespace rex::graphics;
  if (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) {
    uint32_t scratch = index - XE_GPU_REG_SCRATCH_REG0;
    if ((uint32_t(1) << scratch) & regs_.values[XE_GPU_REG_SCRATCH_UMSK]) {
      uint32_t addr = regs_.values[XE_GPU_REG_SCRATCH_ADDR] + scratch * 4;
      *memory_->TranslatePhysical<uint32_t*>(addr) = ByteSwap32(value);
    }
    return;
  }
  switch (index) {
    case XE_GPU_REG_COHER_STATUS_HOST:
      const_cast<volatile uint32_t&>(regs_.values[index]) |= 0x80000000u;
      break;
    case XE_GPU_REG_DC_LUT_RW_INDEX:
      gamma_rw_component_ = 0;
      break;
    case XE_GPU_REG_DC_LUT_SEQ_COLOR: {
      uint32_t rw_index = regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
      bool write = (regs_.values[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                    (uint32_t(1) << (2 - gamma_rw_component_))) != 0;
      if (write) {
        uint32_t c = (value & 0xFFFF) >> 6;
        uint32_t& entry = gamma_ramp_.table[rw_index];
        // Red, green, blue order; stored as blue 0:9, green 10:19, red 20:29.
        uint32_t shift = gamma_rw_component_ == 0 ? 20 : (gamma_rw_component_ == 1 ? 10 : 0);
        entry = (entry & ~(uint32_t(0x3FF) << shift)) | ((c & 0x3FF) << shift);
        gamma_ramp_.table_dirty = true;
      }
      if (++gamma_rw_component_ >= 3) {
        gamma_rw_component_ = 0;
        regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] =
            (regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & ~uint32_t(0xFF)) | ((rw_index + 1) & 0xFF);
      }
    } break;
    case XE_GPU_REG_DC_LUT_PWL_DATA: {
      uint32_t rw_index = regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
      uint32_t pwl_index = rw_index & 0x7F;
      bool write = (regs_.values[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] &
                    (uint32_t(1) << (2 - gamma_rw_component_))) != 0;
      if (write) {
        uint32_t base = value & 0xFFC0;
        uint32_t delta = (value >> 16) & 0xFFC0;
        gamma_ramp_.pwl[pwl_index][gamma_rw_component_] = base | (delta << 16);
        gamma_ramp_.pwl_dirty = true;
      }
      if (++gamma_rw_component_ >= 3) {
        gamma_rw_component_ = 0;
        regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] =
            (rw_index & ~uint32_t(0x7F)) | ((pwl_index + 1) & 0x7F);
      }
    } break;
    case XE_GPU_REG_DC_LUT_30_COLOR: {
      uint32_t rw_index = regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & 0xFF;
      uint32_t mask = regs_.values[XE_GPU_REG_DC_LUT_WRITE_EN_MASK] & 0b111;
      uint32_t& entry = gamma_ramp_.table[rw_index];
      if (mask & 0b001) entry = (entry & ~0x3FFu) | (value & 0x3FF);
      if (mask & 0b010) entry = (entry & ~(0x3FFu << 10)) | (value & (0x3FFu << 10));
      if (mask & 0b100) entry = (entry & ~(0x3FFu << 20)) | (value & (0x3FFu << 20));
      if (mask) gamma_ramp_.table_dirty = true;
      gamma_rw_component_ = 0;
      regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] =
          (regs_.values[XE_GPU_REG_DC_LUT_RW_INDEX] & ~uint32_t(0xFF)) | ((rw_index + 1) & 0xFF);
    } break;
    default:
      break;
  }
}

bool CommandProcessor::CompareWaitValue(uint32_t function, uint32_t value, uint32_t ref,
                                        uint32_t mask) const {
  value &= mask;
  switch (function & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
  }
}

void CommandProcessor::WaitSleep(uint32_t wait) {
  if (wait >= 0x100) {
    // Most waits are released by the vblank interrupt or by another CPU
    // thread; sleep until the next vblank or a short timeout.
    rex::thread::Wait(vblank_event_.get(), false,
                      std::chrono::milliseconds(std::max<uint32_t>(1, wait / 0x100)));
  } else {
    rex::thread::MaybeYield();
  }
}

void CommandProcessor::LoadShader(xenos::ShaderType type, const uint32_t* guest_be,
                                  uint32_t dword_count) {
  Shader* shader = renderer_->LoadShader(type, guest_be, dword_count);
  if (type == xenos::ShaderType::kVertex) {
    active_vertex_shader_ = shader;
  } else {
    active_pixel_shader_ = shader;
  }
}

CommandProcessor::DebugState CommandProcessor::GetDebugState() const {
  DebugState d;
  d.read_ptr = read_ptr_index_;
  d.write_ptr = write_ptr_index_.load(std::memory_order_relaxed);
  d.ring_size = primary_buffer_size_dwords_;
  d.last_opcode = dbg_last_opcode_.load(std::memory_order_relaxed);
  d.packets = dbg_packets_.load(std::memory_order_relaxed);
  d.waiting_reg_mem = dbg_waiting_.load(std::memory_order_relaxed);
  d.wait_addr = dbg_wait_addr_.load(std::memory_order_relaxed);
  d.wait_ref = dbg_wait_ref_.load(std::memory_order_relaxed);
  d.draws = dbg_draws_.load(std::memory_order_relaxed);
  d.swaps = swap_count_.load(std::memory_order_relaxed);
  return d;
}

bool CommandProcessor::ExecuteType3(Reader& r, uint32_t packet) {
  const uint32_t opcode = (packet >> 8) & 0x7F;
  dbg_last_opcode_.store(opcode, std::memory_order_relaxed);
  dbg_packets_.fetch_add(1, std::memory_order_relaxed);
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
  if (r.remaining() < count) {
    return false;
  }
  const uint32_t data_start = r.read;
  auto finish = [&]() {
    // Always leave the reader at the end of the packet.
    uint32_t consumed = r.ring ? ((r.read - data_start) & (r.capacity - 1)) : (r.read - data_start);
    if (consumed < count) {
      r.Skip(count - consumed);
    }
  };

  // Predicated packets: skip unless the bin test passes. Swaps are never
  // predicated in practice.
  if (packet & 1) {
    if (!(bin_select_ & bin_mask_) || opcode == xenos::PM4_XE_SWAP) {
      r.Skip(count);
      return true;
    }
  }

  using namespace rex::graphics;
  switch (opcode) {
    case xenos::PM4_ME_INIT:
    case xenos::PM4_NOP:
    case xenos::PM4_INVALIDATE_STATE:
    case xenos::PM4_CONTEXT_UPDATE:
    case xenos::PM4_WAIT_FOR_IDLE:
    case xenos::PM4_SET_SHADER_BASES:
      break;

    case xenos::PM4_INTERRUPT: {
      uint32_t cpu_mask = r.Read();
      for (uint32_t n = 0; n < 6; ++n) {
        if (cpu_mask & (uint32_t(1) << n)) {
          system_->DispatchInterruptCallback(1, n);
        }
      }
    } break;

    case xenos::PM4_XE_SWAP: {
      r.Read();  // 'SWAP'
      uint32_t frontbuffer_ptr = r.Read();
      uint32_t frontbuffer_width = r.Read();
      uint32_t frontbuffer_height = r.Read();
      finish();
      renderer_->Swap(regs_, frontbuffer_ptr, frontbuffer_width, frontbuffer_height, gamma_ramp_);
      gamma_ramp_.table_dirty = false;
      gamma_ramp_.pwl_dirty = false;
      swap_count_.fetch_add(1, std::memory_order_release);
      return true;
    }

    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      uint32_t list_ptr = r.Read() & 0x1FFFFFFF;
      uint32_t list_length = r.Read() & 0xFFFFF;
      finish();
      ExecuteIndirectBuffer(list_ptr, list_length);
      return true;
    }

    case xenos::PM4_WAIT_REG_MEM: {
      uint32_t wait_info = r.Read();
      uint32_t poll_addr = r.Read();
      uint32_t ref = r.Read();
      uint32_t mask = r.Read();
      uint32_t wait = r.Read();
      finish();
      bool is_memory = (wait_info & 0x10) != 0;
      dbg_wait_addr_.store(poll_addr, std::memory_order_relaxed);
      dbg_wait_ref_.store(ref, std::memory_order_relaxed);
      dbg_waiting_.store(1, std::memory_order_relaxed);
      struct ClearWaiting {
        std::atomic<uint32_t>& w;
        ~ClearWaiting() { w.store(0, std::memory_order_relaxed); }
      } clear_waiting{dbg_waiting_};
      while (true) {
        uint32_t value;
        if (is_memory) {
          value = *memory_->TranslatePhysical<volatile uint32_t*>(poll_addr & ~uint32_t(3));
          value = GpuSwap32(value, xenos::Endian(poll_addr & 3));
        } else {
          if (poll_addr == XE_GPU_REG_COHER_STATUS_HOST) {
            // Memory is always coherent here: nothing is cached on the host.
            regs_.values[XE_GPU_REG_COHER_STATUS_HOST] = 0;
          }
          value = ReadRegisterValue(poll_addr);
        }
        if (CompareWaitValue(wait_info, value, ref, mask)) {
          break;
        }
        if (!running_) {
          return false;
        }
        // The guest may be waiting for work we have queued but not submitted.
        renderer_->FlushForWait();
        auto wait_start = std::chrono::steady_clock::now();
        WaitSleep(wait);
        renderer_->NoteCpWait(std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wait_start).count());
      }
      return true;
    }

    case xenos::PM4_REG_RMW: {
      uint32_t rmw_info = r.Read();
      uint32_t and_mask = r.Read();
      uint32_t or_mask = r.Read();
      uint32_t value = regs_.values[rmw_info & 0x1FFF];
      value &= (rmw_info >> 31) & 1 ? regs_.values[and_mask & 0x1FFF] : and_mask;
      value |= (rmw_info >> 30) & 1 ? regs_.values[or_mask & 0x1FFF] : or_mask;
      WriteRegister(rmw_info & 0x1FFF, value);
    } break;

    case xenos::PM4_REG_TO_MEM: {
      uint32_t reg_addr = r.Read();
      uint32_t mem_addr = r.Read();
      uint32_t value = GpuSwap32(ReadRegisterValue(reg_addr), xenos::Endian(mem_addr & 3));
      *memory_->TranslatePhysical<uint32_t*>(mem_addr & ~uint32_t(3)) = value;
    } break;

    case xenos::PM4_MEM_WRITE: {
      uint32_t write_addr = r.Read();
      for (uint32_t i = 0; i + 1 < count; ++i) {
        uint32_t data = GpuSwap32(r.Read(), xenos::Endian(write_addr & 3));
        *memory_->TranslatePhysical<uint32_t*>(write_addr & ~uint32_t(3)) = data;
        write_addr += 4;
      }
    } break;

    case xenos::PM4_COND_WRITE: {
      uint32_t wait_info = r.Read();
      uint32_t poll_addr = r.Read();
      uint32_t ref = r.Read();
      uint32_t mask = r.Read();
      uint32_t write_addr = r.Read();
      uint32_t write_data = r.Read();
      uint32_t value;
      if (wait_info & 0x10) {
        value = *memory_->TranslatePhysical<volatile uint32_t*>(poll_addr & ~uint32_t(3));
        value = GpuSwap32(value, xenos::Endian(poll_addr & 3));
      } else {
        value = ReadRegisterValue(poll_addr);
      }
      if (CompareWaitValue(wait_info, value, ref, mask)) {
        if (wait_info & 0x100) {
          *memory_->TranslatePhysical<uint32_t*>(write_addr & ~uint32_t(3)) =
              GpuSwap32(write_data, xenos::Endian(write_addr & 3));
        } else {
          WriteRegister(write_addr, write_data);
        }
      }
    } break;

    case xenos::PM4_EVENT_WRITE: {
      uint32_t initiator = r.Read();
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
    } break;

    case xenos::PM4_EVENT_WRITE_SHD: {
      uint32_t initiator = r.Read();
      uint32_t address = r.Read();
      uint32_t value = r.Read();
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      uint32_t data = (initiator >> 31) & 1 ? counter_.load(std::memory_order_relaxed) : value;
      *memory_->TranslatePhysical<uint32_t*>(address & ~uint32_t(3)) =
          GpuSwap32(data, xenos::Endian(address & 3));
    } break;

    case xenos::PM4_EVENT_WRITE_EXT: {
      uint32_t initiator = r.Read();
      uint32_t address = r.Read();
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      // Screen extents of the previous draws: report the whole screen.
      const uint16_t extents[] = {0, uint16_t(xenos::kTexture2DCubeMaxWidthHeight >> 3), 0,
                                  uint16_t(xenos::kTexture2DCubeMaxWidthHeight >> 3), 0, 1};
      uint16_t* dst = memory_->TranslatePhysical<uint16_t*>(address & ~uint32_t(3));
      for (size_t i = 0; i < 6; ++i) {
        dst[i] = ByteSwap16(extents[i]);
      }
    } break;

    case xenos::PM4_EVENT_WRITE_ZPD: {
      uint32_t initiator = r.Read();
      WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
      // Occlusion queries: report a fixed number of passed samples.
      uint32_t addr = regs_.values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
      if (addr) {
        uint32_t* counts = memory_->TranslatePhysical<uint32_t*>(addr);
        const uint32_t kQueryFinished = ByteSwap32(0xFFFFFEEDu);
        // Layout: Total_A, Total_B, ZFail_A, ZFail_B, ZPass_A, ZPass_B, StencilFail_A/B.
        bool ended = counts[4] == kQueryFinished || counts[5] == kQueryFinished ||
                     counts[2] == kQueryFinished || counts[3] == kQueryFinished;
        std::memset(counts, 0, sizeof(uint32_t) * 8);
        if (ended) {
          counts[4] = 1000;
          counts[0] = 1000;
        }
      }
    } break;

    case xenos::PM4_DRAW_INDX: {
      r.Read();  // Viz query token.
      bool ok = ExecuteDraw(r, count - 1);
      finish();
      return ok;
    }
    case xenos::PM4_DRAW_INDX_2: {
      bool ok = ExecuteDraw(r, count);
      finish();
      return ok;
    }

    case xenos::PM4_SET_CONSTANT: {
      uint32_t offset_type = r.Read();
      uint32_t index = offset_type & 0x7FF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      static constexpr uint32_t kBases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (type < 5) {
        WriteRegistersFromReader(r, kBases[type] + index, count - 1);
      }
    } break;

    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS: {
      uint32_t offset_type = r.Read();
      WriteRegistersFromReader(r, offset_type & 0xFFFF, count - 1);
    } break;

    case xenos::PM4_LOAD_ALU_CONSTANT: {
      uint32_t address = r.Read() & 0x3FFFFFFF;
      uint32_t offset_type = r.Read();
      uint32_t size_dwords = r.Read() & 0xFFF;
      uint32_t index = offset_type & 0x7FF;
      uint32_t type = (offset_type >> 16) & 0xFF;
      static constexpr uint32_t kBases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (type < 5) {
        WriteRegistersFromGuest(kBases[type] + index,
                                memory_->TranslatePhysical<const uint32_t*>(address), size_dwords);
      }
    } break;

    case xenos::PM4_IM_LOAD: {
      uint32_t addr_type = r.Read();
      uint32_t start_size = r.Read();
      uint32_t addr = addr_type & ~uint32_t(3);
      LoadShader(xenos::ShaderType(addr_type & 3),
                 memory_->TranslatePhysical<const uint32_t*>(addr), start_size & 0xFFFF);
    } break;

    case xenos::PM4_IM_LOAD_IMMEDIATE: {
      uint32_t type = r.Read();
      uint32_t start_size = r.Read();
      uint32_t size_dwords = std::min(start_size & 0xFFFF, count - 2);
      if (const uint32_t* contiguous = r.Contiguous(size_dwords)) {
        LoadShader(xenos::ShaderType(type & 1), contiguous, size_dwords);
      } else {
        std::vector<uint32_t> copy(size_dwords);
        for (uint32_t i = 0; i < size_dwords; ++i) {
          copy[i] = r.base[(r.read + i) & (r.capacity - 1)];
        }
        LoadShader(xenos::ShaderType(type & 1), copy.data(), size_dwords);
      }
      r.Skip(size_dwords);
    } break;

    case xenos::PM4_VIZ_QUERY: {
      uint32_t dword0 = r.Read();
      uint32_t id = dword0 & 0x3F;
      if (dword0 & 0x100) {
        // Report every query as visible.
        if (id < 32) {
          regs_.values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_0] |= uint32_t(1) << id;
        } else {
          regs_.values[XE_GPU_REG_PA_SC_VIZ_QUERY_STATUS_1] |= uint32_t(1) << (id - 32);
        }
      }
    } break;

    case xenos::PM4_SET_BIN_MASK_LO:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | r.Read();
      break;
    case xenos::PM4_SET_BIN_MASK_HI:
      bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
      break;
    case xenos::PM4_SET_BIN_SELECT_LO:
      bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | r.Read();
      break;
    case xenos::PM4_SET_BIN_SELECT_HI:
      bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
      break;
    case xenos::PM4_SET_BIN_MASK: {
      uint64_t hi = r.Read();
      uint64_t lo = r.Read();
      bin_mask_ = (hi << 32) | lo;
    } break;
    case xenos::PM4_SET_BIN_SELECT: {
      uint64_t hi = r.Read();
      uint64_t lo = r.Read();
      bin_select_ = (hi << 32) | lo;
    } break;

    default: {
      static uint32_t logged = 0;
      if (logged < 32) {
        ++logged;
        REXGPU_WARN("[carbon-gpu] unhandled PM4 type-3 opcode {:02X} ({} dwords)", opcode, count);
      }
    } break;
  }
  finish();
  return true;
}

bool CommandProcessor::ExecuteDraw(Reader& r, uint32_t count_remaining) {
  using namespace rex::graphics;
  if (!count_remaining) {
    return false;
  }
  reg::VGT_DRAW_INITIATOR initiator;
  initiator.value = r.Read();
  --count_remaining;
  WriteRegister(XE_GPU_REG_VGT_DRAW_INITIATOR, initiator.value);

  DrawInfo info;
  info.primitive_type = initiator.prim_type;
  info.index_count = initiator.num_indices;
  switch (initiator.source_select) {
    case xenos::SourceSelect::kDMA: {
      if (count_remaining < 2) {
        return false;
      }
      uint32_t dma_base = r.Read();
      reg::VGT_DMA_SIZE dma_size;
      dma_size.value = r.Read();
      count_remaining -= 2;
      WriteRegister(XE_GPU_REG_VGT_DMA_BASE, dma_base);
      WriteRegister(XE_GPU_REG_VGT_DMA_SIZE, dma_size.value);
      uint32_t index_size = initiator.index_size == xenos::IndexFormat::kInt16 ? 2 : 4;
      info.indexed = true;
      info.index_base = dma_base & ~(index_size - 1);
      info.index_format = initiator.index_size;
      info.index_endian = dma_size.swap_mode;
      info.index_buffer_size_bytes = dma_size.num_words * index_size;
    } break;
    case xenos::SourceSelect::kAutoIndex:
      break;
    default: {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXGPU_WARN("[carbon-gpu] draw with immediate indices is not supported");
      }
      return true;
    }
  }

  dbg_draws_.fetch_add(1, std::memory_order_relaxed);
  auto viz = regs_.Get<reg::PA_SC_VIZ_QUERY>();
  if (viz.viz_query_ena && viz.kill_pix_post_hi_z) {
    return true;
  }
  auto mode = regs_.Get<reg::RB_MODECONTROL>();
  if (mode.edram_mode == xenos::EdramMode::kCopy) {
    renderer_->Resolve(regs_);
  } else {
    renderer_->Draw(regs_, active_vertex_shader_, active_pixel_shader_, info);
  }
  return true;
}

}  // namespace carbon::gpu
