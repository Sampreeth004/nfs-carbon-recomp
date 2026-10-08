// Carbon native renderer: Vulkan backend.
//
// One command processor thread records everything. Each guest frame (ended
// by the guest's swap) is a "frame slot" with its own command buffers, fence
// and upload chunks; up to kFramesInFlight slots are on the GPU at once.
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <thread>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
  // Unique stamp (from Renderer::rt_write_counter_) of the last change to the
  // contents: a recorded draw, a clear, or creation. Stamps are never reused, so
  // a recreated target at the same address cannot match an older resolve.
  uint64_t write_gen = 0;
  // Host image.
  VkImage image = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;           // attachment view
  VkImageView sample_view = VK_NULL_HANDLE;    // color: raw (UNORM) view; depth: depth aspect
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;    // host image
  uint32_t lw = 0, lh = 0;           // guest (logical) size
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  uint64_t last_used_frame = 0;
  uint64_t memory_bytes = 0;
  // Rows the game actually resolves from this target. Passes only cover these,
  // so oversized targets do not cost a full load and store per pass.
  uint32_t used_rows = 0;
  // Written as an attachment by a rendering pass since the last dependency that
  // covers it. A later pass using it in the same layout needs an attachment ->
  // attachment dependency (no layout transition would provide one).
  bool attachment_written = false;
};

// What a resolve copied into a region of a resolved texture. A later resolve with
// an identical signature copies exactly the same texels, so it can be skipped.
struct ResolveSig {
  const RenderTarget* src = nullptr;
  uint64_t src_gen = 0;
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // destination rectangle, host pixels
  int32_t dy = 0;
  int32_t exp_bias = 0;
  uint32_t swap_rb = 0;
  uint32_t depth = 0;
  bool sampled = false;  // Read by a draw or the present since this resolve (diagnostic).
  bool operator==(const ResolveSig& o) const {
    return src == o.src && src_gen == o.src_gen && x0 == o.x0 && y0 == o.y0 && x1 == o.x1 &&
           y1 == o.y1 && dy == o.dy && exp_bias == o.exp_bias && swap_rb == o.swap_rb &&
           depth == o.depth;
  }
  bool Overlaps(const ResolveSig& o) const {
    return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1;
  }
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
  // Regions of a resolved texture whose current contents are known (most recent
  // last). Any resolve into an overlapping region replaces the entry.
  std::vector<ResolveSig> resolve_sigs;
  bool has_unsampled = false;
  float res_scale = 1.0f;           // host pixels per guest pixel (render scale)
  bool resolved_swap_rb = false;    // Last resolve swapped red/blue (copy_dest_swap).
  uint64_t resolved_memory_hash = 0;  // Guest memory hash when last resolved.
  uint64_t last_used_frame = 0;
  uint64_t memory_bytes = 0;
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
  void OnFloatConstantsWritten(uint32_t index, uint32_t count, uint32_t changed) {
    // A batch belongs to one stage, 1024 dwords per stage, starting at c0.x.
    stats_.constant_words += count;
    stats_.constant_words_changed += changed;
    if (changed) float_constants_dirty_[index >> 10] = true;
  }
  void Draw(const RegisterFile& regs, Shader* vs, Shader* ps, const DrawInfo& info);
  void Resolve(const RegisterFile& regs);
  void Swap(const RegisterFile& regs, uint32_t frontbuffer_ptr, uint32_t width, uint32_t height,
            const GammaRamp& gamma);
  void FlushForWait() {}
  // Command processor time spent idle (ring empty) or in WAIT_REG_MEM, for the
  // per-frame spike report.
  void NoteCpIdle(double seconds) { frame_idle_s_ += seconds; }
  void NoteCpWait(double seconds) { frame_wait_s_ += seconds; }
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
  // `clear`: clear the pass's single attachment (color 0 or depth/stencil) over
  // the whole render area with the load operation instead of loading it.
  // `clear_area`: render area to use instead of the whole pass (the rectangle).
  bool BeginRendering(const PassState& pass, const VkClearValue* clear = nullptr,
                      const VkRect2D* clear_area = nullptr);
  bool rendering_ = false;
  PassState current_pass_;

