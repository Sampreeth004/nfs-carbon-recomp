// Carbon native renderer: PM4 ring reader and Xenos register mirror.
//
// Runs on its own guest-visible host thread ("GPU Commands"). It keeps the
// contract the game's Direct3D relies on (read pointer write-back, interrupts,
// scratch/fence writes, WAIT_REG_MEM) and hands draws, resolves and swaps to
// the Renderer, which records them as Vulkan work.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "gpu_common.h"

namespace rex::memory {
class Memory;
}

namespace carbon::gpu {

class GpuSystem;
class Renderer;
class Shader;

struct DrawInfo {
  xenos::PrimitiveType primitive_type = xenos::PrimitiveType::kNone;
  uint32_t index_count = 0;
  bool indexed = false;
  uint32_t index_base = 0;  // Guest physical address.
  uint32_t index_buffer_size_bytes = 0;
  xenos::IndexFormat index_format = xenos::IndexFormat::kInt16;
  xenos::Endian index_endian = xenos::Endian::kNone;
};

struct GammaRamp {
  // 256-entry table, 10 bits per component (blue in 0:9, green 10:19, red 20:29).
  uint32_t table[256] = {};
  // Piecewise linear ramp, [entry][component] = base (16 bits) | delta (16 bits) << 16.
  uint32_t pwl[128][3] = {};
  bool table_dirty = true;
  bool pwl_dirty = true;
};

class CommandProcessor {
 public:
  CommandProcessor(GpuSystem* system, Renderer* renderer);
  ~CommandProcessor();

  bool Start();
  void Stop();

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);
  void UpdateWritePointer(uint32_t value);

  RegisterFile& registers() { return regs_; }
  void IncrementCounter() { counter_.fetch_add(1, std::memory_order_relaxed); }

  // Wakes WAIT_REG_MEM sleepers (called by the vsync thread).
  void NotifyVblank();
  // Swaps seen so far (used by the vsync thread for flip pacing).
  uint32_t swap_count() const { return swap_count_.load(std::memory_order_acquire); }

  // Runs fn on the command processor thread before the next packet batch.
  void CallInThread(std::function<void()> fn);

  // Diagnostics for the watchdog log line.
  struct DebugState {
    uint32_t read_ptr, write_ptr, ring_size, last_opcode, packets, waiting_reg_mem;
    uint32_t wait_addr, wait_ref, draws, swaps;
  };
  DebugState GetDebugState() const;

 private:
  void WorkerMain();
  uint32_t ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index);
  void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count);

  // A cursor over a (possibly wrapping) span of big-endian dwords.
  struct Reader {
    const uint32_t* base;
    uint32_t capacity;  // in dwords, power of two for the ring
    uint32_t read;      // dword index
    uint32_t end;       // dword index (exclusive, may wrap)
    bool ring;

    uint32_t remaining() const {
      return ring ? ((end - read) & (capacity - 1)) : (end - read);
    }
    uint32_t Read() {
      uint32_t v = ByteSwap32(base[read]);
      read = ring ? ((read + 1) & (capacity - 1)) : read + 1;
      return v;
    }
    void Skip(uint32_t count) {
      read = ring ? ((read + count) & (capacity - 1)) : read + count;
    }
    // Pointer to the next dword if `count` dwords are contiguous, else null.
    const uint32_t* Contiguous(uint32_t count) const {
      if (!ring || read + count <= capacity) {
        return base + read;
      }
      return nullptr;
    }
  };

  bool ExecutePacket(Reader& r);
  bool ExecuteType3(Reader& r, uint32_t packet);
  void WriteRegister(uint32_t index, uint32_t value);
  void WriteRegistersFromGuest(uint32_t base_index, const uint32_t* guest_be, uint32_t count);
  void WriteRegistersFromReader(Reader& r, uint32_t base_index, uint32_t count);
  bool ExecuteDraw(Reader& r, uint32_t count_remaining);
  void LoadShader(xenos::ShaderType type, const uint32_t* guest_be, uint32_t dword_count);
  uint32_t ReadRegisterValue(uint32_t index) const;
  bool CompareWaitValue(uint32_t function, uint32_t value, uint32_t ref, uint32_t mask) const;
  void WaitSleep(uint32_t wait);
  void PublishReadPointer(uint32_t read_index);

  GpuSystem* system_;
  Renderer* renderer_;
  rex::memory::Memory* memory_;

  RegisterFile regs_;
  GammaRamp gamma_ramp_;
  uint32_t gamma_rw_component_ = 0;

  uint32_t primary_buffer_ptr_ = 0;
  uint32_t primary_buffer_size_dwords_ = 0;
  uint32_t read_ptr_index_ = 0;
  std::atomic<uint32_t> write_ptr_index_{0xBAADF00Du};
  uint32_t read_ptr_writeback_ptr_ = 0;
  uint32_t read_ptr_update_dwords_ = 0;

  uint64_t bin_mask_ = ~uint64_t(0);
  uint64_t bin_select_ = ~uint64_t(0);
  std::atomic<uint32_t> counter_{0};
  std::atomic<uint32_t> swap_count_{0};

  Shader* active_vertex_shader_ = nullptr;
  Shader* active_pixel_shader_ = nullptr;

  std::atomic<bool> running_{false};
  std::unique_ptr<rex::thread::Event> write_ptr_event_;
  std::unique_ptr<rex::thread::Event> vblank_event_;
  rex::system::object_ref<rex::system::XHostThread> worker_;

  std::mutex pending_mutex_;
  std::vector<std::function<void()>> pending_;
  std::atomic<bool> has_pending_{false};

  std::atomic<uint32_t> dbg_last_opcode_{0};
  std::atomic<uint32_t> dbg_packets_{0};
  std::atomic<uint32_t> dbg_waiting_{0};
  std::atomic<uint32_t> dbg_wait_addr_{0};
  std::atomic<uint32_t> dbg_wait_ref_{0};
  std::atomic<uint32_t> dbg_draws_{0};
};

}  // namespace carbon::gpu
