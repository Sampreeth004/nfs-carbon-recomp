// Carbon native renderer: Vulkan backend core (frames, uploads, pipelines,
// draws, presentation).

#include "renderer.h"

#include <algorithm>
#include <cstddef>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <type_traits>
#if defined(__ANDROID__)
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/system/gpu_plugin.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/system/xmemory.h>
#include <rex/system/xvideo.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "gpu_system.h"
#include "shader_compiler.h"

REXCVAR_DEFINE_INT32(carbon_gpu_upload_chunk_mb, 32, "CarbonGPU",
                     "Size of each per-frame upload buffer chunk (vertices, indices, constants)")
    .range(8, 256);
REXCVAR_DEFINE_BOOL(carbon_gpu_stats, true, "CarbonGPU",
                    "Log renderer statistics (fps, draws, passes, uploads) every 5 seconds");
REXCVAR_DEFINE_BOOL(carbon_gpu_profile, false, "CarbonGPU",
                    "Detailed renderer profiling: CPU draw/resolve timers, GPU timestamp queries, "
                    "per-pass and per-resolve-destination breakdowns and slow-frame reports "
                    "(read at startup)");
REXCVAR_DEFINE_BOOL(carbon_gpu_precise_barriers, true, "CarbonGPU",
                    "Image barriers wait only for the stages and accesses that used the image "
                    "(false = ALL_COMMANDS barriers on layout changes only)");
REXCVAR_DEFINE_BOOL(carbon_gpu_clear_load_op, true, "CarbonGPU",
                    "Clear render targets with the attachment load operation over the cleared "
                    "rectangle (false = load the whole target and clear the rectangle)");
REXCVAR_DEFINE_INT32(carbon_gpu_trace_frame, 0, "CarbonGPU",
                     "Log every pass, draw and resolve of this frame number (0 = off)");
REXCVAR_DEFINE_INT32(carbon_gpu_dump_frame_interval, 0, "CarbonGPU",
                     "Save the presented guest frame as carbon_frames/frame_<n>.bmp every N frames "
                     "(0 = off)");
REXCVAR_DEFINE_INT32(carbon_gpu_vertex_arena_mb, 128, "CarbonGPU",
                     "Size of the persistent vertex buffer cache (0 = upload vertices every frame)")
    .range(0, 1024);
REXCVAR_DEFINE_BOOL(carbon_gpu_skip_redundant_resolves, true, "CarbonGPU",
                    "Skip resolves whose source contents, rectangle and parameters match the "
                    "previous resolve into the same destination region (false = always copy)");
REXCVAR_DEFINE_BOOL(carbon_gpu_vertex_ranges, true, "CarbonGPU",
                    "Copy only the part of a vertex buffer the draw's indices reach");
#if defined(__ANDROID__)
REXCVAR_DEFINE_INT32(carbon_gpu_texture_budget_mb, 640, "CarbonGPU",
                     "Texture memory above which least recently used textures are freed")
    .range(64, 8192);
#else
REXCVAR_DEFINE_INT32(carbon_gpu_texture_budget_mb, 3072, "CarbonGPU",
                     "Texture memory above which least recently used textures are freed")
    .range(64, 16384);
#endif
#if defined(__ANDROID__)
REXCVAR_DEFINE_INT32(carbon_gpu_max_anisotropy, 4, "CarbonGPU",
                     "Cap on anisotropic filtering for guest textures (1 = off)")
    .range(1, 16);
#else
REXCVAR_DEFINE_INT32(carbon_gpu_max_anisotropy, 16, "CarbonGPU",
                     "Cap on anisotropic filtering for guest textures (1 = off)")
    .range(1, 16);
#endif
REXCVAR_DEFINE_INT32(carbon_gpu_fps_cap, 0, "CarbonGPU",
                     "Frame rate cap applied when the guest presents (0 = none)")
    .range(0, 240);
REXCVAR_DEFINE_INT32(carbon_gpu_reflection_faces, 6, "CarbonGPU",
                     "Car reflection cube faces redrawn per frame (6 = all, every frame, 0 = none). "
                     "The faces are not sampled by this renderer yet, so 0 loses nothing visible")
    .range(0, 6);
REXCVAR_DEFINE_BOOL(carbon_gpu_mirror_half_rate, false, "CarbonGPU",
                    "Redraw the rear-view mirror every other frame");
REXCVAR_DEFINE_BOOL(carbon_gpu_bloom, true, "CarbonGPU",
                    "Run the game's bloom blur passes (very heavy on phone GPUs)");
REXCVAR_DEFINE_BOOL(carbon_gpu_async_pipelines, true, "CarbonGPU",
                    "Build new pipelines on worker threads (draws needing them are skipped until ready)");
REXCVAR_DEFINE_INT32(carbon_gpu_render_scale, 100, "CarbonGPU",
                     "Render resolution in percent of the game's (100 = native 1280x720)")
    .range(25, 200);
REXCVAR_DEFINE_BOOL(carbon_gpu_debug_cycle, false, "CarbonGPU",
                    "Debug: every 10 s cycle between normal, skipping small post passes, skipping the main scene, and both");
REXCVAR_DEFINE_BOOL(carbon_gpu_dump_shaders, false, "CarbonGPU",
                    "Write the generated GLSL of every translated shader to <cache>/carbon_gpu/glsl");

#if defined(__ANDROID__)
// librexruntime.so: the app's fps overlay reads these through the JNI bridge.
extern "C" void rex_gpu_report_fps(float fps, float frame_ms, float worst_ms);
#endif

namespace carbon::gpu {
std::atomic<bool> g_paused{false};
}  // namespace carbon::gpu

// Called by the app when it goes to the background: the command thread parks at
// the next frame, so the game, the GPU and the SoC go quiet until it returns.
extern "C" REX_GPU_PLUGIN_EXPORT void carbon_gpu_set_paused(int paused) {
  carbon::gpu::g_paused.store(paused != 0, std::memory_order_release);
}

namespace carbon::gpu {

using rex::ui::vulkan::VulkanDevice;

constexpr uint32_t kWarmMagic = 0x4C575043;  // "CPWL"
constexpr uint32_t kWarmVersion = 1;

uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
  return XXH3_64bits_withSeed(data, size, seed);
}

namespace {

// Writes RGBA8 pixels as a top-down 32-bit BMP.
void WriteBmp(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height, bool swap_rb) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    REXGPU_WARN("[carbon-gpu] could not write {}", path);
    return;
  }
  uint32_t image_size = width * height * 4;
  uint8_t header[54] = {'B', 'M'};
  auto put32 = [&](uint32_t offset, uint32_t v) { std::memcpy(header + offset, &v, 4); };
  put32(2, 54 + image_size);
  put32(10, 54);
  put32(14, 40);
  put32(18, width);
  put32(22, uint32_t(-int32_t(height)));
  header[26] = 1;
  header[28] = 32;
  put32(34, image_size);
  f.write(reinterpret_cast<const char*>(header), sizeof(header));
  std::vector<uint8_t> row(size_t(width) * 4);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 4 + 0] = src[x * 4 + (swap_rb ? 0 : 2)];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + (swap_rb ? 2 : 0)];
      row[x * 4 + 3] = 255;
    }
    f.write(reinterpret_cast<const char*>(row.data()), std::streamsize(row.size()));
  }
  REXGPU_INFO("[carbon-gpu] saved {}", path);
}

// Guest index conversion: byte swap, primitive restart mapping and min/max in one
// vectorizable pass.
template <bool kSwap, bool kRestart>
void ConvertIndices16Impl(const uint16_t* in, uint16_t* out, uint32_t count, uint16_t reset,
                          uint32_t& lo_out, uint32_t& hi_out) {
  uint16_t lo = 0xFFFF, hi = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t v = in[i];
    if (kSwap) v = __builtin_bswap16(v);
    if (kRestart) {
      const bool is_reset = v == reset;
      out[i] = is_reset ? uint16_t(0xFFFF) : v;
      lo = std::min(lo, is_reset ? uint16_t(0xFFFF) : v);
      hi = std::max(hi, is_reset ? uint16_t(0) : v);
    } else {
      out[i] = v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  lo_out = lo;
  hi_out = hi;
}

void ConvertIndices16(const uint8_t* src, uint16_t* out, uint32_t count, bool swap, bool restart,
                      uint32_t reset, uint32_t& lo, uint32_t& hi) {
  const uint16_t* in = reinterpret_cast<const uint16_t*>(src);
  const uint16_t r = uint16_t(reset);
  if (swap) {
    restart ? ConvertIndices16Impl<true, true>(in, out, count, r, lo, hi)
            : ConvertIndices16Impl<true, false>(in, out, count, r, lo, hi);
  } else {
    restart ? ConvertIndices16Impl<false, true>(in, out, count, r, lo, hi)
            : ConvertIndices16Impl<false, false>(in, out, count, r, lo, hi);
  }
}

template <int kMode, bool kRestart>
void ConvertIndices32Impl(const uint32_t* in, uint32_t* out, uint32_t count, uint32_t reset,
                          uint32_t& lo_out, uint32_t& hi_out) {
  uint32_t lo = 0xFFFFFFFFu, hi = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t v = in[i];
    if (kMode == 1) {
      v = ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    } else if (kMode == 2) {
      v = __builtin_bswap32(v);
    } else if (kMode == 3) {
      v = (v >> 16) | (v << 16);
    }
    v &= 0xFFFFFF;
    if (kRestart) {
      const bool is_reset = v == reset;
      out[i] = is_reset ? 0xFFFFFFFFu : v;
      lo = std::min(lo, is_reset ? 0xFFFFFFFFu : v);
      hi = std::max(hi, is_reset ? 0u : v);
    } else {
      out[i] = v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
  }
  lo_out = lo;
  hi_out = hi;
}

void ConvertIndices32(const uint8_t* src, uint32_t* out, uint32_t count, xenos::Endian endian,
                      bool restart, uint32_t reset, uint32_t& lo, uint32_t& hi) {
  const uint32_t* in = reinterpret_cast<const uint32_t*>(src);
#define CARBON_CONV32(mode)                                                            restart ? ConvertIndices32Impl<mode, true>(in, out, count, reset, lo, hi)                    : ConvertIndices32Impl<mode, false>(in, out, count, reset, lo, hi)
  switch (endian) {
    case xenos::Endian::k8in16:
      CARBON_CONV32(1);
      break;
    case xenos::Endian::k8in32:
      CARBON_CONV32(2);
      break;
    case xenos::Endian::k16in32:
      CARBON_CONV32(3);
      break;
    default:
      CARBON_CONV32(0);
      break;
  }
#undef CARBON_CONV32
}

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::filesystem::path g_glsl_dump_dir;

// How the renderer uses an image while it is in a layout.
struct LayoutUse {
  VkPipelineStageFlags stages;
  VkAccessFlags reads;
  VkAccessFlags writes;
};

LayoutUse LayoutUseOf(VkImageLayout layout) {
  switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      // New images only: nothing earlier to wait for.
      return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, 0};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
      return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
      // Guest textures can be fetched by vertex shaders as well as pixel shaders.
      return {VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_SHADER_READ_BIT, 0};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return {VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT};
    default:
      return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
              VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_MEMORY_WRITE_BIT};
  }
}

}  // namespace

struct Renderer::PipelineKey {
  VkShaderModule vs;
  VkShaderModule ps;
  uint32_t vs_textures;
  uint32_t ps_textures;
  uint32_t color_formats[4];
  uint32_t depth_format;
  uint32_t topology;
  uint32_t primitive_restart;
  uint32_t cull_mode;
  uint32_t front_face;
  uint32_t polygon_mode;
  uint32_t depth_bias;
  uint32_t depth_test;
  uint32_t depth_write;
  uint32_t depth_compare;
  uint32_t stencil_test;
  uint32_t stencil_front[4];  // fail, pass, depth fail, compare
  uint32_t stencil_back[4];
  uint32_t blend[4][7];  // enable, src color, dst color, op color, src alpha, dst alpha, op alpha
  uint32_t write_mask[4];
  uint32_t alpha_to_coverage;
};

Renderer::Renderer() = default;

Renderer::~Renderer() = default;

bool Renderer::Initialize(GpuSystem* system, rex::ui::vulkan::VulkanProvider* provider,
                          rex::ui::Presenter* presenter, rex::memory::Memory* memory) {
  system_ = system;
  provider_ = provider;
  presenter_ = presenter;
  memory_ = memory;
  physical_base_ = memory_->TranslatePhysical<uint8_t*>(0);
  vulkan_device_ = provider->vulkan_device();
  vk_device_ = vulkan_device_->device();
  const auto& dfn = vulkan_device_->functions();
  const auto& props = vulkan_device_->properties();

  if (!props.dynamicRendering) {
    REXGPU_ERROR("[carbon-gpu] the Vulkan device lacks dynamic rendering (Vulkan 1.3)");
    return false;
  }

  allocator_ = rex::ui::vulkan::CreateVmaAllocator(vulkan_device_, true);
  if (!allocator_) {
    return false;
  }
  ubo_alignment_ = std::max<VkDeviceSize>(props.minUniformBufferOffsetAlignment, 16);
  storage_alignment_ = std::max<VkDeviceSize>(props.minStorageBufferOffsetAlignment, 16);
  texture_budget_bytes_ = uint64_t(REXCVAR_GET(carbon_gpu_texture_budget_mb)) << 20;
  max_anisotropy_ = float(REXCVAR_GET(carbon_gpu_max_anisotropy));
  res_scale_ = float(REXCVAR_GET(carbon_gpu_render_scale)) / 100.0f;
  bloom_enabled_ = REXCVAR_GET(carbon_gpu_bloom);
  fps_cap_ = REXCVAR_GET(carbon_gpu_fps_cap);
  reflection_faces_ = REXCVAR_GET(carbon_gpu_reflection_faces);
  mirror_half_rate_ = REXCVAR_GET(carbon_gpu_mirror_half_rate);
  profile_ = REXCVAR_GET(carbon_gpu_profile);
  clear_load_op_ = REXCVAR_GET(carbon_gpu_clear_load_op);
  precise_barriers_ = REXCVAR_GET(carbon_gpu_precise_barriers);
  chunk_size_ = VkDeviceSize(REXCVAR_GET(carbon_gpu_upload_chunk_mb)) << 20;
  chunk_size_ = std::min<VkDeviceSize>(chunk_size_, props.maxStorageBufferRange);

  // BC texture support decides whether DXT is uploaded compressed.
  {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    VkFormatProperties fp = {};
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(),
                                            VK_FORMAT_BC3_UNORM_BLOCK, &fp);
    bc_supported_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
  }

  // Set 0: VS constants, PS constants, draw constants, vertex data.
  {
    VkDescriptorSetLayoutBinding b[5] = {};
    for (uint32_t i = 0; i < 3; ++i) {
      b[i].binding = i;
      b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    b[3].binding = 3;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].descriptorCount = 1;
    b[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    b[4] = b[3];
    b[4].binding = 4;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 5;
    ci.pBindings = b;
    if (dfn.vkCreateDescriptorSetLayout(vk_device_, &ci, nullptr, &set0_layout_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * 256},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 256;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(vk_device_, &pci, nullptr, &set0_pool_) != VK_SUCCESS) {
      return false;
    }
  }
  if (!CreateVertexArena()) {
    return false;
  }

  // Frames.
  for (Frame& f : frames_) {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = vulkan_device_->queue_family_graphics_compute();
    if (dfn.vkCreateCommandPool(vk_device_, &pci, nullptr, &f.pool) != VK_SUCCESS) {
      return false;
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = f.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 2;
    VkCommandBuffer cbs[2];
    if (dfn.vkAllocateCommandBuffers(vk_device_, &ai, cbs) != VK_SUCCESS) {
      return false;
    }
    f.upload_cb = cbs[0];
    f.cb = cbs[1];
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (dfn.vkCreateFence(vk_device_, &fci, nullptr, &f.fence) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 65536},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 16384;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(vk_device_, &dpci, nullptr, &f.descriptor_pool) !=
        VK_SUCCESS) {
      return false;
    }
  }

  {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    VkPhysicalDeviceProperties pdp = {};
    ifn.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(), &pdp);
    ts_period_ns_ = pdp.limits.timestampPeriod;
    pfn_write_timestamp_ = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
        ifn.vkGetDeviceProcAddr(vk_device_, "vkCmdWriteTimestamp"));
    pfn_get_query_results_ = reinterpret_cast<PFN_vkGetQueryPoolResults>(
        ifn.vkGetDeviceProcAddr(vk_device_, "vkGetQueryPoolResults"));
    // Timestamp pools exist only while profiling; without them GpuStamp and
    // ReadGpuStamps do nothing.
    if (profile_ && ts_period_ns_ > 0.0f && pfn_write_timestamp_ && pfn_get_query_results_) {
      for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkQueryPoolCreateInfo qci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxStamps;
        if (dfn.vkCreateQueryPool(vk_device_, &qci, nullptr, &ts_pool_[i]) != VK_SUCCESS) {
          ts_pool_[i] = VK_NULL_HANDLE;
        }
      }
    }
  }
  {
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    dfn.vkCreatePipelineCache(vk_device_, &ci, nullptr, &pipeline_cache_);
  }
  if (REXCVAR_GET(carbon_gpu_async_pipelines)) {
    for (int i = 0; i < 2; ++i) {
      pipeline_workers_.emplace_back([this]() { PipelineWorker(); });
    }
  }

  compiler_ = std::make_unique<ShaderCompiler>();
  texture_set_layouts_.assign(65, VK_NULL_HANDLE);

  null_ps_module_ = CreateModuleFromGlsl(BuildNullPixelShaderGlsl(), false, "null PS");
  if (!null_ps_module_) {
    return false;
  }
  CreateNullTextures();
  if (!CreateResolvePipelines() || !CreatePresentPipeline()) {
    return false;
  }

  REXGPU_INFO("[carbon-gpu] Vulkan device '{}', BC textures {}, upload chunk {} MiB",
              props.deviceName, bc_supported_ ? "native" : "decoded on CPU", chunk_size_ >> 20);
  stats_start_ = NowSeconds();
  return true;
}