  // ---- Barriers ----
  // Layout transition whose stages and access masks follow from the old and new
  // layouts (carbon_gpu_precise_barriers), or ALL_COMMANDS/MEMORY otherwise.
  void TransitionImage(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                       VkImageLayout& layout, VkImageLayout new_layout, uint32_t levels = VK_REMAINING_MIP_LEVELS,
                       uint32_t layers = VK_REMAINING_ARRAY_LAYERS);
  // Always ALL_COMMANDS/MEMORY: for images also used outside this renderer.
  void TransitionImageConservative(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                                   VkImageLayout& layout, VkImageLayout new_layout);
  bool precise_barriers_ = true;

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
  // `y_offset` receives the first row of the resolve in the returned texture
  // (non-zero for tiles resolved into a texture that starts above them).
  Texture* GetOrCreateResolveTexture(uint32_t base_address, uint32_t width, uint32_t height,
                                     xenos::TextureFormat format, uint32_t& y_offset);
  bool CreateTextureImage(Texture& t, VkImageUsageFlags extra_usage);
  void UploadTexture(Texture& t);
  VkImageView GetTextureView(Texture& t, uint32_t swizzle, bool use_signed,
                             VkImageViewType view_type);
  VkSampler GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch, const TextureBinding& binding,
                       bool render_source);
  void BindTextures(const RegisterFile& regs, Shader* shader, uint32_t set_index,
                    DrawConstants& constants, VkDescriptorSet& set_out);
  void CreateNullTextures();
  std::unordered_multimap<uint32_t, std::unique_ptr<Texture>> textures_;  // by base address
  std::vector<Texture*> resolved_textures_;
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
  VkPipeline BuildPipeline(const PipelineKey& key, VkPipelineLayout layout);
  void PipelineWorker();
  void SavePipelineCacheIfDue(double min_interval);

  // Pipeline warm-up: the pipelines used in earlier sessions are recorded (with
  // shader modules named by their SPIR-V cache key) and rebuilt on the workers
  // at start, so the first time a scene appears costs nothing.
  VkShaderModule ModuleForKey(uint64_t key);
  void RecordWarmPipeline(const PipelineKey& key);
  void RunPipelineWarmup();
  void SaveWarmList();
  std::mutex module_mutex_;
  std::unordered_map<uint64_t, VkShaderModule> modules_by_key_;
  std::unordered_map<VkShaderModule, uint64_t> module_keys_;
  std::mutex warm_mutex_;
  std::vector<uint8_t> warm_records_;
  std::unordered_set<uint64_t> warm_seen_;
  bool warm_dirty_ = false;
  std::atomic<bool> warmup_pending_{false};
  std::filesystem::path warm_path_;
  struct PipelineJob {
    std::shared_ptr<PipelineKey> key;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint64_t hash = 0;
  };
  std::mutex pipeline_mutex_;
  std::condition_variable pipeline_cv_;
  std::deque<PipelineJob> pipeline_jobs_;
  std::vector<std::thread> pipeline_workers_;
  bool pipeline_stop_ = false;
  // The driver's pipeline cache is written shortly after new pipelines appear
  // (sessions usually end by the app being killed, never by a clean shutdown).
  std::mutex pipeline_cache_save_mutex_;
  std::atomic<bool> pipeline_cache_dirty_{false};
  double pipeline_cache_saved_at_ = 0.0;
  void SavePipelineCache();

  // ---- Draw helpers ----
  struct VertexRange {
    bool known = false;
    uint32_t min_index = 0, max_index = 0;  // vertex indices, after the index offset
  };
  bool SetupVertexData(const RegisterFile& regs, Shader* vs, DrawConstants& constants,
                       const VertexRange& range);
  // Persistent copy of guest vertex buffers, refreshed by page hash.
  bool CreateVertexArena();
  bool ArenaFetch(uint64_t key, uint32_t address, uint32_t size, uint32_t lo, uint32_t hi,
                  uint32_t& dword_offset);
  struct ArenaEntry {
    uint32_t offset = 0;  // bytes in the arena
    uint32_t size = 0;
    std::vector<uint64_t> page_hash;  // 0 = page not copied yet
    std::vector<uint64_t> page_checked;
    std::vector<uint8_t> page_interval;
  };
  VkBuffer arena_buffer_ = VK_NULL_HANDLE;
  VmaAllocation arena_allocation_ = VK_NULL_HANDLE;
  uint8_t* arena_mapped_ = nullptr;
  VkDeviceSize arena_size_ = 0;
  VkDeviceSize arena_used_ = 0;
  bool arena_reset_pending_ = false;
  std::unordered_map<uint64_t, ArenaEntry> arena_entries_;
  std::unordered_map<uint64_t, uint8_t> arena_changes_;  // >= 3: treated as dynamic

  // Frees textures and render targets that have not been used for a while.
  void EvictResources();
  uint64_t texture_bytes_ = 0;
  uint64_t texture_budget_bytes_ = 0;
  uint64_t render_target_bytes_ = 0;
  void ComputeViewport(const RegisterFile& regs, uint32_t rt_width, uint32_t rt_height,
                       DrawConstants& constants, VkViewport& viewport, VkRect2D& scissor);

  // ---- Presentation ----
  bool CreatePresentPipeline();
  void RecordPresent(VkCommandBuffer cb, VkImageView source, uint32_t source_width,
                     uint32_t source_height, float source_scale, VkImage dest, VkImageView dest_view, uint32_t width,
                     uint32_t height, bool dest_written_before, bool swap_rb);
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout present_pipeline_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
  VkBuffer gamma_buffer_ = VK_NULL_HANDLE;
  VmaAllocation gamma_allocation_ = VK_NULL_HANDLE;
  uint32_t* gamma_mapped_ = nullptr;
  VkSampler linear_sampler_ = VK_NULL_HANDLE;

  // ---- GPU timing (timestamp queries, read back a few frames later) ----
  enum GpuTag : uint8_t { kTagStart, kTagScenePass, kTagOtherPass, kTagResolve, kTagEnd, kTagCount };
  static constexpr uint32_t kMaxStamps = 192;
  struct StampInfo {
    uint32_t width = 0, height = 0, format = 0, draws = 0;
    uint64_t ps_hash = 0, vs_hash = 0;
    uint8_t colors = 0;
    bool depth = false;
  };
  struct PassAgg {
    StampInfo info;
    double seconds = 0;
    uint32_t count = 0;
    uint64_t draws = 0;
  };
  void GpuStamp(GpuTag tag, const StampInfo* info = nullptr);
  StampInfo ts_info_[kFramesInFlight][kMaxStamps];
  std::unordered_map<uint64_t, PassAgg> pass_agg_;
  uint32_t pass_draws_ = 0;
  uint32_t debug_mode_ = 0;
  float max_anisotropy_ = 16.0f;
  // Render resolution scale: host pixels per guest pixel for render targets and
  // resolved images. Everything the game sees stays in guest pixels.
  float res_scale_ = 1.0f;
  uint32_t Scaled(uint32_t v) const {
    return res_scale_ == 1.0f ? v : std::max(1u, uint32_t(std::ceil(float(v) * res_scale_)));
  }

  // State already set on the current command buffer, so repeated draws skip the
  // redundant Vulkan calls. Reset whenever something else may have changed it.
  struct CmdCache {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkViewport viewport = {};
    VkRect2D scissor = {};
    bool viewport_valid = false, scissor_valid = false;
    float depth_bias[2] = {};
    bool depth_bias_valid = false;
    float blend[4] = {};
    bool blend_valid = false;
    uint32_t stencil[6] = {};
    bool stencil_valid = false;
    VkDescriptorSet set1 = VK_NULL_HANDLE, set2 = VK_NULL_HANDLE;
    uint32_t vs_textures = UINT32_MAX, ps_textures = UINT32_MAX;
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  } cmd_;
  // Texture descriptor sets already built this frame, by binding contents.
  std::unordered_map<uint64_t, VkDescriptorSet> texture_set_cache_;
  std::vector<uint32_t> scratch_indices_;
  // Fused index conversion for plain indexed draws: converts into the upload
  // buffer and returns the range of vertex indices used.
  struct IndexRange {
    uint32_t lo = UINT32_MAX, hi = 0;
  };
  bool bloom_enabled_ = true;
  int32_t fps_cap_ = 0;

