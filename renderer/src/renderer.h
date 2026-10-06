// Carbon native renderer: Vulkan backend.
//
// One command processor thread records everything. Each guest frame (ended
// by the guest's swap) is a "frame slot" with its own command buffers, fence
// and upload chunks; up to kFramesInFlight slots are on the GPU at once.
#pragma once

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ui/vulkan/api.h>
#include <rex/ui/vulkan/mem_alloc.h>

#include "command_processor.h"
#include "gpu_common.h"
#include "shader_translator.h"
#include "texture_decode.h"

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class Presenter;
}
namespace rex::ui::vulkan {
class VulkanProvider;
class VulkanDevice;
}

namespace carbon::gpu {

class GpuSystem;
class ShaderCompiler;

constexpr uint32_t kFramesInFlight = 3;
// Texture fetch swizzle selecting X, Y, Z, W in order.
constexpr uint32_t kSwizzleRgba = 0 | (1 << 3) | (2 << 6) | (3 << 9);

class Shader {
 public:
  xenos::ShaderType type;
  uint64_t hash = 0;
  std::vector<uint32_t> ucode;  // host byte order
  TranslatedShader translated;
  bool translated_ok = false;

  struct VsModule {
    VertexShaderVariant variant;
    VkShaderModule module = VK_NULL_HANDLE;
  };
  struct PsModule {
    PixelShaderVariant variant;
    VkShaderModule module = VK_NULL_HANDLE;
  };
  std::vector<VsModule> vs_modules;
  std::vector<PsModule> ps_modules;
};

struct RenderTarget {
  // Identity.
  uint32_t edram_base = 0;  // tiles
  uint32_t pitch = 0;       // pixels
  uint32_t guest_format = 0;
  bool is_depth = false;
  // Host image.
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;           // attachment view
  VkImageView sample_view = VK_NULL_HANDLE;    // color: raw (UNORM) view; depth: depth aspect
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint64_t last_used_frame = 0;
};

// A host image that holds guest texture data (decoded from memory, or the
// result of resolves).
struct Texture {
  GuestTexture guest;
  TextureFormatInfo format;
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
  uint32_t width = 1, height = 1, depth = 1, layers = 1, levels = 1;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Views keyed by (swizzle, signed, view type).
  std::unordered_map<uint64_t, VkImageView> views;
  // Guest memory tracking.
  GuestTextureExtent extent;
  uint64_t content_hash = 0;
  uint64_t last_hash_frame = 0;
  uint32_t hash_interval = 1;
  bool resolved = false;            // Contents come from GPU resolves.
  uint64_t resolved_memory_hash = 0;  // Guest memory hash when last resolved.
  uint64_t last_used_frame = 0;
};

class Renderer {
 public:
  Renderer();
  ~Renderer();

  bool Initialize(GpuSystem* system, rex::ui::vulkan::VulkanProvider* provider,
                  rex::ui::Presenter* presenter, rex::memory::Memory* memory);
  void Shutdown();
  void InitializeShaderStorage(const std::filesystem::path& cache_root, uint32_t title_id);

  // Command processor thread.
  void OnCommandThreadStart();
  void OnCommandThreadStop();
  Shader* LoadShader(xenos::ShaderType type, const uint32_t* guest_be, uint32_t dword_count);
  void OnFloatConstantWritten(uint32_t index) {
    // index in dwords from c0.x; 1024 dwords per stage.
    float_constants_dirty_[index >> 10] = true;
  }
  void Draw(const RegisterFile& regs, Shader* vs, Shader* ps, const DrawInfo& info);
  void Resolve(const RegisterFile& regs);
  void Swap(const RegisterFile& regs, uint32_t frontbuffer_ptr, uint32_t width, uint32_t height,
            const GammaRamp& gamma);
  void FlushForWait() {}
  void OnPrimaryBufferEnd() {}

  // Helpers shared by the parts of the renderer.
  const rex::ui::vulkan::VulkanDevice* device() const { return vulkan_device_; }