void Renderer::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                       uint32_t title_id) {
  std::filesystem::path dir = cache_root / "carbon_gpu";
  compiler_->SetCacheDirectory(dir / "spirv");
  if (REXCVAR_GET(carbon_gpu_dump_shaders)) {
    g_glsl_dump_dir = dir / "glsl";
    std::error_code ec;
    std::filesystem::create_directories(g_glsl_dump_dir, ec);
  }
  const auto& props = vulkan_device_->properties();
  char name[96];
  std::snprintf(name, sizeof(name), "%08X.%04X-%04X-%08X.vkpc", title_id, props.vendorID,
                props.deviceID, props.driverVersion);
  pipeline_cache_path_ = dir / name;
  {
    char warm_name[32];
    std::snprintf(warm_name, sizeof(warm_name), "%08X.pipelines", title_id);
    warm_path_ = dir / warm_name;
    std::ifstream warm(warm_path_, std::ios::binary | std::ios::ate);
    if (warm) {
      std::vector<uint8_t> data(size_t(warm.tellg()));
      warm.seekg(0);
      warm.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
      uint32_t header[3] = {};
      if (data.size() >= sizeof(header)) {
        std::memcpy(header, data.data(), sizeof(header));
      }
      if (header[0] == kWarmMagic && header[1] == kWarmVersion &&
          header[2] == sizeof(PipelineKey) &&
          (data.size() - sizeof(header)) % sizeof(PipelineKey) == 0) {
        std::lock_guard<std::mutex> lock(warm_mutex_);
        warm_records_.assign(data.begin() + sizeof(header), data.end());
        for (size_t i = 0; i < warm_records_.size(); i += sizeof(PipelineKey)) {
          warm_seen_.insert(HashBytes(warm_records_.data() + i, sizeof(PipelineKey)));
        }
        warmup_pending_ = true;
      }
    }
  }
  std::ifstream in(pipeline_cache_path_, std::ios::binary | std::ios::ate);
  if (in && pipelines_.empty()) {
    std::vector<char> data(size_t(in.tellg()));
    in.seekg(0);
    in.read(data.data(), std::streamsize(data.size()));
    const auto& dfn = vulkan_device_->functions();
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.data();
    VkPipelineCache cache = VK_NULL_HANDLE;
    if (dfn.vkCreatePipelineCache(vk_device_, &ci, nullptr, &cache) == VK_SUCCESS) {
      dfn.vkDestroyPipelineCache(vk_device_, pipeline_cache_, nullptr);
      pipeline_cache_ = cache;
      REXGPU_INFO("[carbon-gpu] loaded pipeline cache ({} KiB)", data.size() >> 10);
    }
  }
}

#if defined(__ANDROID__)
void Renderer::InitAdpf() {
  // libandroid.so is always loaded; RTLD_NOLOAD avoids a dlopen ref count.
  void* lib = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
  if (!lib) {
    lib = dlopen("libandroid.so", RTLD_NOW);  // fallback: load it ourselves
    if (!lib) return;
  }

  using GetManagerFn    = void* (*)(void);
  using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);

  const auto get_mgr    = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
  const auto create_ses = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHintManager_createSession"));
  adpf_report_fn_       = reinterpret_cast<decltype(adpf_report_fn_)>(
      dlsym(lib, "APerformanceHintSession_reportActualWorkDuration"));
  adpf_update_fn_       = reinterpret_cast<decltype(adpf_update_fn_)>(
      dlsym(lib, "APerformanceHintSession_updateTargetWorkDuration"));

  if (!get_mgr || !create_ses || !adpf_report_fn_ || !adpf_update_fn_) {
    // API < 33, or symbols not present — no-op gracefully.
    adpf_report_fn_ = nullptr;
    adpf_update_fn_ = nullptr;
    return;
  }

  void* mgr = get_mgr();
  if (!mgr) return;

  // Target: fps_cap_ if set, otherwise 60 fps as a reasonable default.
  const int64_t target_ns = fps_cap_ > 0 ? int64_t(1e9 / fps_cap_) : int64_t(16'666'667LL);
  const int32_t tid = static_cast<int32_t>(gettid());
  adpf_session_ = create_ses(mgr, &tid, 1, target_ns);
  adpf_last_cap_ = fps_cap_;

  if (adpf_session_) {
    REXGPU_INFO("[carbon-gpu] ADPF hint session created (target {:.2f} ms, tid {})",
                double(target_ns) / 1e6, tid);
  }
}
#endif