#if defined(__ANDROID__)
  // ADPF (Android Dynamic Performance Framework): hint session that tells the
  // OS scheduler how long frames take and what the target is, so it can scale
  // clocks down on light frames instead of running flat-out (reduces heat).
  // Loaded at runtime via dlopen so the binary runs on all Android versions;
  // on devices that don't have the API it's a no-op.
  bool adpf_init_tried_ = false;
  int32_t adpf_last_cap_ = -1;
  void* adpf_session_ = nullptr;
  int (*adpf_report_fn_)(void*, int64_t) = nullptr;
  int (*adpf_update_fn_)(void*, int64_t) = nullptr;
  void InitAdpf();
#endif
  // Secondary views on a budget (car reflection cube faces, rear-view mirror):
  // throttled passes skip their draws and the resolve that follows, so the
  // texture keeps the previous update.
  int32_t reflection_faces_ = 6;
  bool mirror_half_rate_ = false;
  uint32_t cube_face_in_frame_ = 0;
  RenderTarget* skip_resolve_rt_ = nullptr;
  double cap_next_ = 0.0;
  uint64_t pass_ps_hash_ = 0, pass_vs_hash_ = 0;
  // Per-frame breakdown, reported for frames that miss 25 ms.
  double frame_idle_s_ = 0, frame_wait_s_ = 0, frame_texture_s_ = 0, frame_draw_s_ = 0;
  double frame_texture_decode_s_ = 0, frame_texture_stage_s_ = 0;
  uint32_t frame_textures_ = 0, frame_pipeline_builds_ = 0, spike_reports_ = 0;
  void ReadGpuStamps(uint32_t slot);
  VkQueryPool ts_pool_[kFramesInFlight] = {};
  uint32_t ts_count_[kFramesInFlight] = {};
  uint8_t ts_tag_[kFramesInFlight][kMaxStamps] = {};
  bool ts_valid_[kFramesInFlight] = {};
  float ts_period_ns_ = 0.0f;
  PFN_vkCmdWriteTimestamp pfn_write_timestamp_ = nullptr;
  PFN_vkGetQueryPoolResults pfn_get_query_results_ = nullptr;

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
    kSkipNoTarget,  // Nothing written: color writes masked off, no depth/stencil.
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
    // Resolves that copied texels, and redundant ones skipped (same source contents,
    // same rectangle and parameters, destination region untouched since).
    uint32_t resolves_copied = 0, resolves_skipped = 0;
    uint32_t resolves_full_overwrite = 0;
    // Resolve clears: by the attachment load operation over the whole target or
    // over a rectangle of it, or (fallback) by vkCmdClearAttachments.
    uint32_t clears_full = 0, clears_partial = 0, clears_command = 0;
    // Pipeline barrier commands: layout transitions, and passes that needed an
    // attachment dependency without a layout change.
    uint32_t barriers = 0, attachment_barriers = 0;
    uint64_t resolve_px_copied = 0, resolve_px_skipped = 0;
    uint64_t resolve_bytes_copied = 0, resolve_bytes_skipped = 0;  // source read + dest write
    // Copied resolves whose result was overwritten before anything sampled it.
    uint32_t resolves_dead = 0;
    uint64_t resolve_px_dead = 0, resolve_bytes_dead = 0;
    uint32_t textures_uploaded = 0, shaders_compiled = 0;
    uint64_t upload_bytes = 0, arena_bytes = 0, arena_draws = 0, chunk_vertex_draws = 0;
    uint64_t constant_words = 0, constant_words_changed = 0;
    uint32_t constant_uploads = 0, constant_reuses = 0;
    uint32_t evicted = 0, throttled = 0;
    double draw_s = 0, resolve_s = 0, swap_s = 0, fence_s = 0, worst_ms = 0;
    double gpu_total_s = 0, gpu_tag_s[5] = {};
    uint32_t gpu_frames = 0, gpu_passes = 0;
    uint32_t slow25 = 0, slow34 = 0, slow50 = 0;
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
  // carbon_gpu_profile: CPU draw/resolve timers, GPU timestamps and breakdowns.
  bool profile_ = false;
  // carbon_gpu_clear_load_op: resolve clears use the attachment load operation.
  bool clear_load_op_ = true;
  uint64_t stats_frames_ = 0;
  double stats_start_ = 0.0;
  // Frame pacing for the Android fps overlay (rex_gpu_report_fps), every 0.5 s.
  double overlay_start_ = 0.0, overlay_last_swap_ = 0.0, overlay_worst_ms_ = 0.0;
  uint32_t overlay_frames_ = 0;
  uint64_t rt_write_counter_ = 0;
  // Per destination (guest address, size, format, source pitch): resolves and how many were
  // overwritten before being sampled, for the stats window.
  struct DestAgg {
    uint32_t base = 0, w = 0, h = 0, fmt = 0, src_pitch = 0, src_fmt = 0;
    uint32_t resolves = 0, dead = 0;
    uint64_t px = 0;
  };
  std::unordered_map<uint64_t, DestAgg> dest_agg_;
  std::unordered_map<std::string, uint32_t> partial_clear_agg_;  // profiling: shapes of partial clears
  bool skip_redundant_resolves_ = true;
  // Frame and GPU times of the current stats window, for percentiles.
  std::vector<float> frame_ms_hist_, gpu_ms_hist_;
};

}  // namespace carbon::gpu
