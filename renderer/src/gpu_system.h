// Carbon native renderer: the IGraphicsSystem the SDK loads as a GPU plugin.
#pragma once

#include <atomic>
#include <filesystem>
#include <memory>

#include <rex/system/interfaces/graphics.h>
#include <rex/system/xthread.h>

#include "gpu_common.h"

namespace rex::memory {
class Memory;
}
namespace rex::ui::vulkan {
class VulkanProvider;
}

namespace carbon::gpu {

class CommandProcessor;
class Renderer;

class GpuSystem final : public rex::system::IGraphicsSystem {
 public:
  GpuSystem();
  ~GpuSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                         rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override { return presenter_ != nullptr; }
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override { return presenter_.get(); }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id,
                               bool blocking) override;
  void Shutdown() override;

  rex::memory::Memory* memory() const { return memory_; }
  rex::system::KernelState* kernel_state() const { return kernel_state_; }
  rex::ui::vulkan::VulkanProvider* vulkan_provider() const { return provider_.get(); }
  bool NeedsHostFrameCap(int32_t cap) const;

  // Runs the guest interrupt callback on the calling XThread.
  void DispatchInterruptCallback(uint32_t source, uint32_t cpu);

 private:
  static uint32_t ReadRegisterThunk(void* ppc_context, GpuSystem* gs, uint32_t addr);
  static void WriteRegisterThunk(void* ppc_context, GpuSystem* gs, uint32_t addr,
                                 uint32_t value);
  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);
  void VsyncThreadMain();

  rex::ui::WindowedAppContext* app_context_ = nullptr;
  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  rex::memory::Memory* memory_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;

  uint32_t interrupt_callback_ = 0;
  uint32_t interrupt_callback_data_ = 0;

  std::unique_ptr<Renderer> renderer_;
  std::unique_ptr<CommandProcessor> command_processor_;

  std::atomic<bool> vsync_running_{false};
  double guest_refresh_hz_ = 60.0;  // Initialized before starting guest GPU threads.
  rex::system::object_ref<rex::system::XHostThread> vsync_thread_;
};

}  // namespace carbon::gpu