void Renderer::SavePipelineCache() {
  if (pipeline_cache_path_.empty() || !pipeline_cache_) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  size_t size = 0;
  if (dfn.vkGetPipelineCacheData(vk_device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS ||
      !size) {
    return;
  }
  std::vector<char> data(size);
  if (dfn.vkGetPipelineCacheData(vk_device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(pipeline_cache_path_.parent_path(), ec);
  // Written aside and renamed, so a kill mid-write never leaves a broken cache.
  std::filesystem::path temp = pipeline_cache_path_;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(size));
    if (!out) {
      return;
    }
  }
  std::filesystem::rename(temp, pipeline_cache_path_, ec);
  REXGPU_INFO("[carbon-gpu] saved pipeline cache ({} KiB)", size >> 10);
}

void Renderer::Shutdown() {
  if (!vk_device_) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  for (Frame& f : frames_) {
    if (f.submitted) {
      dfn.vkWaitForFences(vk_device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
      f.submitted = false;
    }
    for (auto& fn : f.deferred_destroy) fn();
    f.deferred_destroy.clear();
  }
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_stop_ = true;
  }
  pipeline_cv_.notify_all();
  for (std::thread& worker : pipeline_workers_) {
    worker.join();
  }
  pipeline_workers_.clear();
  SavePipelineCache();
  // The process is exiting; the device and allocator die with the provider.
}

void Renderer::OnCommandThreadStart() {}

void Renderer::OnCommandThreadStop() {
  if (frame_open_) {
    EndFrame(false, nullptr);
  }
}

// ---------------------------------------------------------------------------
// Frames and uploads
// ---------------------------------------------------------------------------

bool Renderer::BeginFrame() {
  if (frame_open_) {
    return true;
  }
  const auto& dfn = vulkan_device_->functions();
  Frame& f = frame();
  if (f.submitted) {
    double t0 = NowSeconds();
    dfn.vkWaitForFences(vk_device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    stats_.fence_s += NowSeconds() - t0;
    dfn.vkResetFences(vk_device_, 1, &f.fence);
    f.submitted = false;
    ReadGpuStamps(frame_index_);
  }
  if (arena_reset_pending_) {
    // The arena is full: wait until no frame in flight reads it, then start over.
    for (Frame& other : frames_) {
      if (&other != &f && other.submitted) {
        dfn.vkWaitForFences(vk_device_, 1, &other.fence, VK_TRUE, UINT64_MAX);
      }
    }
    REXGPU_INFO("[carbon-gpu] vertex arena full ({} MiB, {} buffers): starting over",
                arena_used_ >> 20, arena_entries_.size());
    arena_entries_.clear();
    arena_used_ = 0;
    arena_reset_pending_ = false;
  }
  for (auto& fn : f.deferred_destroy) fn();
  f.deferred_destroy.clear();
  for (uint32_t c : f.chunks) {
    free_chunks_.push_back(c);
  }
  f.chunks.clear();
  dfn.vkResetDescriptorPool(vk_device_, f.descriptor_pool, 0);
  texture_set_cache_.clear();
  cmd_ = CmdCache();
  if (warmup_pending_.exchange(false)) {
    RunPipelineWarmup();
  }
  cube_face_in_frame_ = 0;
  skip_resolve_rt_ = nullptr;
  dfn.vkResetCommandPool(vk_device_, f.pool, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(f.upload_cb, &bi);
  dfn.vkBeginCommandBuffer(f.cb, &bi);
  ts_count_[frame_index_] = 0;
  ts_valid_[frame_index_] = false;
  if (ts_pool_[frame_index_]) {
    dfn.vkCmdResetQueryPool(f.cb, ts_pool_[frame_index_], 0, kMaxStamps);
    GpuStamp(kTagStart);
  }
  f.number = frame_number_;
  frame_open_ = true;
  tracing_ = REXCVAR_GET(carbon_gpu_trace_frame) > 0 &&
             frame_number_ == uint64_t(REXCVAR_GET(carbon_gpu_trace_frame));
  if (tracing_) {
    REXGPU_WARN("[carbon-gpu] ---- trace of frame {} ----", frame_number_);
  }
  if (frame_number_ % 30 == 0) {
    EvictResources();
  }
  current_chunk_ = UINT32_MAX;
  chunk_offset_ = 0;
  vertex_upload_cache_.clear();
  vertex_upload_cache_chunk_ = UINT32_MAX;
  last_constant_chunk_[0] = last_constant_chunk_[1] = UINT32_MAX;
  return true;
}

void Renderer::EndFrame(bool present, const std::function<void(VkCommandBuffer)>& present_fn) {
  if (!frame_open_) {
    BeginFrame();
  }
  EndRendering();
  const auto& dfn = vulkan_device_->functions();
  Frame& f = frame();
  if (present && present_fn) {
    present_fn(f.cb);
  }
  GpuStamp(kTagEnd);
  ts_valid_[frame_index_] = true;
  dfn.vkEndCommandBuffer(f.upload_cb);
  dfn.vkEndCommandBuffer(f.cb);
  VkCommandBuffer cbs[2] = {f.upload_cb, f.cb};
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 2;
  si.pCommandBuffers = cbs;
  {
    auto queue = vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    VkResult r = dfn.vkQueueSubmit(queue.queue(), 1, &si, f.fence);
    if (r != VK_SUCCESS) {
      REXGPU_ERROR("[carbon-gpu] vkQueueSubmit failed: {}", int(r));
    }
  }
  f.submitted = true;
  frame_open_ = false;
  frame_index_ = (frame_index_ + 1) % kFramesInFlight;
  ++frame_number_;
}

bool Renderer::AcquireChunk() {
  uint32_t index;
  if (!free_chunks_.empty()) {
    index = free_chunks_.back();
    free_chunks_.pop_back();
  } else {
    UploadChunk chunk;
    chunk.size = chunk_size_;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = chunk.size;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(allocator_, &bci, &aci, &chunk.buffer, &chunk.allocation, &info) !=
        VK_SUCCESS) {
      REXGPU_ERROR("[carbon-gpu] failed to allocate a {} MiB upload chunk", chunk.size >> 20);
      return false;
    }
    chunk.mapped = static_cast<uint8_t*>(info.pMappedData);
    // Descriptor set 0 over this chunk.
    const auto& dfn = vulkan_device_->functions();
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = set0_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set0_layout_;
    if (dfn.vkAllocateDescriptorSets(vk_device_, &dai, &chunk.set0) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorBufferInfo bufs[5] = {{chunk.buffer, 0, 4096},
                                      {chunk.buffer, 0, 4096},
                                      {chunk.buffer, 0, sizeof(DrawConstants)},
                                      {chunk.buffer, 0, VK_WHOLE_SIZE},
                                      {arena_buffer_ ? arena_buffer_ : chunk.buffer, 0,
                                       VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[5] = {};
    for (uint32_t i = 0; i < 5; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = chunk.set0;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType =
          i < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[i].pBufferInfo = &bufs[i];
    }
    dfn.vkUpdateDescriptorSets(vk_device_, 5, w, 0, nullptr);
    upload_chunks_.push_back(chunk);
    index = uint32_t(upload_chunks_.size() - 1);
  }
  frame().chunks.push_back(index);
  current_chunk_ = index;
  chunk_offset_ = 0;
  return true;
}

void Renderer::ReserveUpload(VkDeviceSize bytes) {
  if (current_chunk_ == UINT32_MAX || chunk_offset_ + bytes + 1024 > chunk_size_) {
    AcquireChunk();
  }
}

Renderer::UploadAllocation Renderer::Upload(VkDeviceSize size, VkDeviceSize alignment) {
  if (current_chunk_ == UINT32_MAX) {
    AcquireChunk();
  }
  VkDeviceSize offset = AlignUp64(chunk_offset_, alignment);
  if (offset + size > chunk_size_) {
    AcquireChunk();
    offset = 0;
  }
  UploadChunk& c = upload_chunks_[current_chunk_];
  chunk_offset_ = offset + size;
  stats_.upload_bytes += size;
  return {c.mapped + offset, c.buffer, offset, current_chunk_};
}

VkDescriptorSet Renderer::AllocateDescriptorSet(VkDescriptorSetLayout layout) {
  const auto& dfn = vulkan_device_->functions();
  VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = frame().descriptor_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (dfn.vkAllocateDescriptorSets(vk_device_, &ai, &set) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return set;
}

void Renderer::TransitionImage(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                               VkImageLayout& layout, VkImageLayout new_layout, uint32_t levels,
                               uint32_t layers) {
  if (layout == new_layout) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = layout;
  b.newLayout = new_layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, levels, 0, layers};
  VkPipelineStageFlags src_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkPipelineStageFlags dst_stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  if (precise_barriers_) {
    // Every image this renderer owns has one use per layout, so the layout names
    // the stages and accesses on each side. Writes in the old layout are made
    // available; reads in it only need the execution dependency (the layout
    // transition then cannot overwrite data a reader still needs).
    const LayoutUse src = LayoutUseOf(layout), dst = LayoutUseOf(new_layout);
    src_stages = src.stages;
    b.srcAccessMask = src.writes;
    dst_stages = dst.stages;
    b.dstAccessMask = dst.reads | dst.writes;
  } else {
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  }
  dfn.vkCmdPipelineBarrier(cb, src_stages, dst_stages, 0, 0, nullptr, 0, nullptr, 1, &b);
  ++stats_.barriers;
  layout = new_layout;
}

void Renderer::TransitionImageConservative(VkCommandBuffer cb, VkImage image,
                                           VkImageAspectFlags aspect, VkImageLayout& layout,
                                           VkImageLayout new_layout) {
  const bool precise = precise_barriers_;
  precise_barriers_ = false;
  TransitionImage(cb, image, aspect, layout, new_layout);
  precise_barriers_ = precise;
}

// ---------------------------------------------------------------------------
// Rendering instances
// ---------------------------------------------------------------------------

void Renderer::EndRendering() {
  if (!rendering_) {
    return;
  }
  vulkan_device_->functions().vkCmdEndRendering(frame().cb);
  for (RenderTarget* rt : current_pass_.color) {
    if (rt) rt->attachment_written = true;
  }
  if (current_pass_.depth) current_pass_.depth->attachment_written = true;
  if (ts_pool_[frame_index_]) {
    StampInfo info;
    info.width = current_pass_.width;
    info.height = current_pass_.height;
    info.draws = pass_draws_;
    info.ps_hash = pass_ps_hash_;
    info.vs_hash = pass_vs_hash_;
    info.depth = current_pass_.depth != nullptr;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_pass_.color[i]) {
        ++info.colors;
        if (!info.format) info.format = uint32_t(current_pass_.color[i]->format);
      }
    }
    GpuStamp(current_pass_.depth ? kTagScenePass : kTagOtherPass, &info);
  }
  pass_draws_ = 0;
  rendering_ = false;
  current_pass_ = PassState();
}

void Renderer::GpuStamp(GpuTag tag, const StampInfo* info) {
  VkQueryPool pool = ts_pool_[frame_index_];
  uint32_t& n = ts_count_[frame_index_];
  if (!pool || n >= kMaxStamps) {
    return;
  }
  pfn_write_timestamp_(frame().cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, pool, n);
  ts_tag_[frame_index_][n] = tag;
  ts_info_[frame_index_][n] = info ? *info : StampInfo();
  ++n;
}

void Renderer::ReadGpuStamps(uint32_t slot) {
  VkQueryPool pool = ts_pool_[slot];
  uint32_t n = ts_count_[slot];
  if (!pool || !ts_valid_[slot] || n < 2) {
    return;
  }
  uint64_t results[kMaxStamps];
  if (pfn_get_query_results_(vk_device_, pool, 0, n, sizeof(uint64_t) * n, results,
                             sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  double total = double(results[n - 1] - results[0]) * ts_period_ns_ * 1e-9;
  if (total <= 0.0 || total > 1.0) {
    return;
  }
  stats_.gpu_total_s += total;
  ++stats_.gpu_frames;
  gpu_ms_hist_.push_back(float(total * 1000.0));
  for (uint32_t i = 1; i < n; ++i) {
    double dt = double(results[i] - results[i - 1]) * ts_period_ns_ * 1e-9;
    if (dt > 0.0 && dt < 1.0) {
      stats_.gpu_tag_s[ts_tag_[slot][i]] += dt;
      uint8_t tag = ts_tag_[slot][i];
      if (tag == kTagScenePass || tag == kTagOtherPass) {
        const StampInfo& si = ts_info_[slot][i];
        uint64_t key = (uint64_t(si.width) << 44) ^ (uint64_t(si.height) << 28) ^
                       (uint64_t(si.format) << 8) ^ (uint64_t(si.colors) << 4) ^ si.depth;
        PassAgg& a = pass_agg_[key];
        a.info = si;
        a.seconds += dt;
        a.draws += si.draws;
        ++a.count;
      }
    }
    stats_.gpu_passes += ts_tag_[slot][i] == kTagScenePass || ts_tag_[slot][i] == kTagOtherPass;
  }
}

bool Renderer::BeginRendering(const PassState& pass, const VkClearValue* clear,
                              const VkRect2D* clear_area) {
  cmd_ = CmdCache();
  const auto& dfn = vulkan_device_->functions();
  VkCommandBuffer cb = frame().cb;
  // Attachments already in their attachment layout get no transition. If an
  // earlier rendering pass wrote them, this pass's loads and writes still need a
  // dependency on those writes; one barrier command covers all of them.
  VkImageMemoryBarrier same_layout[5];
  uint32_t same_layout_count = 0;
  VkPipelineStageFlags same_layout_stages = 0;
  auto attach = [&](RenderTarget* rt, VkImageAspectFlags aspect, VkImageLayout layout) {
    if (rt->layout != layout) {
      TransitionImage(cb, rt->image, aspect, rt->layout, layout);
    } else if (rt->attachment_written && precise_barriers_) {
      const LayoutUse use = LayoutUseOf(layout);
      VkImageMemoryBarrier& b = same_layout[same_layout_count++];
      b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = use.writes;
      b.dstAccessMask = use.reads | use.writes;
      b.oldLayout = b.newLayout = layout;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = rt->image;
      b.subresourceRange = {aspect, 0, 1, 0, 1};
      same_layout_stages |= use.stages;
    }
    rt->attachment_written = false;
  };
  VkRenderingAttachmentInfo color[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color[i].imageView = VK_NULL_HANDLE;
    color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (RenderTarget* rt = pass.color[i]) {
      attach(rt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      color[i].imageView = rt->view;
      if (clear && i == 0) {
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color[i].clearValue = *clear;
      }
      rt->last_used_frame = frame_number_;
    }
  }
  VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  if (pass.depth) {
    attach(pass.depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depth.imageView = pass.depth->view;
    if (clear && !pass.color[0]) {
      depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      depth.clearValue = *clear;
    }
    pass.depth->last_used_frame = frame_number_;
  }
  if (same_layout_count) {
    dfn.vkCmdPipelineBarrier(cb, same_layout_stages, same_layout_stages, 0, 0, nullptr, 0,
                             nullptr, same_layout_count, same_layout);
    ++stats_.attachment_barriers;
  }
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  uint32_t area_w = Scaled(pass.width), area_h = Scaled(pass.height);
  for (RenderTarget* rt : pass.color) {
    if (rt) {
      area_w = std::min(area_w, rt->width);
      area_h = std::min(area_h, rt->height);
    }
  }
  if (pass.depth) {
    area_w = std::min(area_w, pass.depth->width);
    area_h = std::min(area_h, pass.depth->height);
  }
  ri.renderArea = {{0, 0}, {area_w, area_h}};
  if (clear_area) {
    // Only this rectangle is cleared and stored; pixels outside the render area
    // are not touched by the load and store operations.
    ri.renderArea = *clear_area;
  }
  ri.layerCount = 1;
  ri.colorAttachmentCount = 4;
  ri.pColorAttachments = color;
  ri.pDepthAttachment = pass.depth ? &depth : nullptr;
  ri.pStencilAttachment = pass.depth ? &depth : nullptr;
  dfn.vkCmdBeginRendering(cb, &ri);
  rendering_ = true;
  current_pass_ = pass;
  if (tracing_) {
    std::string t;
    for (uint32_t i = 0; i < 4; ++i) {
      if (pass.color[i]) {
        t += fmt::format(" c{}=[base {} pitch {} fmt {} {}x{}]", i, pass.color[i]->edram_base,
                         pass.color[i]->pitch, pass.color[i]->guest_format, pass.color[i]->width,
                         pass.color[i]->height);
      }
    }
    if (pass.depth) {
      t += fmt::format(" depth=[base {} fmt {} {}x{}]", pass.depth->edram_base,
                       pass.depth->guest_format, pass.depth->width, pass.depth->height);
    }
    REXGPU_WARN("[carbon-gpu] trace: pass {}x{}{}", pass.width, pass.height, t);
  }
  ++stats_.passes;
  return true;
}

// ---------------------------------------------------------------------------
// Shaders and pipelines
// ---------------------------------------------------------------------------

Shader* Renderer::LoadShader(xenos::ShaderType type, const uint32_t* guest_be,
                             uint32_t dword_count) {
  uint64_t hash = HashBytes(guest_be, size_t(dword_count) * 4, uint64_t(type));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>();
  shader->type = type;
  shader->hash = hash;
  shader->ucode.resize(dword_count);
  for (uint32_t i = 0; i < dword_count; ++i) {
    shader->ucode[i] = ByteSwap32(guest_be[i]);
  }
  shader->translated = TranslateShader(type, shader->ucode.data(), dword_count);
  shader->translated_ok = shader->translated.valid;
  if (!shader->translated_ok) {
    REXGPU_ERROR("[carbon-gpu] failed to translate {} shader {:016X}: {}",
                 type == xenos::ShaderType::kVertex ? "vertex" : "pixel", hash,
                 shader->translated.error);
  }
  Shader* raw = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return raw;
}

VkShaderModule Renderer::CreateModuleFromGlsl(const std::string& glsl, bool vertex,
                                              const char* what) {
  const uint64_t module_key = ShaderCompiler::KeyFor(glsl);
  {
    std::lock_guard<std::mutex> lock(module_mutex_);
    auto it = modules_by_key_.find(module_key);
    if (it != modules_by_key_.end()) {
      return it->second;
    }
  }
  std::vector<uint32_t> spirv;
  std::string error;
  if (!compiler_->Compile(glsl, vertex, spirv, error)) {
    REXGPU_ERROR("[carbon-gpu] failed to compile {}: {}", what, error);
    static int dumped = 0;
    if (dumped < 8) {
      ++dumped;
      REXGPU_ERROR("[carbon-gpu] GLSL:\n{}", glsl);
    }
    return VK_NULL_HANDLE;
  }
  ++stats_.shaders_compiled;
  const auto& dfn = vulkan_device_->functions();
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  dfn.vkCreateShaderModule(vk_device_, &ci, nullptr, &module);
  if (module) {
    std::lock_guard<std::mutex> lock(module_mutex_);
    modules_by_key_[module_key] = module;
    module_keys_[module] = module_key;
  }
  return module;
}

VkShaderModule Renderer::ModuleForKey(uint64_t key) {
  {
    std::lock_guard<std::mutex> lock(module_mutex_);
    auto it = modules_by_key_.find(key);
    if (it != modules_by_key_.end()) {
      return it->second;
    }
  }
  std::vector<uint32_t> spirv;
  if (!compiler_->LoadCached(key, spirv)) {
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  vulkan_device_->functions().vkCreateShaderModule(vk_device_, &ci, nullptr, &module);
  if (module) {
    std::lock_guard<std::mutex> lock(module_mutex_);
    modules_by_key_[key] = module;
    module_keys_[module] = key;
  }
  return module;
}

static_assert(sizeof(VkShaderModule) == sizeof(uint64_t), "module keys replace the handles");

void Renderer::RecordWarmPipeline(const PipelineKey& key) {
  PipelineKey record = key;
  {
    std::lock_guard<std::mutex> lock(module_mutex_);
    auto vs = module_keys_.find(key.vs);
    auto ps = module_keys_.find(key.ps);
    if (vs == module_keys_.end() || ps == module_keys_.end()) {
      return;
    }
    std::memcpy(&record.vs, &vs->second, sizeof(uint64_t));
    std::memcpy(&record.ps, &ps->second, sizeof(uint64_t));
  }
  uint64_t hash = HashBytes(&record, sizeof(record));
  std::lock_guard<std::mutex> lock(warm_mutex_);
  if (!warm_seen_.insert(hash).second) {
    return;
  }
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  warm_records_.insert(warm_records_.end(), bytes, bytes + sizeof(record));
  warm_dirty_ = true;
  pipeline_cache_dirty_ = true;
}

void Renderer::SaveWarmList() {
  std::vector<uint8_t> records;
  {
    std::lock_guard<std::mutex> lock(warm_mutex_);
    if (!warm_dirty_ || warm_path_.empty()) {
      return;
    }
    warm_dirty_ = false;
    records = warm_records_;
  }
  uint32_t header[3] = {kWarmMagic, kWarmVersion, uint32_t(sizeof(PipelineKey))};
  std::filesystem::path temp = warm_path_;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(records.data()), std::streamsize(records.size()));
    if (!out) {
      return;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, warm_path_, ec);
}

void Renderer::RunPipelineWarmup() {
  std::vector<uint8_t> records;
  {
    std::lock_guard<std::mutex> lock(warm_mutex_);
    records = warm_records_;
  }
  const size_t count = records.size() / sizeof(PipelineKey);
  uint32_t queued = 0, missing = 0;
  const double start = NowSeconds();
  for (size_t i = 0; i < count; ++i) {
    PipelineKey key;
    std::memcpy(&key, records.data() + i * sizeof(PipelineKey), sizeof(key));
    uint64_t vs_key, ps_key;
    std::memcpy(&vs_key, &key.vs, sizeof(uint64_t));
    std::memcpy(&ps_key, &key.ps, sizeof(uint64_t));
    VkShaderModule vs = ModuleForKey(vs_key);
    VkShaderModule ps = ModuleForKey(ps_key);
    if (!vs || !ps) {
      ++missing;
      continue;
    }
    key.vs = vs;
    key.ps = ps;
    uint64_t hash = HashBytes(&key, sizeof(key));
    VkPipelineLayout layout = GetPipelineLayout(key.vs_textures, key.ps_textures);
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    if (pipelines_.count(hash)) {
      continue;
    }
    if (pipeline_workers_.empty()) {
      pipelines_.emplace(hash, BuildPipeline(key, layout));
    } else {
      pipelines_.emplace(hash, VK_NULL_HANDLE);
      pipeline_jobs_.push_back({std::make_shared<PipelineKey>(key), layout, hash});
      pipeline_cv_.notify_one();
    }
    ++queued;
  }
  REXGPU_INFO("[carbon-gpu] pipeline warm-up: {} pipelines queued from earlier sessions "
              "({} without cached shaders), {:.0f} ms",
              queued, missing, (NowSeconds() - start) * 1000.0);
}

static void DumpGlsl(uint64_t hash, const char* suffix, const std::string& glsl) {
  if (g_glsl_dump_dir.empty()) {
    return;
  }
  char name[64];
  std::snprintf(name, sizeof(name), "%016llx_%s.glsl", static_cast<unsigned long long>(hash),
                suffix);
  std::ofstream(g_glsl_dump_dir / name) << glsl;
}

VkShaderModule Renderer::GetVertexShaderModule(Shader* vs, const VertexShaderVariant& variant) {
  for (const auto& m : vs->vs_modules) {
    if (m.variant == variant) {
      return m.module;
    }
  }
  std::string glsl = BuildVertexShaderGlsl(vs->translated, variant);
  DumpGlsl(vs->hash, variant.rect_list ? "vs_rect" : "vs", glsl);
  VkShaderModule module = CreateModuleFromGlsl(glsl, true, "vertex shader");
  vs->vs_modules.push_back({variant, module});
  return module;
}

VkShaderModule Renderer::GetPixelShaderModule(Shader* ps, const PixelShaderVariant& variant) {
  for (const auto& m : ps->ps_modules) {
    if (m.variant == variant) {
      return m.module;
    }
  }
  std::string glsl = BuildPixelShaderGlsl(ps->translated, variant);
  DumpGlsl(ps->hash, "ps", glsl);
  VkShaderModule module = CreateModuleFromGlsl(glsl, false, "pixel shader");
  ps->ps_modules.push_back({variant, module});
  return module;
}

VkDescriptorSetLayout Renderer::GetTextureSetLayout(uint32_t count) {
  if (count >= texture_set_layouts_.size()) {
    count = uint32_t(texture_set_layouts_.size() - 1);
  }
  VkDescriptorSetLayout& layout = texture_set_layouts_[count];
  if (layout) {
    return layout;
  }
  std::vector<VkDescriptorSetLayoutBinding> b(count);
  for (uint32_t i = 0; i < count; ++i) {
    b[i].binding = i;
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  ci.bindingCount = count;
  ci.pBindings = b.data();
  vulkan_device_->functions().vkCreateDescriptorSetLayout(vk_device_, &ci, nullptr, &layout);
  return layout;
}

VkPipelineLayout Renderer::GetPipelineLayout(uint32_t vs_textures, uint32_t ps_textures) {
  uint32_t key = (vs_textures << 16) | ps_textures;
  auto it = pipeline_layouts_.find(key);
  if (it != pipeline_layouts_.end()) {
    return it->second;
  }
  VkDescriptorSetLayout sets[3] = {set0_layout_, GetTextureSetLayout(vs_textures),
                                   GetTextureSetLayout(ps_textures)};
  VkPipelineLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  ci.setLayoutCount = 3;
  ci.pSetLayouts = sets;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  vulkan_device_->functions().vkCreatePipelineLayout(vk_device_, &ci, nullptr, &layout);
  pipeline_layouts_.emplace(key, layout);
  return layout;
}

VkPipeline Renderer::GetPipeline(const PipelineKey& key) {
  // Hashed as raw bytes: no padding allowed.
  static_assert(std::has_unique_object_representations_v<PipelineKey>);
  uint64_t hash = HashBytes(&key, sizeof(key));
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    auto it = pipelines_.find(hash);
    if (it != pipelines_.end()) {
      return it->second;  // VK_NULL_HANDLE while a worker is still building it
    }
    if (!pipeline_workers_.empty()) {
      RecordWarmPipeline(key);
      // Built off the command thread: the draws that need it are skipped for
      // the few frames it takes, instead of the whole frame stalling.
      pipelines_.emplace(hash, VK_NULL_HANDLE);
      pipeline_jobs_.push_back({std::make_shared<PipelineKey>(key),
                                GetPipelineLayout(key.vs_textures, key.ps_textures), hash});
      pipeline_cv_.notify_one();
      ++stats_.pipelines_created;
      return VK_NULL_HANDLE;
    }
  }
  RecordWarmPipeline(key);
  VkPipeline pipeline =
      BuildPipeline(key, GetPipelineLayout(key.vs_textures, key.ps_textures));
  ++stats_.pipelines_created;
  frame_pipeline_builds_++;
  {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipelines_.emplace(hash, pipeline);
  }
  pipeline_cache_dirty_ = true;
  return pipeline;
}

void Renderer::PipelineWorker() {
  for (;;) {
    PipelineJob job;
    {
      std::unique_lock<std::mutex> lock(pipeline_mutex_);
      pipeline_cv_.wait(lock, [this] { return pipeline_stop_ || !pipeline_jobs_.empty(); });
      if (pipeline_stop_) {
        return;
      }
      job = pipeline_jobs_.front();
      pipeline_jobs_.pop_front();
    }
    VkPipeline pipeline = BuildPipeline(*job.key, job.layout);
    bool idle;
    {
      std::lock_guard<std::mutex> lock(pipeline_mutex_);
      pipelines_[job.hash] = pipeline;
      idle = pipeline_jobs_.empty();
    }
    pipeline_cache_dirty_ = true;
    if (idle) {
      SavePipelineCacheIfDue(3.0);
    }
  }
}

void Renderer::SavePipelineCacheIfDue(double min_interval) {
  if (!pipeline_cache_dirty_) {
    return;
  }
  std::lock_guard<std::mutex> lock(pipeline_cache_save_mutex_);
  double now = NowSeconds();
  if (now - pipeline_cache_saved_at_ < min_interval) {
    return;
  }
  pipeline_cache_dirty_ = false;
  pipeline_cache_saved_at_ = now;
  SavePipelineCache();
  SaveWarmList();
}

VkPipeline Renderer::BuildPipeline(const PipelineKey& key, VkPipelineLayout layout) {
  const auto& dfn = vulkan_device_->functions();
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = key.vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = key.ps;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vi = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(key.topology);
  ia.primitiveRestartEnable = key.primitive_restart;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.depthClampEnable = vulkan_device_->properties().depthClamp ? VK_TRUE : VK_FALSE;
  rs.polygonMode = VkPolygonMode(key.polygon_mode);
  rs.cullMode = key.cull_mode;
  rs.frontFace = VkFrontFace(key.front_face);
  rs.depthBiasEnable = key.depth_bias;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  ms.alphaToCoverageEnable = key.alpha_to_coverage;
  VkPipelineDepthStencilStateCreateInfo ds = {
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = key.depth_test;
  ds.depthWriteEnable = key.depth_write;
  ds.depthCompareOp = VkCompareOp(key.depth_compare);
  ds.stencilTestEnable = key.stencil_test;
  auto stencil = [](const uint32_t* s) {
    VkStencilOpState o = {};
    o.failOp = VkStencilOp(s[0]);
    o.passOp = VkStencilOp(s[1]);
    o.depthFailOp = VkStencilOp(s[2]);
    o.compareOp = VkCompareOp(s[3]);
    o.compareMask = 0xFF;
    o.writeMask = 0xFF;
    return o;
  };
  ds.front = stencil(key.stencil_front);
  ds.back = stencil(key.stencil_back);
  VkPipelineColorBlendAttachmentState att[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    att[i].blendEnable = key.blend[i][0];
    att[i].srcColorBlendFactor = VkBlendFactor(key.blend[i][1]);
    att[i].dstColorBlendFactor = VkBlendFactor(key.blend[i][2]);
    att[i].colorBlendOp = VkBlendOp(key.blend[i][3]);
    att[i].srcAlphaBlendFactor = VkBlendFactor(key.blend[i][4]);
    att[i].dstAlphaBlendFactor = VkBlendFactor(key.blend[i][5]);
    att[i].alphaBlendOp = VkBlendOp(key.blend[i][6]);
    att[i].colorWriteMask = key.write_mask[i];
  }
  VkPipelineColorBlendStateCreateInfo cb = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 4;
  cb.pAttachments = att;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                          VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                          VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                          VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = uint32_t(std::size(dyn));
  dy.pDynamicStates = dyn;
  VkFormat color_formats[4];
  for (uint32_t i = 0; i < 4; ++i) color_formats[i] = VkFormat(key.color_formats[i]);
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 4;
  rendering.pColorAttachmentFormats = color_formats;
  rendering.depthAttachmentFormat = VkFormat(key.depth_format);
  rendering.stencilAttachmentFormat = VkFormat(key.depth_format);

  VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rendering;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dy;
  ci.layout = layout;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = dfn.vkCreateGraphicsPipelines(vk_device_, pipeline_cache_, 1, &ci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    REXGPU_ERROR("[carbon-gpu] vkCreateGraphicsPipelines failed: {}", int(r));
    return VK_NULL_HANDLE;
  }
  return pipeline;
}

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------

namespace {

VkCompareOp ToVkCompare(xenos::CompareFunction f) {
  // Xenos and Vulkan share the same encoding.
  return VkCompareOp(uint32_t(f));
}

VkStencilOp ToVkStencilOp(xenos::StencilOp op) {
  switch (op) {
    case xenos::StencilOp::kKeep: return VK_STENCIL_OP_KEEP;
    case xenos::StencilOp::kZero: return VK_STENCIL_OP_ZERO;
    case xenos::StencilOp::kReplace: return VK_STENCIL_OP_REPLACE;
    case xenos::StencilOp::kIncrementClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case xenos::StencilOp::kDecrementClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case xenos::StencilOp::kInvert: return VK_STENCIL_OP_INVERT;
    case xenos::StencilOp::kIncrementWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case xenos::StencilOp::kDecrementWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
  }
  return VK_STENCIL_OP_KEEP;
}

VkBlendFactor ToVkBlendFactor(xenos::BlendFactor f) {
  switch (f) {
    case xenos::BlendFactor::kZero: return VK_BLEND_FACTOR_ZERO;
    case xenos::BlendFactor::kOne: return VK_BLEND_FACTOR_ONE;
    case xenos::BlendFactor::kSrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case xenos::BlendFactor::kOneMinusSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case xenos::BlendFactor::kSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case xenos::BlendFactor::kOneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case xenos::BlendFactor::kDstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case xenos::BlendFactor::kOneMinusDstColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case xenos::BlendFactor::kDstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case xenos::BlendFactor::kOneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case xenos::BlendFactor::kConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case xenos::BlendFactor::kOneMinusConstantColor:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case xenos::BlendFactor::kConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case xenos::BlendFactor::kOneMinusConstantAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case xenos::BlendFactor::kSrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToVkBlendOp(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kAdd: return VK_BLEND_OP_ADD;
    case xenos::BlendOp::kSubtract: return VK_BLEND_OP_SUBTRACT;
    case xenos::BlendOp::kMin: return VK_BLEND_OP_MIN;
    case xenos::BlendOp::kMax: return VK_BLEND_OP_MAX;
    case xenos::BlendOp::kRevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
  }
  return VK_BLEND_OP_ADD;
}

constexpr uint32_t kColorInfoRegs[4] = {0x2001, 0x2003, 0x2004, 0x2005};
constexpr uint32_t kBlendControlRegs[4] = {0x2201, 0x2209, 0x220A, 0x220B};

}  // namespace

void Renderer::ComputeViewport(const RegisterFile& regs, uint32_t rt_width, uint32_t rt_height,
                               DrawConstants& c, VkViewport& viewport, VkRect2D& scissor) {
  using namespace rex::graphics;
  auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto vtx_cntl = regs.Get<reg::PA_SU_VTX_CNTL>();
  float sx = vte.vport_x_scale_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
  float ox = vte.vport_x_offset_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
  float sy = vte.vport_y_scale_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
  float oy = vte.vport_y_offset_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
  float sz = vte.vport_z_scale_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
  float oz = vte.vport_z_offset_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
  float win_x = 0.0f, win_y = 0.0f;
  auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  if (mode.vtx_window_offset_enable) {
    win_x = float(window_offset.window_x_offset);
    win_y = float(window_offset.window_y_offset);
  }
  // Direct3D pixel centers are at integer coordinates.
  float half = vtx_cntl.pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
  float w = float(std::max<uint32_t>(rt_width, 1));
  float h = float(std::max<uint32_t>(rt_height, 1));
  c.ndc_scale[0] = sx * 2.0f / w;
  c.ndc_scale[1] = sy * 2.0f / h;
  c.ndc_scale[2] = sz;
  c.ndc_offset[0] = (ox + win_x + half) * 2.0f / w - 1.0f;
  c.ndc_offset[1] = (oy + win_y + half) * 2.0f / h - 1.0f;
  c.ndc_offset[2] = oz;
  c.vtx[0] = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 4u : 0u);
  c.pixel_pos[0] = 1.0f;
  c.pixel_pos[1] = 1.0f;
  // Param gen position: host pixel centers are at +0.5.
  c.pixel_pos[2] = -win_x - 0.5f + (half > 0.0f ? 0.0f : 0.5f);
  c.pixel_pos[3] = -win_y - 0.5f + (half > 0.0f ? 0.0f : 0.5f);

  viewport = {0.0f, 0.0f, w, h, 0.0f, 1.0f};

  // Scissor: window scissor (with the window offset) intersected with the
  // screen scissor and the render target.
  auto wtl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
  auto wbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
  auto stl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
  auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
  int32_t x0 = int32_t(wtl.tl_x), y0 = int32_t(wtl.tl_y);
  int32_t x1 = int32_t(wbr.br_x), y1 = int32_t(wbr.br_y);
  if (!wtl.window_offset_disable) {
    x0 += window_offset.window_x_offset;
    x1 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset;
    y1 += window_offset.window_y_offset;
  }
  x0 = std::max(x0, int32_t(stl.tl_x));
  y0 = std::max(y0, int32_t(stl.tl_y));
  x1 = std::min(x1, int32_t(sbr.br_x));
  y1 = std::min(y1, int32_t(sbr.br_y));
  x0 = std::clamp(x0, 0, int32_t(rt_width));
  y0 = std::clamp(y0, 0, int32_t(rt_height));
  x1 = std::clamp(x1, x0, int32_t(rt_width));
  y1 = std::clamp(y1, y0, int32_t(rt_height));
  if (res_scale_ != 1.0f) {
    // Guest pixels to host pixels; the NDC transform above is scale-free.
    const float s = res_scale_;
    viewport = {0.0f, 0.0f, w * s, h * s, 0.0f, 1.0f};
    int32_t max_x = int32_t(Scaled(rt_width)), max_y = int32_t(Scaled(rt_height));
    int32_t hx0 = std::min(int32_t(std::floor(x0 * s)), max_x);
    int32_t hy0 = std::min(int32_t(std::floor(y0 * s)), max_y);
    int32_t hx1 = std::clamp(int32_t(std::ceil(x1 * s)), hx0, max_x);
    int32_t hy1 = std::clamp(int32_t(std::ceil(y1 * s)), hy0, max_y);
    if (x1 == x0) hx1 = hx0;
    if (y1 == y0) hy1 = hy0;
    scissor = {{hx0, hy0}, {uint32_t(hx1 - hx0), uint32_t(hy1 - hy0)}};
    // Param gen positions stay in guest pixels.
    c.pixel_pos[0] = 1.0f / s;
    c.pixel_pos[1] = 1.0f / s;
    return;
  }
  scissor = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}

namespace {
constexpr uint32_t kArenaPageBytes = 16 * 1024;
constexpr uint8_t kArenaMaxInterval = 4;
}  // namespace

bool Renderer::CreateVertexArena() {
  int32_t mb = REXCVAR_GET(carbon_gpu_vertex_arena_mb);
  if (mb <= 0) {
    return true;
  }
  const auto& props = vulkan_device_->properties();
  arena_size_ = std::min<VkDeviceSize>(VkDeviceSize(mb) << 20, props.maxStorageBufferRange);
  arena_size_ = std::min<VkDeviceSize>(arena_size_, 0x7FFFFFF0u);
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = arena_size_;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo info = {};
  if (vmaCreateBuffer(allocator_, &bci, &aci, &arena_buffer_, &arena_allocation_, &info) !=
      VK_SUCCESS) {
    REXGPU_WARN("[carbon-gpu] could not allocate the {} MiB vertex arena; uploading per frame", mb);
    arena_buffer_ = VK_NULL_HANDLE;
    arena_size_ = 0;
    return true;
  }
  arena_mapped_ = static_cast<uint8_t*>(info.pMappedData);
  return true;
}

// Makes [lo, hi) bytes of the guest vertex buffer available in the arena.
// Returns false when the buffer has to go through the per-frame upload.
bool Renderer::ArenaFetch(uint64_t key, uint32_t address, uint32_t size, uint32_t lo, uint32_t hi,
                          uint32_t& dword_offset) {
  if (!arena_buffer_ || !size || size > arena_size_ / 4) {
    return false;
  }
  const uint8_t* src = physical_base_ + (address & 0x1FFFFFFF);
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto it = arena_entries_.find(key);
    if (it == arena_entries_.end()) {
      auto ch = arena_changes_.find(key);
      if (ch != arena_changes_.end() && ch->second >= 3) {
        return false;  // rewritten too often to be worth caching
      }
      VkDeviceSize aligned = AlignUp64(size, 256);
      if (arena_used_ + aligned > arena_size_) {
        arena_reset_pending_ = true;
        return false;
      }
      ArenaEntry e;
      e.offset = uint32_t(arena_used_);
      e.size = size;
      uint32_t pages = (size + kArenaPageBytes - 1) / kArenaPageBytes;
      e.page_hash.assign(pages, 0);
      e.page_checked.assign(pages, 0);
      e.page_interval.assign(pages, 1);
      arena_used_ += aligned;
      it = arena_entries_.emplace(key, std::move(e)).first;
    }
    ArenaEntry& e = it->second;
    bool changed = false;
    if (hi > lo) {
      uint32_t first = lo / kArenaPageBytes, last = (hi - 1) / kArenaPageBytes;
      for (uint32_t p = first; p <= last && p < e.page_hash.size(); ++p) {
        if (e.page_checked[p] == frame_number_) continue;
        if (e.page_hash[p] && frame_number_ < e.page_checked[p] + e.page_interval[p]) continue;
        uint32_t page_start = p * kArenaPageBytes;
        uint32_t n = std::min(kArenaPageBytes, size - page_start);
        uint64_t h = HashBytes(src + page_start, n, 0) | 1;
        if (!e.page_hash[p]) {
          std::memcpy(arena_mapped_ + e.offset + page_start, src + page_start, n);
          vmaFlushAllocation(allocator_, arena_allocation_, e.offset + page_start, n);
          e.page_hash[p] = h;
          stats_.arena_bytes += n;
        } else if (h != e.page_hash[p]) {
          changed = true;
          break;
        } else {
          e.page_interval[p] = std::min<uint8_t>(e.page_interval[p] * 2, kArenaMaxInterval);
        }
        e.page_checked[p] = frame_number_;
      }
    }
    if (!changed) {
      dword_offset = e.offset / 4;
      return true;
    }
    // The buffer was rewritten. Frames in flight may still read the old copy, so
    // never overwrite it: drop the entry and copy into a fresh one (or give up on
    // buffers that keep changing).
    uint8_t& count = arena_changes_[key];
    if (count < 255) ++count;
    arena_entries_.erase(key);
    if (count >= 3) {
      return false;
    }
  }
  return false;
}

bool Renderer::SetupVertexData(const RegisterFile& regs, Shader* vs, DrawConstants& c,
                               const VertexRange& range) {
  const TranslatedShader& t = vs->translated;
  const bool use_ranges = REXCVAR_GET(carbon_gpu_vertex_ranges);
  for (uint32_t i = 0; i < 96; ++i) {
    c.vfetch[i * 2] = 0;
    c.vfetch[i * 2 + 1] = 0;
    if (!(t.vfetch_used[i >> 5] & (1u << (i & 31)))) {
      continue;
    }
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(i);
    uint32_t address = fetch.address << 2;
    uint32_t size = fetch.size << 2;
    if (fetch.type != xenos::FetchConstantType::kVertex || !size) {
      // Point at a zeroed dword so reads are defined.
      size = 0;
    }
    size = std::min<uint32_t>(size, uint32_t(chunk_size_ / 4));
    uint64_t key = (uint64_t(address) << 32) | size;

    if (size) {
      uint32_t lo = 0, hi = size;
      uint32_t stride = t.vfetch_stride[i];
      if (use_ranges && range.known && stride && !(t.vfetch_computed[i >> 5] & (1u << (i & 31)))) {
        uint64_t lo64 = uint64_t(range.min_index) * stride * 4;
        uint64_t hi64 = (uint64_t(range.max_index) * stride +
                         std::max<uint32_t>(stride, t.vfetch_extent[i])) * 4;
        lo = uint32_t(std::min<uint64_t>(lo64, size));
        hi = uint32_t(std::min<uint64_t>(hi64, size));
      }
      uint32_t arena_dwords;
      if (ArenaFetch(key, address, size, lo, hi, arena_dwords)) {
        c.vfetch[i * 2] = arena_dwords;
        c.vfetch[i * 2 + 1] = uint32_t(fetch.endian) | 0x80000000u;
        ++stats_.arena_draws;
        continue;
      }
    }
    ++stats_.chunk_vertex_draws;
    if (vertex_upload_cache_chunk_ != current_chunk_) {
      vertex_upload_cache_.clear();
      vertex_upload_cache_chunk_ = current_chunk_;
    }
    auto it = vertex_upload_cache_.find(key);
    uint32_t dword_offset;
    if (it != vertex_upload_cache_.end()) {
      dword_offset = it->second;
    } else {
      UploadAllocation a = Upload(std::max<uint32_t>(size, 16), 16);
      if (a.chunk != vertex_upload_cache_chunk_) {
        // A new chunk was started: the set 0 of this draw must use it, and
        // anything uploaded before is in the old chunk. Callers reserve
        // enough space up front, so this is rare; redo the whole draw setup.
        vertex_upload_cache_.clear();
        vertex_upload_cache_chunk_ = a.chunk;
        return false;
      }
      if (size) {
        std::memcpy(a.ptr, physical_base_ + (address & 0x1FFFFFFF), size);
      } else {
        std::memset(a.ptr, 0, 16);
      }
      dword_offset = uint32_t(a.offset / 4);
      vertex_upload_cache_.emplace(key, dword_offset);
    }
    c.vfetch[i * 2] = dword_offset;
    c.vfetch[i * 2 + 1] = uint32_t(fetch.endian);
  }
  return true;
}

void Renderer::Draw(const RegisterFile& regs, Shader* vs, Shader* ps, const DrawInfo& info) {
  // One measurement feeds both the stats window and the slow-frame report.
  struct Timer {
    Renderer* self;
    double start = self->profile_ ? NowSeconds() : 0.0;
    ~Timer() {
      if (self->profile_) {
        const double dt = NowSeconds() - start;
        self->stats_.draw_s += dt;
        self->frame_draw_s_ += dt;
      }
    }
  } draw_timer{this};
  using namespace rex::graphics;
  if (!BeginFrame()) {
    return;
  }
  ++stats_.draws;
  if (!vs || !vs->translated_ok || !info.index_count) {
    Skip(kSkipNoShader);
    return;
  }
  auto edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    Skip(kSkipEdramMode);
    return;
  }
  if (ps && !ps->translated_ok) {
    Skip(kSkipPsFailed);
    return;
  }
  if (vs->translated.uses_memexport && !vs->translated.interpolators_written) {
    // Memory export only: no visible output.
    Skip(kSkipMemexport);
    return;
  }

  // Primitive type.
  VkPrimitiveTopology topology;
  bool restart_allowed = false;
  enum class Expand { kNone, kQuadList, kTriangleFan, kLineLoop, kRectList } expand = Expand::kNone;
  switch (info.primitive_type) {
    case xenos::PrimitiveType::kPointList:
      topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
      break;
    case xenos::PrimitiveType::kLineList:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
      break;
    case xenos::PrimitiveType::kLineStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      restart_allowed = true;
      break;
    case xenos::PrimitiveType::kTriangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      break;
    case xenos::PrimitiveType::kTriangleStrip:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
      restart_allowed = true;
      break;
    case xenos::PrimitiveType::kTriangleFan:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      expand = Expand::kTriangleFan;
      break;
    case xenos::PrimitiveType::kQuadList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      expand = Expand::kQuadList;
      break;
    case xenos::PrimitiveType::kLineLoop:
      topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
      expand = Expand::kLineLoop;
      break;
    case xenos::PrimitiveType::kRectangleList:
      topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      expand = Expand::kRectList;
      break;
    default: {
      static uint32_t logged = 0;
      if (logged < 8) {
        ++logged;
        REXGPU_WARN("[carbon-gpu] unsupported primitive type {:02X}",
                    uint32_t(info.primitive_type));
      }
      Skip(kSkipPrimitive);
      return;
    }
  }

  // The bloom blur chain is 6-8 full-screen passes of 8 scattered texture reads each;
  // on phone GPUs they cost more than the whole scene for a subtle glow.
  if (!bloom_enabled_ && ps) {
    switch (ps->hash) {
      case 0xF84B05E744D442E8ull:
      case 0x2047A91B3C7E8F62ull:
      case 0x56B15EE1C23228EEull:
      case 0xA25318EF353C6AD7ull:
        Skip(kSkipNoTarget);
        return;
      default:
        break;
    }
  }

  // Render targets.
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface.surface_pitch;
  if (!pitch) {
    Skip(kSkipNoPitch);
    return;
  }
  // Height needed: the bottom of the scissor, or of the viewport when that is
  // higher up. Screen-space draws often leave the scissor at its 8192 default,
  // so the EDRAM layout of the bound targets caps it further below.
  uint32_t needed_height;
  {
    auto wtl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    auto wbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
    int32_t window_y = regs.Get<reg::PA_SC_WINDOW_OFFSET>().window_y_offset;
    int32_t bottom = int32_t(wbr.br_y) + (wtl.window_offset_disable ? 0 : window_y);
    bottom = std::min(bottom, int32_t(sbr.br_y));
    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    if (vte.vport_y_scale_ena) {
      float sy = std::fabs(regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_YSCALE));
      float oy = vte.vport_y_offset_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
      float vp_bottom = oy + sy;
      if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
        vp_bottom += float(window_y);
      }
      if (vp_bottom > 0.0f && vp_bottom < 8192.0f) {
        bottom = std::min(bottom, int32_t(std::ceil(vp_bottom)));
      }
    }
    needed_height = uint32_t(std::clamp<int32_t>(bottom, 1, 8192));
  }
  // Rows a target can have before it runs into the next bound target (or the
  // end of the 2048-tile EDRAM). Tiles are 80x16 samples of 32 bits.
  auto msaa = surface.msaa_samples;
  uint32_t edram_bases[5] = {};
  uint32_t edram_base_count = 0;
  auto edram_rows = [&](uint32_t base, bool wide) {
    uint32_t limit = 2048;
    for (uint32_t i = 0; i < edram_base_count; ++i) {
      if (edram_bases[i] > base && edram_bases[i] < limit) limit = edram_bases[i];
    }
    if (base >= limit) return 16u;
    uint32_t tile_w = 80 >> ((msaa == xenos::MsaaSamples::k4X ? 1 : 0) + (wide ? 1 : 0));
    uint32_t tile_h = msaa != xenos::MsaaSamples::k1X ? 8 : 16;
    uint32_t tiles_per_strip = (pitch + tile_w - 1) / tile_w;
    return std::max((limit - base) / tiles_per_strip * tile_h, 16u);
  };
  PassState pass;
  uint32_t color_write_masks[4] = {};
  uint32_t ps_colors = ps ? ps->translated.colors_written : 0;
  auto color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  bool depth_used = depth_control.z_enable || depth_control.stencil_enable;
  auto di = regs.Get<reg::RB_DEPTH_INFO>();
  reg::RB_COLOR_INFO color_infos[4];
  bool color_used[4] = {};
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t mask = (color_mask >> (i * 4)) & 0xF;
      if (!mask || !(ps_colors & (1u << i))) {
        continue;
      }
      color_infos[i].value = regs[kColorInfoRegs[i]];
      color_used[i] = true;
      color_write_masks[i] = mask;
      edram_bases[edram_base_count++] = color_infos[i].color_base;
    }
  }
  if (depth_used) {
    edram_bases[edram_base_count++] = di.depth_base;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!color_used[i]) continue;
    const auto& ci = color_infos[i];
    pass.color[i] =
        GetRenderTarget(false, ci.color_base, pitch, uint32_t(ci.color_format),
                        std::min(needed_height,
                                 edram_rows(ci.color_base, xenos::IsColorRenderTargetFormat64bpp(ci.color_format))));
  }
  if (depth_used) {
    pass.depth = GetRenderTarget(true, di.depth_base, pitch, uint32_t(di.depth_format),
                                 std::min(needed_height, edram_rows(di.depth_base, false)));
  }
  bool any_target = pass.depth != nullptr;
  for (auto* c : pass.color) any_target |= c != nullptr;
  if (!any_target) {
    if (tracing_) {
      REXGPU_WARN(
          "[carbon-gpu] trace: no target: edram mode {} color mask {:04X} ps {:016X} colors {:X} "
          "depthcontrol {:08X} prim {:02X} count {}",
          uint32_t(edram_mode), color_mask, ps ? ps->hash : 0, ps_colors,
          regs[XE_GPU_REG_RB_DEPTHCONTROL], uint32_t(info.primitive_type), info.index_count);
    }
    Skip(kSkipNoTarget);
    return;
  }
  pass.width = UINT32_MAX;
  pass.height = UINT32_MAX;
  uint32_t hinted_rows = UINT32_MAX;
  for (auto* c : pass.color) {
    if (c) {
      pass.width = std::min(pass.width, c->lw);
      pass.height = std::min(pass.height, c->lh);
      if (c->used_rows) hinted_rows = std::min(hinted_rows, c->used_rows);
    }
  }
  if (pass.depth) {
    pass.width = std::min(pass.width, pass.depth->lw);
    pass.height = std::min(pass.height, pass.depth->lh);
    if (pass.depth->used_rows) hinted_rows = std::min(hinted_rows, pass.depth->used_rows);
  }
  pass.height = std::min(pass.height, hinted_rows);

  // Car reflection faces (280-pitch target with depth) and the mirror (640-pitch
  // 8888 target with depth) can be updated less often than the main view.
  if (pass.color[0] && pass.depth && !pass.color[1]) {
    const RenderTarget* c0 = pass.color[0];
    bool throttle = false;
    if (c0->pitch == 280 && reflection_faces_ == 0) {
      throttle = true;
    } else if (c0->pitch == 280 && reflection_faces_ < 6) {
      uint32_t face = cube_face_in_frame_ % 6;
      uint32_t first = uint32_t((frame_number_ * uint64_t(reflection_faces_)) % 6);
      throttle = (face + 6 - first) % 6 >= uint32_t(reflection_faces_);
    } else if (mirror_half_rate_ && c0->pitch == 640 && c0->guest_format == 0 &&
               (frame_number_ & 1)) {
      throttle = true;
    }
    if (throttle) {
      skip_resolve_rt_ = pass.color[0];
      ++stats_.throttled;
      Skip(kSkipNoTarget);
      return;
    }
  }

  // Debug cycle: 0 normal, 1 post passes with a 1-pixel scissor, 2 post passes with an
  // empty pixel shader, 3 post passes skipped entirely.
  bool debug_tiny = false, debug_null_ps = false;
  if (REXCVAR_GET(carbon_gpu_debug_cycle)) {
    uint32_t mode = uint32_t(NowSeconds() / 10.0) & 3;
    debug_mode_ = mode;
    bool small_pass = !pass.depth && pass.width <= 640;
    if (small_pass) {
      if (mode == 1) debug_tiny = true;
      if (mode == 2) debug_null_ps = true;
      if (mode == 3) {
        Skip(kSkipNoTarget);
        return;
      }
    }
  }

  // Shaders.
  auto program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  uint32_t interpolators = std::min<uint32_t>(16, program_cntl.vs_export_count + 1);
  if (ps) {
    interpolators = std::min<uint32_t>(interpolators, program_cntl.ps_num_reg + 1);
  } else {
    interpolators = 0;
  }
  VertexShaderVariant vs_variant;
  vs_variant.interpolator_count = interpolators;
  vs_variant.rect_list = expand == Expand::kRectList;
  VkShaderModule vs_module = GetVertexShaderModule(vs, vs_variant);
  VkShaderModule ps_module = null_ps_module_;
  if (ps) {
    PixelShaderVariant ps_variant;
    ps_variant.interpolator_count = interpolators;
    // SQ_INTERPOLATOR_CNTL.param_shade is not used for flat shading: the game
    // leaves bits set for texture coordinates (boot movies sampled one texel).
    // Everything is interpolated smoothly, as in the SDK's Xenos backends.
    ps_variant.flat_mask = 0;
    ps_variant.param_gen = program_cntl.param_gen != 0;
    ps_variant.param_gen_index = ps_variant.param_gen
                                     ? regs.Get<reg::SQ_CONTEXT_MISC>().param_gen_pos
                                     : 0;
    ps_module = GetPixelShaderModule(ps, ps_variant);
    if (debug_null_ps) ps_module = null_ps_module_;
  }
  if (!vs_module || !ps_module) {
    Skip(kSkipModules);
    return;
  }

  // Pipeline state.
  PipelineKey key = {};
  key.vs = vs_module;
  key.ps = ps_module;
  key.vs_textures = vs->translated.texture_descriptor_count;
  key.ps_textures = ps ? ps->translated.texture_descriptor_count : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    key.color_formats[i] = pass.color[i] ? uint32_t(pass.color[i]->format) : 0;
    key.write_mask[i] = color_write_masks[i];
  }
  key.depth_format = pass.depth ? uint32_t(pass.depth->format) : 0;
  key.topology = uint32_t(topology);
  auto mode = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  key.primitive_restart = restart_allowed && mode.multi_prim_ib_ena && info.indexed;
  bool is_triangles = topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                      topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  uint32_t cull = 0;
  if (is_triangles && expand != Expand::kRectList) {
    if (mode.cull_front) cull |= VK_CULL_MODE_FRONT_BIT;
    if (mode.cull_back) cull |= VK_CULL_MODE_BACK_BIT;
  }
  if (cull == VK_CULL_MODE_FRONT_AND_BACK) {
    Skip(kSkipCullAll);
    return;
  }
  key.cull_mode = cull;
  // face: 0 = front is CCW, 1 = front is CW (screen space, y down like Vulkan).
  key.front_face = mode.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  key.polygon_mode = VK_POLYGON_MODE_FILL;
  if (mode.poly_mode == xenos::PolygonModeEnable::kDualMode && is_triangles &&
      vulkan_device_->properties().fillModeNonSolid) {
    xenos::PolygonType type = mode.cull_front ? mode.polymode_back_ptype : mode.polymode_front_ptype;
    if (type == xenos::PolygonType::kLines) key.polygon_mode = VK_POLYGON_MODE_LINE;
    if (type == xenos::PolygonType::kPoints) key.polygon_mode = VK_POLYGON_MODE_POINT;
  }
  bool poly_offset = is_triangles ? (mode.poly_offset_front_enable || mode.poly_offset_back_enable)
                                  : mode.poly_offset_para_enable;
  key.depth_bias = poly_offset && pass.depth;
  if (pass.depth) {
    key.depth_test = depth_control.z_enable;
    key.depth_write = depth_control.z_enable && depth_control.z_write_enable;
    key.depth_compare = depth_control.z_enable ? ToVkCompare(depth_control.zfunc) : VK_COMPARE_OP_ALWAYS;
    key.stencil_test = depth_control.stencil_enable;
    if (depth_control.stencil_enable) {
      key.stencil_front[0] = ToVkStencilOp(depth_control.stencilfail);
      key.stencil_front[1] = ToVkStencilOp(depth_control.stencilzpass);
      key.stencil_front[2] = ToVkStencilOp(depth_control.stencilzfail);
      key.stencil_front[3] = ToVkCompare(depth_control.stencilfunc);
      if (depth_control.backface_enable) {
        key.stencil_back[0] = ToVkStencilOp(depth_control.stencilfail_bf);
        key.stencil_back[1] = ToVkStencilOp(depth_control.stencilzpass_bf);
        key.stencil_back[2] = ToVkStencilOp(depth_control.stencilzfail_bf);
        key.stencil_back[3] = ToVkCompare(depth_control.stencilfunc_bf);
      } else {
        std::memcpy(key.stencil_back, key.stencil_front, sizeof(key.stencil_back));
      }
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!pass.color[i]) {
      continue;
    }
    reg::RB_BLENDCONTROL bc;
    bc.value = regs[kBlendControlRegs[i]];
    bool enable = !(bc.color_srcblend == xenos::BlendFactor::kOne &&
                    bc.color_destblend == xenos::BlendFactor::kZero &&
                    bc.color_comb_fcn == xenos::BlendOp::kAdd &&
                    bc.alpha_srcblend == xenos::BlendFactor::kOne &&
                    bc.alpha_destblend == xenos::BlendFactor::kZero &&
                    bc.alpha_comb_fcn == xenos::BlendOp::kAdd);
    key.blend[i][0] = enable;
    if (enable) {
      key.blend[i][1] = ToVkBlendFactor(bc.color_srcblend);
      key.blend[i][2] = ToVkBlendFactor(bc.color_destblend);
      key.blend[i][3] = ToVkBlendOp(bc.color_comb_fcn);
      key.blend[i][4] = ToVkBlendFactor(bc.alpha_srcblend);
      key.blend[i][5] = ToVkBlendFactor(bc.alpha_destblend);
      key.blend[i][6] = ToVkBlendOp(bc.alpha_comb_fcn);
    }
  }
  auto color_control = regs.Get<reg::RB_COLORCONTROL>();
  key.alpha_to_coverage = 0;
  VkPipeline pipeline = GetPipeline(key);
  if (!pipeline) {
    Skip(kSkipPipeline);
    return;
  }

  // ---- Uploads (all in one chunk) ----
  // Only the header is cleared: the per-fetch arrays are written for the entries
  // the shaders use, and only those entries are uploaded.
  DrawConstants consts;
  std::memset(&consts, 0, offsetof(DrawConstants, vfetch));
  std::memcpy(consts.bools, &regs.values[kRegBoolConstants], sizeof(consts.bools));
  std::memcpy(consts.loops, &regs.values[kRegLoopConstants], sizeof(consts.loops));
  consts.vtx[1] = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  consts.vtx[2] = regs[XE_GPU_REG_VGT_MIN_VTX_INDX] & 0xFFFFFF;
  consts.vtx[3] = regs[XE_GPU_REG_VGT_MAX_VTX_INDX] & 0xFFFFFF;
  if (consts.vtx[3] < consts.vtx[2]) {
    consts.vtx[3] = 0xFFFFFF;
  }
  consts.vtx2[0] = 0xFFFFFFFFu;
  consts.alpha_test[0] = regs.GetFloat(XE_GPU_REG_RB_ALPHA_REF);
  consts.alpha_test[1] = float(uint32_t(color_control.alpha_func));
  consts.alpha_test[2] = color_control.alpha_test_enable && pass.color[0] ? 1.0f : 0.0f;
  VkViewport viewport;
  VkRect2D scissor;
  ComputeViewport(regs, pass.width, pass.height, consts, viewport, scissor);
  if (debug_tiny) {
    scissor = {{0, 0}, {1, 1}};
  }
  if (!scissor.extent.width || !scissor.extent.height) {
    Skip(kSkipScissor);
    return;
  }

  // Estimate the upload size so the draw fits in one chunk.
  VkDeviceSize estimate = 4096 * 2 + sizeof(DrawConstants) + 3 * 256;
  for (uint32_t i = 0; i < 96; ++i) {
    if (!arena_buffer_ && (vs->translated.vfetch_used[i >> 5] & (1u << (i & 31)))) {
      estimate += (VkDeviceSize(regs.GetVertexFetch(i).size) << 2) + 32;
    }
  }
  estimate += VkDeviceSize(info.index_count) * 8 + 64;
  ReserveUpload(std::min(estimate, chunk_size_ - 2048));

  // Textures (may record uploads in the upload command buffer).
  VkDescriptorSet vs_set = VK_NULL_HANDLE, ps_set = VK_NULL_HANDLE;
  BindTextures(regs, vs, 1, consts, vs_set);
  if (ps) {
    BindTextures(regs, ps, 2, consts, ps_set);
  }

  // Indices.
  uint32_t host_count = info.index_count;
  bool host_indexed = false;
  VkIndexType index_type = VK_INDEX_TYPE_UINT32;
  VkBuffer index_buffer = VK_NULL_HANDLE;
  VkDeviceSize index_offset = 0;
  uint32_t reset_index = regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;
  const uint8_t* guest_indices =
      info.indexed ? physical_base_ + (info.index_base & 0x1FFFFFFF) : nullptr;
  uint32_t available = info.indexed ? info.index_buffer_size_bytes /
                                          (info.index_format == xenos::IndexFormat::kInt16 ? 2 : 4)
                                    : info.index_count;
  uint32_t count = std::min(info.index_count, available);
  auto read_index = [&](uint32_t i) -> uint32_t {
    if (!info.indexed) {
      return i;
    }
    if (info.index_format == xenos::IndexFormat::kInt16) {
      uint16_t v;
      std::memcpy(&v, guest_indices + i * 2, 2);
      if (info.index_endian == xenos::Endian::k8in16 || info.index_endian == xenos::Endian::k8in32) {
        v = ByteSwap16(v);
      }
      return v;
    }
    uint32_t v;
    std::memcpy(&v, guest_indices + i * 4, 4);
    return GpuSwap32(v, info.index_endian);
  };
  // The vertices the indices reach, so only that part of each buffer is copied.
  VertexRange vertex_range;
  bool indices_done = false;
  const uint32_t index_offset_v = regs[XE_GPU_REG_VGT_INDX_OFFSET] & 0xFFFFFF;
  if (info.indexed && expand == Expand::kNone && count) {
    // Plain indexed draw: convert straight into the upload buffer and take the
    // vertex range from the same pass.
    const bool is16 = info.index_format == xenos::IndexFormat::kInt16;
    UploadAllocation a = Upload(VkDeviceSize(count) * (is16 ? 2 : 4) + 4, 16);
    uint32_t lo = 0, hi = 0;
    if (is16) {
      bool swap = info.index_endian == xenos::Endian::k8in16 ||
                  info.index_endian == xenos::Endian::k8in32;
      ConvertIndices16(guest_indices, reinterpret_cast<uint16_t*>(a.ptr), count, swap,
                       key.primitive_restart != 0, reset_index & 0xFFFF, lo, hi);
      index_type = VK_INDEX_TYPE_UINT16;
    } else {
      ConvertIndices32(guest_indices, reinterpret_cast<uint32_t*>(a.ptr), count, info.index_endian,
                       key.primitive_restart != 0, reset_index, lo, hi);
      index_type = VK_INDEX_TYPE_UINT32;
    }
    index_buffer = a.buffer;
    index_offset = a.offset;
    host_indexed = true;
    host_count = count;
    indices_done = true;
    if (lo <= hi) {
      vertex_range.known = true;
      vertex_range.min_index = lo + index_offset_v;
      vertex_range.max_index = hi + index_offset_v;
    }
  } else if (!info.indexed) {
    vertex_range.known = count > 0;
    vertex_range.min_index = index_offset_v;
    vertex_range.max_index = index_offset_v + (count ? count - 1 : 0);
  } else if (count) {
    uint32_t lo_i = UINT32_MAX, hi_i = 0;
    bool restart = key.primitive_restart != 0;
    uint32_t reset_mask = info.index_format == xenos::IndexFormat::kInt16 ? 0xFFFF : 0xFFFFFF;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = read_index(i) & reset_mask;
      if (restart && v == (reset_index & reset_mask)) continue;
      lo_i = std::min(lo_i, v);
      hi_i = std::max(hi_i, v);
    }
    if (lo_i <= hi_i) {
      vertex_range.known = true;
      vertex_range.min_index = lo_i + index_offset_v;
      vertex_range.max_index = hi_i + index_offset_v;
    }
  }
  if (!SetupVertexData(regs, vs, consts, vertex_range)) {
    // Chunk switched mid-way; retry once in the fresh chunk.
    if (!SetupVertexData(regs, vs, consts, vertex_range)) {
      Skip(kSkipVertices);
      return;
    }
  }
  if (indices_done) {
    // Already converted above.
  } else if (expand == Expand::kRectList) {
    uint32_t rects = count / 3;
    host_count = rects * 6;
    if (info.indexed) {
      UploadAllocation a = Upload(VkDeviceSize(rects) * 3 * 4 + 16, 16);
      uint32_t* dst = reinterpret_cast<uint32_t*>(a.ptr);
      for (uint32_t i = 0; i < rects * 3; ++i) dst[i] = read_index(i);
      consts.vtx2[0] = uint32_t(a.offset / 4);
    }
  } else if (expand != Expand::kNone) {
    std::vector<uint32_t>& out = scratch_indices_;
    out.clear();
    if (expand == Expand::kQuadList) {
      uint32_t quads = count / 4;
      out.reserve(quads * 6);
      for (uint32_t q = 0; q < quads; ++q) {
        uint32_t b = q * 4;
        uint32_t i0 = read_index(b), i1 = read_index(b + 1), i2 = read_index(b + 2),
                 i3 = read_index(b + 3);
        out.insert(out.end(), {i0, i1, i2, i0, i2, i3});
      }
    } else if (expand == Expand::kTriangleFan) {
      if (count >= 3) {
        uint32_t i0 = read_index(0);
        for (uint32_t i = 1; i + 1 < count; ++i) {
          out.insert(out.end(), {i0, read_index(i), read_index(i + 1)});
        }
      }
    } else {  // Line loop.
      for (uint32_t i = 0; i < count; ++i) out.push_back(read_index(i));
      if (count) out.push_back(read_index(0));
    }
    host_count = uint32_t(out.size());
    if (host_count) {
      UploadAllocation a = Upload(VkDeviceSize(host_count) * 4, 16);
      std::memcpy(a.ptr, out.data(), size_t(host_count) * 4);
      index_buffer = a.buffer;
      index_offset = a.offset;
      host_indexed = true;
    }
  } else if (info.indexed) {
    host_count = count;
    bool is16 = info.index_format == xenos::IndexFormat::kInt16;
    UploadAllocation a = Upload(VkDeviceSize(count) * (is16 ? 2 : 4) + 4, 16);
    if (is16) {
      uint16_t* dst = reinterpret_cast<uint16_t*>(a.ptr);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = read_index(i);
        dst[i] = key.primitive_restart && v == (reset_index & 0xFFFF) ? 0xFFFF : uint16_t(v);
      }
      index_type = VK_INDEX_TYPE_UINT16;
    } else {
      uint32_t* dst = reinterpret_cast<uint32_t*>(a.ptr);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t v = read_index(i) & 0xFFFFFF;
        dst[i] = key.primitive_restart && v == reset_index ? 0xFFFFFFFFu : v;
      }
    }
    index_buffer = a.buffer;
    index_offset = a.offset;
    host_indexed = true;
  }
  if (!host_count) {
    Skip(kSkipNoIndices);
    return;
  }

  // Constants.
  uint32_t dyn_offsets[3];
  for (uint32_t stage = 0; stage < 2; ++stage) {
    Shader* s = stage == 0 ? vs : ps;
    uint32_t n = s ? s->translated.float_constant_count : 0;
    if (!float_constants_dirty_[stage] && last_constant_chunk_[stage] == current_chunk_ &&
        last_constant_count_[stage] >= n) {
      dyn_offsets[stage] = uint32_t(last_constant_offset_[stage]);
      ++stats_.constant_reuses;
      continue;
    }
    UploadAllocation a = Upload(4096, ubo_alignment_);
    ++stats_.constant_uploads;
    if (n) {
      std::memcpy(a.ptr, &regs.values[kRegFloatConstants + stage * 1024], size_t(n) * 16);
    }
    dyn_offsets[stage] = uint32_t(a.offset);
    last_constant_offset_[stage] = a.offset;
    last_constant_chunk_[stage] = a.chunk;
    last_constant_count_[stage] = n;
    float_constants_dirty_[stage] = false;
  }
  {
    UploadAllocation a = Upload(sizeof(DrawConstants), ubo_alignment_);
    const uint8_t* src = reinterpret_cast<const uint8_t*>(&consts);
    constexpr size_t kHead = offsetof(DrawConstants, vfetch);
    std::memcpy(a.ptr, src, kHead);
    // Vertex fetch slots up to the highest one the vertex shader uses.
    uint32_t vfetch_count = 0;
    for (int w = 2; w >= 0; --w) {
      if (uint32_t bits = vs->translated.vfetch_used[w]) {
        vfetch_count = uint32_t(w) * 32 + 32 - uint32_t(__builtin_clz(bits));
        break;
      }
    }
    std::memcpy(a.ptr + kHead, src + kHead, size_t(vfetch_count) * 2 * sizeof(uint32_t));
    // Texture parameters for the fetch constants either shader samples.
    uint32_t tex_mask = 0;
    for (const TextureBinding& b : vs->translated.textures) tex_mask |= 1u << (b.fetch_index & 31);
    if (ps) {
      for (const TextureBinding& b : ps->translated.textures) tex_mask |= 1u << (b.fetch_index & 31);
    }
    while (tex_mask) {
      uint32_t fi = uint32_t(__builtin_ctz(tex_mask));
      tex_mask &= tex_mask - 1;
      for (size_t field : {offsetof(DrawConstants, tex_size), offsetof(DrawConstants, tex_info),
                           offsetof(DrawConstants, tex_uv)}) {
        std::memcpy(a.ptr + field + fi * 16, src + field + fi * 16, 16);
      }
    }
    dyn_offsets[2] = uint32_t(a.offset);
  }

  if (tracing_) {
    REXGPU_WARN(
        "[carbon-gpu] trace: draw prim {:02X} count {} indexed {} vs {:016X} ps {:016X} "
        "tex {}+{} scissor {},{} {}x{} vte {:X} ndc {:.4f},{:.4f} {:.4f},{:.4f}",
        uint32_t(info.primitive_type), info.index_count, info.indexed, vs->hash,
        ps ? ps->hash : 0, key.vs_textures, key.ps_textures, scissor.offset.x, scissor.offset.y,
        scissor.extent.width, scissor.extent.height, consts.vtx[0], consts.ndc_scale[0],
        consts.ndc_scale[1], consts.ndc_offset[0], consts.ndc_offset[1]);
  }
  // ---- Record ----
  bool same_pass = rendering_;
  if (same_pass) {
    for (uint32_t i = 0; i < 4; ++i) same_pass &= current_pass_.color[i] == pass.color[i];
    same_pass &= current_pass_.depth == pass.depth;
    same_pass &= current_pass_.width == pass.width && current_pass_.height == pass.height;
  }
  if (!same_pass) {
    EndRendering();
    BeginRendering(pass);
  }
  const auto& dfn = vulkan_device_->functions();
  VkCommandBuffer cb = frame().cb;
  if (cmd_.pipeline != pipeline) {
    dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    cmd_.pipeline = pipeline;
  }
  ++pass_draws_;
  // Contents change: invalidates any earlier resolve that read these targets.
  for (RenderTarget* rt : current_pass_.color) {
    if (rt) rt->write_gen = ++rt_write_counter_;
  }
  if (current_pass_.depth) current_pass_.depth->write_gen = ++rt_write_counter_;
  pass_ps_hash_ = ps ? ps->hash : 0;
  pass_vs_hash_ = vs->hash;
  VkPipelineLayout layout = GetPipelineLayout(key.vs_textures, key.ps_textures);
  VkDescriptorSet set0 = upload_chunks_[current_chunk_].set0;
  dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set0, 3,
                              dyn_offsets);
  // Texture sets stay bound while the set layouts they sit under are unchanged.
  if (cmd_.layout != layout) {
    if (cmd_.vs_textures != key.vs_textures) {
      cmd_.set1 = cmd_.set2 = VK_NULL_HANDLE;
    } else if (cmd_.ps_textures != key.ps_textures) {
      cmd_.set2 = VK_NULL_HANDLE;
    }
    cmd_.layout = layout;
    cmd_.vs_textures = key.vs_textures;
    cmd_.ps_textures = key.ps_textures;
  }
  if (vs_set && cmd_.set1 != vs_set) {
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &vs_set, 0,
                                nullptr);
    cmd_.set1 = vs_set;
  }
  if (ps_set && cmd_.set2 != ps_set) {
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &ps_set, 0,
                                nullptr);
    cmd_.set2 = ps_set;
  }
  if (!cmd_.viewport_valid || std::memcmp(&cmd_.viewport, &viewport, sizeof(viewport)) != 0) {
    dfn.vkCmdSetViewport(cb, 0, 1, &viewport);
    cmd_.viewport = viewport;
    cmd_.viewport_valid = true;
  }
  if (!cmd_.scissor_valid || std::memcmp(&cmd_.scissor, &scissor, sizeof(scissor)) != 0) {
    dfn.vkCmdSetScissor(cb, 0, 1, &scissor);
    cmd_.scissor = scissor;
    cmd_.scissor_valid = true;
  }
  {
    float scale = 0.0f, offset = 0.0f;
    if (key.depth_bias) {
      bool front = mode.poly_offset_front_enable;
      scale = regs.GetFloat(front ? XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE
                                  : XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
      offset = regs.GetFloat(front ? XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET
                                   : XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      scale *= xenos::kPolygonOffsetScaleSubpixelUnit;
      offset *= float(1 << 24);
    }
    if (!cmd_.depth_bias_valid || cmd_.depth_bias[0] != offset || cmd_.depth_bias[1] != scale) {
      dfn.vkCmdSetDepthBias(cb, offset, 0.0f, scale);
      cmd_.depth_bias[0] = offset;
      cmd_.depth_bias[1] = scale;
      cmd_.depth_bias_valid = true;
    }
  }
  {
    float blend[4] = {regs.GetFloat(XE_GPU_REG_RB_BLEND_RED), regs.GetFloat(XE_GPU_REG_RB_BLEND_GREEN),
                      regs.GetFloat(XE_GPU_REG_RB_BLEND_BLUE), regs.GetFloat(XE_GPU_REG_RB_BLEND_ALPHA)};
    if (!cmd_.blend_valid || std::memcmp(cmd_.blend, blend, sizeof(blend)) != 0) {
      dfn.vkCmdSetBlendConstants(cb, blend);
      std::memcpy(cmd_.blend, blend, sizeof(blend));
      cmd_.blend_valid = true;
    }
  }
  if (pass.depth) {
    auto ref = regs.Get<reg::RB_STENCILREFMASK>();
    reg::RB_STENCILREFMASK ref_bf;
    ref_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
    if (!depth_control.backface_enable) ref_bf = ref;
    uint32_t st[6] = {ref.stencilref,       ref_bf.stencilref,       ref.stencilmask,
                      ref_bf.stencilmask,   ref.stencilwritemask,    ref_bf.stencilwritemask};
    if (!cmd_.stencil_valid || std::memcmp(cmd_.stencil, st, sizeof(st)) != 0) {
      dfn.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, st[0]);
      dfn.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, st[1]);
      dfn.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, st[2]);
      dfn.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, st[3]);
      dfn.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, st[4]);
      dfn.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, st[5]);
      std::memcpy(cmd_.stencil, st, sizeof(st));
      cmd_.stencil_valid = true;
    }
  }
  if (host_indexed) {
    if (cmd_.index_buffer != index_buffer || cmd_.index_offset != index_offset ||
        cmd_.index_type != index_type) {
      dfn.vkCmdBindIndexBuffer(cb, index_buffer, index_offset, index_type);
      cmd_.index_buffer = index_buffer;
      cmd_.index_offset = index_offset;
      cmd_.index_type = index_type;
    }
    dfn.vkCmdDrawIndexed(cb, host_count, 1, 0, 0, 0);
  } else {
    dfn.vkCmdDraw(cb, host_count, 1, 0, 0);
  }
}