 private:
  // ---- Frames and submission ----
  struct UploadChunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDescriptorSet set0 = VK_NULL_HANDLE;  // UBOs + vertex data over this chunk
  };
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer upload_cb = VK_NULL_HANDLE;  // texture uploads, before draws
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool submitted = false;
    uint64_t number = 0;
    std::vector<uint32_t> chunks;  // indices into upload_chunks_
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::vector<std::function<void()>> deferred_destroy;
  };
  struct UploadAllocation {
    uint8_t* ptr = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t chunk = 0;
  };

  bool BeginFrame();
  void EndFrame(bool present, const std::function<void(VkCommandBuffer)>& present_fn);
  Frame& frame() { return frames_[frame_index_]; }
  bool AcquireChunk();
  // Ensures `bytes` fit in the current chunk (so one draw stays in one chunk).
  void ReserveUpload(VkDeviceSize bytes);
  UploadAllocation Upload(VkDeviceSize size, VkDeviceSize alignment);
  VkDescriptorSet AllocateDescriptorSet(VkDescriptorSetLayout layout);
  void DeferDestroy(std::function<void()> fn) { frame().deferred_destroy.push_back(std::move(fn)); }

  // ---- Rendering instance (dynamic rendering) ----
  struct PassState {
    RenderTarget* color[4] = {};
    RenderTarget* depth = nullptr;
    uint32_t width = 0, height = 0;
  };
  void EndRendering();
  bool BeginRendering(const PassState& pass);
  bool rendering_ = false;
  PassState current_pass_;

  // ---- Barriers ----
  void TransitionImage(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                       VkImageLayout& layout, VkImageLayout new_layout, uint32_t levels = VK_REMAINING_MIP_LEVELS,
                       uint32_t layers = VK_REMAINING_ARRAY_LAYERS);

  // ---- Render targets (render_targets.cpp) ----
  RenderTarget* GetRenderTarget(bool depth, uint32_t edram_base, uint32_t pitch, uint32_t format,
                                uint32_t min_height);
  VkFormat ColorTargetFormat(xenos::ColorRenderTargetFormat f) const;
  VkFormat DepthTargetFormat(xenos::DepthRenderTargetFormat f) const;
  bool CreateRenderTargetImage(RenderTarget& rt, uint32_t width, uint32_t height);
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> render_targets_;

  // ---- Resolves (render_targets.cpp) ----
  bool CreateResolvePipelines();
  VkPipeline GetResolvePipeline(VkFormat dest_format, bool depth_source);
  std::unordered_map<uint64_t, VkPipeline> resolve_pipelines_;
  VkDescriptorSetLayout resolve_set_layout_ = VK_NULL_HANDLE;
  VkPipelineLayout resolve_pipeline_layout_ = VK_NULL_HANDLE;
  VkShaderModule resolve_vs_ = VK_NULL_HANDLE;
  VkShaderModule resolve_ps_color_ = VK_NULL_HANDLE;
  VkShaderModule resolve_ps_depth_ = VK_NULL_HANDLE;
  VkSampler point_sampler_ = VK_NULL_HANDLE;

  // ---- Textures (textures.cpp) ----
  Texture* GetTexture(const GuestTexture& guest, bool allow_upload);
  Texture* FindResolvedTexture(uint32_t base_address, uint32_t width, uint32_t height);
  Texture* GetOrCreateResolveTexture(uint32_t base_address, uint32_t width, uint32_t height,
                                     xenos::TextureFormat format);
  bool CreateTextureImage(Texture& t, VkImageUsageFlags extra_usage);
  void UploadTexture(Texture& t);
  VkImageView GetTextureView(Texture& t, uint32_t swizzle, bool use_signed,
                             VkImageViewType view_type);
  VkSampler GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch, const TextureBinding& binding);
  void BindTextures(const RegisterFile& regs, Shader* shader, uint32_t set_index,
                    DrawConstants& constants, VkDescriptorSet& set_out);
  void CreateNullTextures();
  std::unordered_multimap<uint32_t, std::unique_ptr<Texture>> textures_;  // by base address
  std::unordered_map<uint64_t, VkSampler> samplers_;
  struct NullTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  NullTexture null_2d_, null_3d_, null_2d_array_, null_cube_;
  bool bc_supported_ = false;

  // ---- Shaders and pipelines ----
  VkShaderModule GetVertexShaderModule(Shader* vs, const VertexShaderVariant& variant);
  VkShaderModule GetPixelShaderModule(Shader* ps, const PixelShaderVariant& variant);
  VkShaderModule CreateModuleFromGlsl(const std::string& glsl, bool vertex, const char* what);
  VkDescriptorSetLayout GetTextureSetLayout(uint32_t count);
  VkPipelineLayout GetPipelineLayout(uint32_t vs_textures, uint32_t ps_textures);
  struct PipelineKey;
  VkPipeline GetPipeline(const PipelineKey& key);
  void SavePipelineCache();

  // ---- Draw helpers ----
  bool SetupVertexData(const RegisterFile& regs, Shader* vs, DrawConstants& constants);
  void ComputeViewport(const RegisterFile& regs, uint32_t rt_width, uint32_t rt_height,
                       DrawConstants& constants, VkViewport& viewport, VkRect2D& scissor);

  // ---- Presentation ----
  bool CreatePresentPipeline();
  void RecordPresent(VkCommandBuffer cb, VkImageView source, uint32_t source_width,
                     uint32_t source_height, VkImage dest, VkImageView dest_view, uint32_t width,
                     uint32_t height, bool dest_written_before);
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout present_pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkBuffer gamma_buffer_ = VK_NULL_HANDLE;
  VmaAllocation gamma_allocation_ = VK_NULL_HANDLE;
  uint32_t* gamma_mapped_ = nullptr;
  VkSampler linear_sampler_ = VK_NULL_HANDLE;

  // ---- State ----
  GpuSystem* system_ = nullptr;
  rex::ui::vulkan::VulkanProvider* provider_ = nullptr;
  const rex::ui::vulkan::VulkanDevice* vulkan_device_ = nullptr;
  rex::ui::Presenter* presenter_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;
  uint8_t* physical_base_ = nullptr;
  VkDevice vk_device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  PFN_vkCmdCopyImage pfn_copy_image_ = nullptr;

  std::array<Frame, kFramesInFlight> frames_;
  uint32_t frame_index_ = 0;
  uint64_t frame_number_ = 1;
  bool frame_open_ = false;

  std::vector<UploadChunk> upload_chunks_;
  std::vector<uint32_t> free_chunks_;
  uint32_t current_chunk_ = UINT32_MAX;
  VkDeviceSize chunk_offset_ = 0;
  VkDeviceSize chunk_size_ = 32u << 20;
  VkDeviceSize ubo_alignment_ = 256;
  VkDeviceSize storage_alignment_ = 256;

  VkDescriptorSetLayout set0_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool set0_pool_ = VK_NULL_HANDLE;
  std::vector<VkDescriptorSetLayout> texture_set_layouts_;  // by count
  std::unordered_map<uint32_t, VkPipelineLayout> pipeline_layouts_;
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  std::filesystem::path pipeline_cache_path_;
  std::unordered_map<uint64_t, VkPipeline> pipelines_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  std::unique_ptr<ShaderCompiler> compiler_;
  VkShaderModule null_ps_module_ = VK_NULL_HANDLE;

  // Per-frame upload dedupe: guest (address, size) -> dword offset in chunk.
  std::unordered_map<uint64_t, uint32_t> vertex_upload_cache_;
  uint32_t vertex_upload_cache_chunk_ = UINT32_MAX;

  // Float constant upload reuse.
  bool float_constants_dirty_[2] = {true, true};
  VkDeviceSize last_constant_offset_[2] = {};
  uint32_t last_constant_chunk_[2] = {UINT32_MAX, UINT32_MAX};
  uint32_t last_constant_count_[2] = {};

  // Statistics (logged periodically).
  enum SkipReason : uint32_t {
    kSkipNoShader,
    kSkipEdramMode,
    kSkipPsFailed,
    kSkipMemexport,
    kSkipPrimitive,
    kSkipNoPitch,
    kSkipNoTarget,
    kSkipModules,
    kSkipCullAll,
    kSkipPipeline,
    kSkipScissor,
    kSkipVertices,
    kSkipNoIndices,
    kSkipReasonCount,
  };
  struct Stats {
    uint32_t draws = 0, draws_skipped = 0, resolves = 0, passes = 0, pipelines_created = 0;
    uint32_t textures_uploaded = 0, shaders_compiled = 0;
    uint64_t upload_bytes = 0;
    uint32_t skip[kSkipReasonCount] = {};
  } stats_;
  void Skip(SkipReason reason) {
    ++stats_.draws_skipped;
    ++stats_.skip[reason];
    if (tracing_) {
      REXGPU_WARN("[carbon-gpu] trace: draw skipped, reason {}", uint32_t(reason));
    }
  }
  // Frame tracing (carbon_gpu_trace_frame): logs every pass, draw and resolve.
  bool tracing_ = false;
  uint64_t stats_frames_ = 0;
  double stats_start_ = 0.0;
};

}  // namespace carbon::gpu
