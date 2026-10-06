// Carbon native renderer: graphics system (presentation, MMIO, interrupts,
// vblank). The MMIO register values and the vblank/interrupt contract match
// what the game's Direct3D expects from the Xenos (see the SDK's
// graphics_system.cpp for the emulated equivalent).

#include "gpu_system.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/windowed_app_context.h>

#include "command_processor.h"
#include "renderer.h"

REXCVAR_DEFINE_BOOL(carbon_gpu_vsync, true, "CarbonGPU",
                    "Pace guest vblank interrupts at the video mode refresh rate. false = 1 kHz "
                    "(uncapped, for benchmarking)");

REXCVAR_DEFINE_BOOL(carbon_gpu_watchdog, true, "CarbonGPU",
                    "Log the command processor state every 3 seconds (debugging)");

namespace carbon::gpu {

using rex::X_STATUS;

GpuSystem::GpuSystem() = default;

GpuSystem::~GpuSystem() = default;

rex::ui::GraphicsProvider* GpuSystem::provider() const { return provider_.get(); }

rex::X_STATUS GpuSystem::SetupPresentation(rex::ui::WindowedAppContext* app_context) {
  if (presenter_) {
    return X_STATUS_SUCCESS;
  }
  if (!provider_) {
    // with_gpu_emulation = true enables the optional device features the
    // renderer uses (independent blend, depth clamp, anisotropy, dynamic
    // rendering, ...) when the device has them.
    provider_ = rex::ui::vulkan::VulkanProvider::Create(true, true);
    if (!provider_) {
      REXGPU_ERROR("[carbon-gpu] unable to create the Vulkan provider");
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  app_context_ = app_context;
  auto loss = [](bool, bool) { rex::FatalError("Graphics device lost"); };
  if (app_context_) {
    app_context_->CallInUIThreadSynchronous(
        [this, loss]() { presenter_ = provider_->CreatePresenter(loss); });
  } else {
    presenter_ = provider_->CreatePresenter(loss);
  }
  if (!presenter_) {
    REXGPU_ERROR("[carbon-gpu] unable to create the presenter");
    return X_STATUS_UNSUCCESSFUL;
  }
  return X_STATUS_SUCCESS;
}

rex::X_STATUS GpuSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                  rex::system::KernelState* kernel_state) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;
  if (!provider_) {
    provider_ = rex::ui::vulkan::VulkanProvider::Create(true, false);
    if (!provider_) {
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  renderer_ = std::make_unique<Renderer>();
  if (!renderer_->Initialize(this, provider_.get(), presenter_.get(), memory_)) {
    REXGPU_ERROR("[carbon-gpu] renderer initialization failed");
    return X_STATUS_UNSUCCESSFUL;
  }

  command_processor_ = std::make_unique<CommandProcessor>(this, renderer_.get());

  // GPU registers: 0x7FC80000-0x7FCFFFFF.
  memory_->AddVirtualMappedRange(
      0x7FC80000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<rex::runtime::MMIOReadCallback>(ReadRegisterThunk),
      reinterpret_cast<rex::runtime::MMIOWriteCallback>(WriteRegisterThunk));

  command_processor_->Start();

  vsync_running_ = true;
  vsync_thread_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        VsyncThreadMain();
        return 0;
      }));
  vsync_thread_->set_name("GPU VSync");
  vsync_thread_->Create();

  REXGPU_INFO("[carbon-gpu] native renderer active (no Xenos emulation)");
  return X_STATUS_SUCCESS;
}

void GpuSystem::VsyncThreadMain() {
  REXGPU_INFO("[carbon-gpu] vsync thread started");
  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  double refresh_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
  if (refresh_hz < 20.0) {
    refresh_hz = 60.0;
  }
  REXGPU_INFO("[carbon-gpu] vsync at {:.2f} Hz", refresh_hz);
  using clock = std::chrono::steady_clock;
  auto next = clock::now();
  auto next_watchdog = next + std::chrono::seconds(3);
  while (vsync_running_) {
    if (REXCVAR_GET(carbon_gpu_watchdog) && command_processor_ && clock::now() >= next_watchdog) {
      next_watchdog = clock::now() + std::chrono::seconds(3);
      auto d = command_processor_->GetDebugState();
      // Warning level: the log file is only flushed on warnings.
      REXGPU_WARN(
          "[carbon-gpu] watchdog: rptr {:X} wptr {:X} ring {:X} packets {} last op {:02X} "
          "wait_reg_mem {} ({:08X} ref {:X}) draws {} swaps {} irq_cb {:08X}",
          d.read_ptr, d.write_ptr, d.ring_size, d.packets, d.last_opcode, d.waiting_reg_mem,
          d.wait_addr, d.wait_ref, d.draws, d.swaps, interrupt_callback_);
    }
    const auto interval = REXCVAR_GET(carbon_gpu_vsync)
                              ? std::chrono::nanoseconds(int64_t(1e9 / refresh_hz))
                              : std::chrono::nanoseconds(1000000);
    next += interval;
    auto now = clock::now();
    static int debug_iterations = 0;
    if (debug_iterations < 4) {
      ++debug_iterations;
      REXGPU_INFO("[carbon-gpu] vsync tick: interval {} ns, sleep {} ns", interval.count(),
                  std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count());
    }
    if (next > now) {
      std::this_thread::sleep_for(next - now);
    } else if (now - next > interval * 4) {
      // Fell far behind (debugger, hitch): resynchronize instead of bursting.
      next = now;
    }
    if (!vsync_running_) {
      break;
    }
    if (command_processor_) {
      command_processor_->IncrementCounter();
    }
    DispatchInterruptCallback(0, 2);
    if (command_processor_) {
      command_processor_->NotifyVblank();
    }
  }
}

void GpuSystem::Shutdown() {
  if (vsync_thread_) {
    vsync_running_ = false;
    vsync_thread_->Wait(0, 0, 0, nullptr);
    vsync_thread_.reset();
  }
  if (command_processor_) {
    command_processor_->Stop();
    command_processor_.reset();
  }
  if (renderer_) {
    renderer_->Shutdown();
    renderer_.reset();
  }
  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    presenter_.reset();
  }
  provider_.reset();
}

void GpuSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("[carbon-gpu] SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GpuSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GpuSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GpuSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                        uint32_t title_id, bool blocking) {
  (void)blocking;
  if (renderer_) {
    renderer_->InitializeShaderStorage(cache_root, title_id);
  }
}

void GpuSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread) {
    return;
  }
  if (cpu == 0xFFFFFFFFu) {
    cpu = 2;
  }
  thread->SetActiveCpu(uint8_t(cpu));
  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args, 2);
}

uint32_t GpuSystem::ReadRegisterThunk(void*, GpuSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GpuSystem::WriteRegisterThunk(void*, GpuSystem* gs, uint32_t addr, uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GpuSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:  // Interrupt status: vblank.
      return 1;
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      rex::system::X_VIDEO_MODE video_mode;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t w = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t h = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (w << 16) | h;
    }
    default:
      break;
  }
  if (!command_processor_ || r >= kRegisterCount) {
    return 0;
  }
  return command_processor_->registers().values[r];
}

void GpuSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  if (r == 0x01C5) {  // CP_RB_WPTR
    command_processor_->UpdateWritePointer(value);
  }
  if (command_processor_ && r < kRegisterCount) {
    command_processor_->registers().values[r] = value;
  }
}

}  // namespace carbon::gpu