// ---------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------

bool Renderer::CreatePresentPipeline() {
  const auto& dfn = vulkan_device_->functions();
  static const char* kVs = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
  static const char* kPs = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D xe_source;
layout(std430, set = 0, binding = 1) readonly buffer XeGamma { uint table[256]; } xe_gamma;
layout(push_constant) uniform XePresent { vec2 scale; uint use_gamma; uint swap_rb; } xe_p;
layout(location = 0) out vec4 xe_out;
void main() {
  vec4 c = texture(xe_source, gl_FragCoord.xy * xe_p.scale);
  if (xe_p.swap_rb != 0u) c = c.bgra;
  if (xe_p.use_gamma != 0u) {
    uvec3 i = uvec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);
    uint r = xe_gamma.table[i.r], g = xe_gamma.table[i.g], b = xe_gamma.table[i.b];
    c.rgb = vec3(float((r >> 20) & 1023u), float((g >> 10) & 1023u), float(b & 1023u)) / 1023.0;
  }
  xe_out = vec4(c.rgb, 1.0);
}
)";
  VkShaderModule vs = CreateModuleFromGlsl(kVs, true, "present VS");
  VkShaderModule ps = CreateModuleFromGlsl(kPs, false, "present PS");
  if (!vs || !ps) {
    return false;
  }
  VkDescriptorSetLayoutBinding b[2] = {};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 2;
  lci.pBindings = b;
  dfn.vkCreateDescriptorSetLayout(vk_device_, &lci, nullptr, &present_set_layout_);
  VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &present_set_layout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pc;
  dfn.vkCreatePipelineLayout(vk_device_, &plci, nullptr, &present_pipeline_layout_);

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_FRAGMENT_BIT, ps, "main", nullptr};
  VkPipelineVertexInputStateCreateInfo vi = {
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState att = {};
  att.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cbs = {
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = 1;
  cbs.pAttachments = &att;
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;
  VkFormat out_format = rex::ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &out_format;
  VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  ci.pNext = &rendering;
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pColorBlendState = &cbs;
  ci.pDynamicState = &dy;
  ci.layout = present_pipeline_layout_;
  if (dfn.vkCreateGraphicsPipelines(vk_device_, VK_NULL_HANDLE, 1, &ci, nullptr,
                                    &present_pipeline_) != VK_SUCCESS) {
    return false;
  }

  // Gamma tables, one per frame slot.
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = 1024 * kFramesInFlight;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_AUTO;
  aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
              VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo info = {};
  if (vmaCreateBuffer(allocator_, &bci, &aci, &gamma_buffer_, &gamma_allocation_, &info) !=
      VK_SUCCESS) {
    return false;
  }
  gamma_mapped_ = static_cast<uint32_t*>(info.pMappedData);

  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_LINEAR;
  sci.minFilter = VK_FILTER_LINEAR;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.maxLod = 0.0f;
  dfn.vkCreateSampler(vk_device_, &sci, nullptr, &linear_sampler_);
  return true;
}

void Renderer::RecordPresent(VkCommandBuffer cb, VkImageView source, uint32_t source_width,
                             uint32_t source_height, float source_scale, VkImage dest, VkImageView dest_view,
                             uint32_t width, uint32_t height, bool dest_written_before, bool swap_rb) {
  const auto& dfn = vulkan_device_->functions();
  VkImageLayout dest_layout = dest_written_before
                                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout
                                  : VK_IMAGE_LAYOUT_UNDEFINED;
  // The presenter also uses this image, outside this renderer's tracking.
  TransitionImageConservative(cb, dest, VK_IMAGE_ASPECT_COLOR_BIT, dest_layout,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = dest_view;
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = source ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {width, height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  dfn.vkCmdBeginRendering(cb, &ri);
  if (source) {
    VkDescriptorSet set = AllocateDescriptorSet(present_set_layout_);
    VkDescriptorImageInfo ii = {linear_sampler_, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi = {gamma_buffer_, VkDeviceSize(frame_index_) * 1024, 1024};
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = w[1].dstSet = set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &ii;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bi;
    dfn.vkUpdateDescriptorSets(vk_device_, 2, w, 0, nullptr);
    dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_layout_, 0,
                                1, &set, 0, nullptr);
    struct {
      float scale[2];
      uint32_t use_gamma;
      uint32_t swap_rb;
    } pc = {{1.0f / float(source_width), 1.0f / float(source_height)}, 1, swap_rb ? 1u : 0u};
    // The guest output may be smaller than the source image (padded resolves).
    pc.scale[0] = source_scale / float(source_width);
    pc.scale[1] = source_scale / float(source_height);
    dfn.vkCmdPushConstants(cb, present_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    VkViewport vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {width, height}};
    dfn.vkCmdSetViewport(cb, 0, 1, &vp);
    dfn.vkCmdSetScissor(cb, 0, 1, &sc);
    dfn.vkCmdDraw(cb, 3, 1, 0, 0);
  }
  dfn.vkCmdEndRendering(cb);
  TransitionImageConservative(cb, dest, VK_IMAGE_ASPECT_COLOR_BIT, dest_layout,
                              rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
}

void Renderer::Swap(const RegisterFile& regs, uint32_t frontbuffer_ptr, uint32_t width,
                    uint32_t height, const GammaRamp& gamma) {
  if (g_paused.load(std::memory_order_acquire)) {
    SavePipelineCacheIfDue(0.0);
  }
  while (g_paused.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cap_next_ = 0.0;
  }
  // Re-read fps_cap_ each frame so runtime cvar changes take effect immediately.
  fps_cap_ = REXCVAR_GET(carbon_gpu_fps_cap);
  skip_redundant_resolves_ = REXCVAR_GET(carbon_gpu_skip_redundant_resolves);
  if (frame_number_ == 1) {
    REXGPU_INFO("[carbon-gpu] frame pacing: requested cap {}, additional host sleep {}",
                fps_cap_, system_->NeedsHostFrameCap(fps_cap_) ? "enabled" : "disabled");
  }

#if defined(__ANDROID__)
  // Lazy ADPF init: called on the rendering thread so gettid() gives the
  // right TID.  Must happen after the first fps_cap_ read above.
  if (!adpf_init_tried_) {
    adpf_init_tried_ = true;
    InitAdpf();
  }
  const double adpf_work_start = NowSeconds();
#endif

  struct Timer {
    double& total;
    double start = NowSeconds();
    ~Timer() { total += NowSeconds() - start; }
  } swap_timer{stats_.swap_s};
  if (!BeginFrame()) {
    return;
  }
  EndRendering();
  std::memcpy(gamma_mapped_ + frame_index_ * 256, gamma.table, 1024);
  if (frame_number_ % 600 == 0) {
    REXGPU_WARN("[carbon-gpu] gamma table[64]={:08X} [128]={:08X} [255]={:08X} dirty {} pwl_dirty {}",
                gamma.table[64], gamma.table[128], gamma.table[255], gamma.table_dirty,
                gamma.pwl_dirty);
  }

  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_w = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_h = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  // The front buffer is normally the destination of the last resolve.
  Texture* front = FindResolvedTexture(frontbuffer_ptr & 0x1FFFFFFF, width, height);
  if (front && front->has_unsampled) {
    for (ResolveSig& s : front->resolve_sigs) s.sampled = true;
    front->has_unsampled = false;
  }
  if (!front) {
    // Not resolved this way: try the guest texture in memory.
    GuestTexture g;
    g.base_address = frontbuffer_ptr & 0x1FFFF000;
    g.width = std::max<uint32_t>(width, 1);
    g.height = std::max<uint32_t>(height, 1);
    g.pitch_texels = AlignUp(g.width, 32);
    g.format = xenos::TextureFormat::k_8_8_8_8;
    g.endian = xenos::Endian::k8in32;
    g.tiled = true;
    if (g.base_address) {
      front = GetTexture(g, true);
    }
  }
  VkImageView source_view = VK_NULL_HANDLE;
  uint32_t source_w = width ? width : 1280, source_h = height ? height : 720;
  if (front) {
    TransitionImage(frame().cb, front->image, VK_IMAGE_ASPECT_COLOR_BIT, front->layout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    source_view = GetTextureView(*front, kSwizzleRgba, false,
                                 VK_IMAGE_VIEW_TYPE_2D);
    source_w = front->width;
    source_h = front->height;
  }
  const float source_scale = front ? front->res_scale : 1.0f;
  uint32_t out_w = width ? width : (front ? front->guest.width : source_w);
  uint32_t out_h = height ? height : (front ? front->guest.height : source_h);

  // Frame dump (debugging): copy the front buffer to a readback buffer.
  VkBuffer dump_buffer = VK_NULL_HANDLE;
  VmaAllocation dump_allocation = VK_NULL_HANDLE;
  void* dump_mapped = nullptr;
  uint32_t dump_frame_slot = frame_index_;
  uint64_t dump_number = frame_number_;
  int32_t dump_interval = REXCVAR_GET(carbon_gpu_dump_frame_interval);
  if (front && dump_interval > 0 && frame_number_ % uint64_t(dump_interval) == 0 &&
      front->format.host_format == VK_FORMAT_R8G8B8A8_UNORM) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = VkDeviceSize(front->width) * front->height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ai;
    if (vmaCreateBuffer(allocator_, &bci, &aci, &dump_buffer, &dump_allocation, &ai) ==
        VK_SUCCESS) {
      dump_mapped = ai.pMappedData;
      VkCommandBuffer cb = frame().cb;
      TransitionImage(cb, front->image, VK_IMAGE_ASPECT_COLOR_BIT, front->layout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      VkBufferImageCopy region = {};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {front->width, front->height, 1};
      vulkan_device_->functions().vkCmdCopyImageToBuffer(
          cb, front->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buffer, 1, &region);
      TransitionImage(cb, front->image, VK_IMAGE_ASPECT_COLOR_BIT, front->layout,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
  }

  bool submitted = false;
  if (presenter_) {
    presenter_->RefreshGuestOutput(
        out_w, out_h, display_w, display_h,
        [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
          auto& vk_context =
              static_cast<rex::ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(
                  context);
          context.SetIs8bpc(true);
          EndFrame(true, [&](VkCommandBuffer cb) {
            RecordPresent(cb, source_view, source_w, source_h, source_scale, vk_context.image(),
                          vk_context.image_view(), out_w, out_h,
                          vk_context.image_ever_written_previously(), front && front->resolved_swap_rb);
          });
          submitted = true;
          return true;
        });
  }
  if (!submitted) {
    EndFrame(false, nullptr);
  }
  if (dump_buffer) {
    const auto& dfn = vulkan_device_->functions();
    dfn.vkWaitForFences(vk_device_, 1, &frames_[dump_frame_slot].fence, VK_TRUE, UINT64_MAX);
    vmaInvalidateAllocation(allocator_, dump_allocation, 0, VK_WHOLE_SIZE);
    WriteBmp(fmt::format("carbon_frames/frame_{:06}.bmp", dump_number),
             static_cast<const uint8_t*>(dump_mapped), front->width, front->height,
             front->resolved_swap_rb);
    vmaDestroyBuffer(allocator_, dump_buffer, dump_allocation);
  }

#if defined(__ANDROID__)
  // Report actual work duration to ADPF *before* the fps cap sleep, so the
  // scheduler sees the real cost, not the artificial idle.
  if (adpf_session_ && adpf_report_fn_ && adpf_update_fn_) {
    const int64_t actual_ns = std::max(int64_t(1'000'000LL),
        int64_t((NowSeconds() - adpf_work_start) * 1e9));
    adpf_report_fn_(adpf_session_, actual_ns);
    if (fps_cap_ != adpf_last_cap_) {
      adpf_last_cap_ = fps_cap_;
      const int64_t target_ns = fps_cap_ > 0
          ? int64_t(1e9 / fps_cap_) : int64_t(16'666'667LL);
      adpf_update_fn_(adpf_session_, target_ns);
    }
  }
#endif

  // Guest vblank already enforces its refresh rate. Add a host sleep only for a
  // lower cap, or when guest vsync is disabled, to avoid two drifting 60 Hz clocks.
  if (system_->NeedsHostFrameCap(fps_cap_)) {
    const double period = 1.0 / double(fps_cap_);
    double t = NowSeconds();
    if (cap_next_ == 0.0 || t - cap_next_ > period * 2.0) {
      cap_next_ = t;
    }
    cap_next_ += period;
    if (t < cap_next_) {
      std::this_thread::sleep_for(std::chrono::duration<double>(cap_next_ - t));
    }
  } else {
    cap_next_ = 0.0;
  }

  // Statistics.
  ++stats_frames_;
  double now = NowSeconds();
  if (profile_ && overlay_last_swap_ > 0.0 && (now - overlay_last_swap_) * 1000.0 > 25.0 &&
      REXCVAR_GET(carbon_gpu_stats) && spike_reports_ < 6) {
    ++spike_reports_;
    REXGPU_INFO(
        "[carbon-gpu] slow frame {:.1f} ms: draws {:.1f} ms, texture uploads {} ({:.1f} ms), "
        "pipelines built inline {}, cp idle {:.1f} ms, cp WAIT_REG_MEM {:.1f} ms | "
        "texture decode {:.1f} ms, staging {:.1f} ms",
        (now - overlay_last_swap_) * 1000.0, frame_draw_s_ * 1000.0, frame_textures_,
        frame_texture_s_ * 1000.0, frame_pipeline_builds_, frame_idle_s_ * 1000.0,
        frame_wait_s_ * 1000.0, frame_texture_decode_s_ * 1000.0, frame_texture_stage_s_ * 1000.0);
  }
  frame_idle_s_ = frame_wait_s_ = frame_texture_s_ = frame_draw_s_ = 0;
  frame_texture_decode_s_ = frame_texture_stage_s_ = 0;
  frame_textures_ = frame_pipeline_builds_ = 0;
  if (overlay_last_swap_ > 0.0) {
    double dt_ms = (now - overlay_last_swap_) * 1000.0;
    stats_.worst_ms = std::max(stats_.worst_ms, dt_ms);
    frame_ms_hist_.push_back(float(dt_ms));
    stats_.slow25 += dt_ms > 25.0;
    stats_.slow34 += dt_ms > 34.0;
    stats_.slow50 += dt_ms > 50.0;
    overlay_worst_ms_ = std::max(overlay_worst_ms_, (now - overlay_last_swap_) * 1000.0);
  } else {
    overlay_start_ = now;
  }
  overlay_last_swap_ = now;
  ++overlay_frames_;
  if (now - overlay_start_ >= 0.5) {
#if defined(__ANDROID__)
    double secs = now - overlay_start_;
    rex_gpu_report_fps(float(overlay_frames_ / secs), float(secs * 1000.0 / overlay_frames_),
                       float(overlay_worst_ms_));
#endif
    overlay_start_ = now;
    overlay_frames_ = 0;
    overlay_worst_ms_ = 0.0;
  }
  if (now - stats_start_ >= 5.0) {
    if (REXCVAR_GET(carbon_gpu_stats)) {
      double secs = now - stats_start_;
      double f = double(std::max<uint64_t>(stats_frames_, 1));
      REXGPU_INFO(
          "[carbon-gpu] {:.1f} fps | per frame: draws {:.0f} (skipped {:.0f}), passes {:.1f}, "
          "resolves {:.1f}, upload {:.2f} MiB, vertex copy {:.2f} MiB (arena {:.0f}/chunk {:.0f} fetches) | "
          "new: pipelines {}, shaders {}, textures {} | mem: tex {} MiB, rt {} MiB, arena {} MiB, evicted {}",
          f / secs, stats_.draws / f, stats_.draws_skipped / f, stats_.passes / f,
          stats_.resolves / f, double(stats_.upload_bytes) / f / (1024.0 * 1024.0),
          double(stats_.arena_bytes) / f / (1024.0 * 1024.0), stats_.arena_draws / f,
          stats_.chunk_vertex_draws / f, stats_.pipelines_created, stats_.shaders_compiled,
          stats_.textures_uploaded, texture_bytes_ >> 20, render_target_bytes_ >> 20,
          arena_used_ >> 20, stats_.evicted);
    }
    if (REXCVAR_GET(carbon_gpu_stats) && stats_.draws_skipped) {
      static const char* kNames[kSkipReasonCount] = {
          "no-vs", "edram-mode", "ps-failed", "memexport", "primitive", "no-pitch", "no-output",
          "modules", "cull-all", "pipeline", "scissor", "vertices", "no-indices"};
      std::string reasons;
      double frames = double(std::max<uint64_t>(stats_frames_, 1));
      for (uint32_t i = 0; i < kSkipReasonCount; ++i) {
        if (stats_.skip[i]) {
          reasons += fmt::format(" {}={:.1f}", kNames[i], stats_.skip[i] / frames);
        }
      }
      REXGPU_INFO("[carbon-gpu] skipped per frame:{}", reasons);
    }
    if (REXCVAR_GET(carbon_gpu_stats)) {
      double frames = double(std::max<uint64_t>(stats_frames_, 1));
      // Draw and resolve CPU time are only measured while profiling.
      std::string cpu_detail =
          profile_ ? fmt::format("draw {:.2f}, resolve {:.2f}, ", stats_.draw_s * 1000.0 / frames,
                                 stats_.resolve_s * 1000.0 / frames)
                   : std::string();
      REXGPU_INFO(
          "[carbon-gpu] pacing(dbg mode {}): avg {:.1f} ms, worst {:.0f} ms, frames >25ms {} >34ms {} >50ms {} | "
          "per frame ms: {}swap {:.2f}, gpu fence wait {:.2f}",
          debug_mode_, (now - stats_start_) * 1000.0 / frames, stats_.worst_ms, stats_.slow25, stats_.slow34,
          stats_.slow50, cpu_detail, stats_.swap_s * 1000.0 / frames, stats_.fence_s * 1000.0 / frames);
      auto pct = [](std::vector<float>& v, double p) {
        if (v.empty()) return 0.0f;
        size_t k = std::min(v.size() - 1, size_t(p * double(v.size())));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
      };
      if (!stats_.gpu_frames) {
        REXGPU_INFO("[carbon-gpu] frame time ms p50/p95/p99 {:.1f}/{:.1f}/{:.1f}",
                    pct(frame_ms_hist_, 0.50), pct(frame_ms_hist_, 0.95),
                    pct(frame_ms_hist_, 0.99));
      }
      if (stats_.gpu_frames) {
        std::vector<const PassAgg*> top;
        for (auto& kv : pass_agg_) top.push_back(&kv.second);
        std::sort(top.begin(), top.end(),
                  [](const PassAgg* a, const PassAgg* b) { return a->seconds > b->seconds; });
        double gf = double(stats_.gpu_frames);
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
          const PassAgg& a = *top[i];
          REXGPU_INFO(
              "[carbon-gpu]   pass type {}x{} fmt {} colors {} depth {}: {:.2f} passes/frame, "
              "{:.1f} draws/frame, {:.2f} ms/frame ({:.3f} ms each) ps {:016X} vs {:016X}",
              a.info.width, a.info.height, a.info.format, a.info.colors, a.info.depth,
              a.count / gf, a.draws / gf, a.seconds * 1000.0 / gf, a.seconds * 1000.0 / a.count,
              a.info.ps_hash, a.info.vs_hash);
        }
        pass_agg_.clear();
        double g = double(stats_.gpu_frames);
        {
          REXGPU_INFO(
              "[carbon-gpu] frame time ms p50/p95/p99 {:.1f}/{:.1f}/{:.1f} | gpu time ms "
              "p50/p95/p99 {:.1f}/{:.1f}/{:.1f}",
              pct(frame_ms_hist_, 0.50), pct(frame_ms_hist_, 0.95), pct(frame_ms_hist_, 0.99),
              pct(gpu_ms_hist_, 0.50), pct(gpu_ms_hist_, 0.95), pct(gpu_ms_hist_, 0.99));
        }
        REXGPU_INFO(
            "[carbon-gpu] gpu time per frame: total {:.1f} ms | scene passes {:.1f}, other passes {:.1f}, "
            "resolves {:.1f}, present+tail {:.1f} (passes {:.0f})",
            stats_.gpu_total_s * 1000.0 / g, stats_.gpu_tag_s[kTagScenePass] * 1000.0 / g,
            stats_.gpu_tag_s[kTagOtherPass] * 1000.0 / g, stats_.gpu_tag_s[kTagResolve] * 1000.0 / g,
            stats_.gpu_tag_s[kTagEnd] * 1000.0 / g, stats_.gpu_passes / g);
      }
    }
    if (REXCVAR_GET(carbon_gpu_stats)) {
      double f = double(std::max<uint64_t>(stats_frames_, 1));
      REXGPU_INFO(
          "[carbon-gpu] resolves per frame: copied {:.1f}, skipped as redundant {:.1f} | pixels "
          "copied {:.0f} k, skipped {:.0f} k | bandwidth copied {:.1f} MiB, avoided {:.1f} MiB "
          "(skip {}) | overwritten before sampled: {:.1f} resolves, {:.0f} k px, {:.1f} MiB",
          stats_.resolves_copied / f, stats_.resolves_skipped / f,
          double(stats_.resolve_px_copied) / f / 1e3, double(stats_.resolve_px_skipped) / f / 1e3,
          double(stats_.resolve_bytes_copied) / f / (1024.0 * 1024.0),
          double(stats_.resolve_bytes_skipped) / f / (1024.0 * 1024.0),
          REXCVAR_GET(carbon_gpu_skip_redundant_resolves) ? "on" : "off",
          stats_.resolves_dead / f, double(stats_.resolve_px_dead) / f / 1e3,
          double(stats_.resolve_bytes_dead) / f / (1024.0 * 1024.0));
      REXGPU_INFO(
          "[carbon-gpu] constant writes per frame: {:.0f} dwords, {:.0f} unchanged | stage blocks "
          "uploaded {:.1f}, reused {:.1f} | full-overwrite resolves {:.1f}",
          double(stats_.constant_words) / f,
          double(stats_.constant_words - stats_.constant_words_changed) / f,
          stats_.constant_uploads / f, stats_.constant_reuses / f,
          stats_.resolves_full_overwrite / f);
      REXGPU_INFO("[carbon-gpu] clears per frame: {:.1f} full (load op), {:.1f} partial (load op), "
                  "{:.1f} by clear command | barriers per frame: {:.1f} layout, {:.1f} attachment "
                  "(precise {})",
                  stats_.clears_full / f, stats_.clears_partial / f, stats_.clears_command / f,
                  stats_.barriers / f, stats_.attachment_barriers / f,
                  precise_barriers_ ? "on" : "off");
      std::vector<std::pair<std::string, uint32_t>> shapes(partial_clear_agg_.begin(),
                                                           partial_clear_agg_.end());
      std::sort(shapes.begin(), shapes.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      for (size_t i = 0; i < shapes.size() && i < 6; ++i) {
        REXGPU_INFO("[carbon-gpu]   partial clear {}: {:.2f}/frame", shapes[i].first,
                    shapes[i].second / f);
      }
      partial_clear_agg_.clear();
    }
    if (REXCVAR_GET(carbon_gpu_stats) && !dest_agg_.empty()) {
      std::vector<const DestAgg*> top;
      for (auto& kv : dest_agg_) top.push_back(&kv.second);
      std::sort(top.begin(), top.end(), [](const DestAgg* a, const DestAgg* b) {
        return a->px * a->dead / std::max<uint32_t>(a->resolves, 1) >
               b->px * b->dead / std::max<uint32_t>(b->resolves, 1);
      });
      double f = double(std::max<uint64_t>(stats_frames_, 1));
      for (size_t i = 0; i < top.size() && i < 8; ++i) {
        const DestAgg& d = *top[i];
        REXGPU_INFO(
            "[carbon-gpu]   resolve dest {:08X} {}x{} fmt {} src pitch {} fmt {}: {:.2f}/frame, "
            "{:.2f}/frame overwritten before sampled",
            d.base, d.w, d.h, d.fmt, d.src_pitch, d.src_fmt, d.resolves / f, d.dead / f);
      }
    }
    dest_agg_.clear();
    frame_ms_hist_.clear();
    gpu_ms_hist_.clear();
    stats_ = Stats();
    spike_reports_ = 0;
    stats_frames_ = 0;
    stats_start_ = now;
  }
}

}  // namespace carbon::gpu
