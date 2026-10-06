// Carbon native renderer: Vulkan backend core (frames, uploads, pipelines,
// draws, presentation).

#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <type_traits>

#include <rex/cvar.h>
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
REXCVAR_DEFINE_INT32(carbon_gpu_trace_frame, 0, "CarbonGPU",
                     "Log every pass, draw and resolve of this frame number (0 = off)");
REXCVAR_DEFINE_BOOL(carbon_gpu_dump_shaders, false, "CarbonGPU",
                    "Write the generated GLSL of every translated shader to <cache>/carbon_gpu/glsl");

namespace carbon::gpu {

using rex::ui::vulkan::VulkanDevice;

uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
  return XXH3_64bits_withSeed(data, size, seed);
}

namespace {

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::filesystem::path g_glsl_dump_dir;

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
    VkDescriptorSetLayoutBinding b[4] = {};
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
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 4;
    ci.pBindings = b;
    if (dfn.vkCreateDescriptorSetLayout(vk_device_, &ci, nullptr, &set0_layout_) != VK_SUCCESS) {
      return false;
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * 256},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 256;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = sizes;
    if (dfn.vkCreateDescriptorPool(vk_device_, &pci, nullptr, &set0_pool_) != VK_SUCCESS) {
      return false;
    }
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
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    dfn.vkCreatePipelineCache(vk_device_, &ci, nullptr, &pipeline_cache_);
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
  std::ofstream out(pipeline_cache_path_, std::ios::binary);
  out.write(data.data(), std::streamsize(size));
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
    dfn.vkWaitForFences(vk_device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(vk_device_, 1, &f.fence);
    f.submitted = false;
  }
  for (auto& fn : f.deferred_destroy) fn();
  f.deferred_destroy.clear();
  for (uint32_t c : f.chunks) {
    free_chunks_.push_back(c);
  }
  f.chunks.clear();
  dfn.vkResetDescriptorPool(vk_device_, f.descriptor_pool, 0);
  dfn.vkResetCommandPool(vk_device_, f.pool, 0);
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(f.upload_cb, &bi);
  dfn.vkBeginCommandBuffer(f.cb, &bi);
  f.number = frame_number_;
  frame_open_ = true;
  tracing_ = REXCVAR_GET(carbon_gpu_trace_frame) > 0 &&
             frame_number_ == uint64_t(REXCVAR_GET(carbon_gpu_trace_frame));
  if (tracing_) {
    REXGPU_WARN("[carbon-gpu] ---- trace of frame {} ----", frame_number_);
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
    VkDescriptorBufferInfo bufs[4] = {{chunk.buffer, 0, 4096},
                                      {chunk.buffer, 0, 4096},
                                      {chunk.buffer, 0, sizeof(DrawConstants)},
                                      {chunk.buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w[4] = {};
    for (uint32_t i = 0; i < 4; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = chunk.set0;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType =
          i < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[i].pBufferInfo = &bufs[i];
    }
    dfn.vkUpdateDescriptorSets(vk_device_, 4, w, 0, nullptr);
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
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = layout;
  b.newLayout = new_layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, levels, 0, layers};
  dfn.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  layout = new_layout;
}

// ---------------------------------------------------------------------------
// Rendering instances
// ---------------------------------------------------------------------------

void Renderer::EndRendering() {
  if (!rendering_) {
    return;
  }
  vulkan_device_->functions().vkCmdEndRendering(frame().cb);
  rendering_ = false;
  current_pass_ = PassState();
}

bool Renderer::BeginRendering(const PassState& pass) {
  const auto& dfn = vulkan_device_->functions();
  VkCommandBuffer cb = frame().cb;
  VkRenderingAttachmentInfo color[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    color[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color[i].imageView = VK_NULL_HANDLE;
    color[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (RenderTarget* rt = pass.color[i]) {
      TransitionImage(cb, rt->image, VK_IMAGE_ASPECT_COLOR_BIT, rt->layout,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
      color[i].imageView = rt->view;
      rt->last_used_frame = frame_number_;
    }
  }
  VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  if (pass.depth) {
    TransitionImage(cb, pass.depth->image,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, pass.depth->layout,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    depth.imageView = pass.depth->view;
    pass.depth->last_used_frame = frame_number_;
  }
  VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {pass.width, pass.height}};
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
  return module;
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
  auto it = pipelines_.find(hash);
  if (it != pipelines_.end()) {
    return it->second;
  }
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
  ci.layout = GetPipelineLayout(key.vs_textures, key.ps_textures);
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkResult r = dfn.vkCreateGraphicsPipelines(vk_device_, pipeline_cache_, 1, &ci, nullptr, &pipeline);
  if (r != VK_SUCCESS) {
    REXGPU_ERROR("[carbon-gpu] vkCreateGraphicsPipelines failed: {}", int(r));
    pipeline = VK_NULL_HANDLE;
  }
  ++stats_.pipelines_created;
  pipelines_.emplace(hash, pipeline);
  if ((stats_.pipelines_created & 63) == 0) {
    SavePipelineCache();
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
  scissor = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}

bool Renderer::SetupVertexData(const RegisterFile& regs, Shader* vs, DrawConstants& c) {
  const TranslatedShader& t = vs->translated;
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

  // Render targets.
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface.surface_pitch;
  if (!pitch) {
    Skip(kSkipNoPitch);
    return;
  }
  // Height needed: the bottom of the scissor / viewport.
  uint32_t needed_height;
  {
    auto wbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
    needed_height = std::min<uint32_t>(wbr.br_y, uint32_t(std::max<int32_t>(sbr.br_y, 0)));
    auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
    if (vte.vport_y_scale_ena) {
      float sy = std::fabs(regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_YSCALE));
      float oy = vte.vport_y_offset_ena ? regs.GetFloat(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
      float bottom = oy + sy;
      if (bottom > 0.0f && bottom < 8192.0f) {
        needed_height = std::max(needed_height, std::min(uint32_t(std::ceil(bottom)),
                                                         std::max<uint32_t>(wbr.br_y, 1)));
      }
    }
    needed_height = std::clamp<uint32_t>(needed_height, 1, 8192);
  }
  PassState pass;
  uint32_t color_write_masks[4] = {};
  uint32_t ps_colors = ps ? ps->translated.colors_written : 0;
  auto color_mask = regs[XE_GPU_REG_RB_COLOR_MASK];
  if (edram_mode == xenos::EdramMode::kColorDepth) {
    for (uint32_t i = 0; i < 4; ++i) {
      uint32_t mask = (color_mask >> (i * 4)) & 0xF;
      if (!mask || !(ps_colors & (1u << i))) {
        continue;
      }
      reg::RB_COLOR_INFO ci;
      ci.value = regs[kColorInfoRegs[i]];
      pass.color[i] = GetRenderTarget(false, ci.color_base, pitch, uint32_t(ci.color_format),
                                      needed_height);
      color_write_masks[i] = mask;
    }
  }
  auto depth_control = regs.Get<reg::RB_DEPTHCONTROL>();
  bool depth_used = depth_control.z_enable || depth_control.stencil_enable;
  if (depth_used) {
    auto di = regs.Get<reg::RB_DEPTH_INFO>();
    pass.depth = GetRenderTarget(true, di.depth_base, pitch, uint32_t(di.depth_format),
                                 needed_height);
  }
  bool any_target = pass.depth != nullptr;
  for (auto* c : pass.color) any_target |= c != nullptr;
  if (!any_target) {
    Skip(kSkipNoTarget);
    return;
  }
  pass.width = UINT32_MAX;
  pass.height = UINT32_MAX;
  for (auto* c : pass.color) {
    if (c) {
      pass.width = std::min(pass.width, c->width);
      pass.height = std::min(pass.height, c->height);
    }
  }
  if (pass.depth) {
    pass.width = std::min(pass.width, pass.depth->width);
    pass.height = std::min(pass.height, pass.depth->height);
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
    ps_variant.flat_mask = regs.Get<reg::SQ_INTERPOLATOR_CNTL>().param_shade &
                           ((1u << interpolators) - 1);
    ps_variant.param_gen = program_cntl.param_gen != 0;
    ps_variant.param_gen_index = ps_variant.param_gen
                                     ? regs.Get<reg::SQ_CONTEXT_MISC>().param_gen_pos
                                     : 0;
    ps_module = GetPixelShaderModule(ps, ps_variant);
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
  DrawConstants consts;
  std::memset(&consts, 0, sizeof(consts));
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
  for (uint32_t i = 0; i < 32; ++i) {
    consts.tex_size[i * 4 + 3] = 1.0f;
    consts.tex_uv[i * 4 + 0] = 1.0f;
    consts.tex_uv[i * 4 + 1] = 1.0f;
  }
  VkViewport viewport;
  VkRect2D scissor;
  ComputeViewport(regs, pass.width, pass.height, consts, viewport, scissor);
  if (!scissor.extent.width || !scissor.extent.height) {
    Skip(kSkipScissor);
    return;
  }

  // Estimate the upload size so the draw fits in one chunk.
  VkDeviceSize estimate = 4096 * 2 + sizeof(DrawConstants) + 3 * 256;
  for (uint32_t i = 0; i < 96; ++i) {
    if (vs->translated.vfetch_used[i >> 5] & (1u << (i & 31))) {
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

  if (!SetupVertexData(regs, vs, consts)) {
    // Chunk switched mid-way; retry once in the fresh chunk.
    if (!SetupVertexData(regs, vs, consts)) {
      Skip(kSkipVertices);
      return;
    }
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
  if (expand == Expand::kRectList) {
    uint32_t rects = count / 3;
    host_count = rects * 6;
    if (info.indexed) {
      UploadAllocation a = Upload(VkDeviceSize(rects) * 3 * 4 + 16, 16);
      uint32_t* dst = reinterpret_cast<uint32_t*>(a.ptr);
      for (uint32_t i = 0; i < rects * 3; ++i) dst[i] = read_index(i);
      consts.vtx2[0] = uint32_t(a.offset / 4);
    }
  } else if (expand != Expand::kNone) {
    std::vector<uint32_t> out;
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
      continue;
    }
    UploadAllocation a = Upload(4096, ubo_alignment_);
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
    std::memcpy(a.ptr, &consts, sizeof(consts));
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
  dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkPipelineLayout layout = GetPipelineLayout(key.vs_textures, key.ps_textures);
  VkDescriptorSet set0 = upload_chunks_[current_chunk_].set0;
  dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set0, 3,
                              dyn_offsets);
  if (vs_set) {
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1, &vs_set, 0,
                                nullptr);
  }
  if (ps_set) {
    dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 2, 1, &ps_set, 0,
                                nullptr);
  }
  dfn.vkCmdSetViewport(cb, 0, 1, &viewport);
  dfn.vkCmdSetScissor(cb, 0, 1, &scissor);
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
    dfn.vkCmdSetDepthBias(cb, offset, 0.0f, scale);
  }
  {
    float blend[4] = {regs.GetFloat(XE_GPU_REG_RB_BLEND_RED), regs.GetFloat(XE_GPU_REG_RB_BLEND_GREEN),
                      regs.GetFloat(XE_GPU_REG_RB_BLEND_BLUE), regs.GetFloat(XE_GPU_REG_RB_BLEND_ALPHA)};
    dfn.vkCmdSetBlendConstants(cb, blend);
  }
  {
    auto ref = regs.Get<reg::RB_STENCILREFMASK>();
    reg::RB_STENCILREFMASK ref_bf;
    ref_bf.value = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
    if (!depth_control.backface_enable) ref_bf = ref;
    dfn.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, ref.stencilref);
    dfn.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, ref_bf.stencilref);
    dfn.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, ref.stencilmask);
    dfn.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, ref_bf.stencilmask);
    dfn.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, ref.stencilwritemask);
    dfn.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, ref_bf.stencilwritemask);
  }
  if (host_indexed) {
    dfn.vkCmdBindIndexBuffer(cb, index_buffer, index_offset, index_type);
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
layout(push_constant) uniform XePresent { vec2 scale; uint use_gamma; uint pad; } xe_p;
layout(location = 0) out vec4 xe_out;
void main() {
  vec4 c = texture(xe_source, gl_FragCoord.xy * xe_p.scale);
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
                             uint32_t source_height, VkImage dest, VkImageView dest_view,
                             uint32_t width, uint32_t height, bool dest_written_before) {
  const auto& dfn = vulkan_device_->functions();
  VkImageLayout dest_layout = dest_written_before
                                  ? rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout
                                  : VK_IMAGE_LAYOUT_UNDEFINED;
  TransitionImage(cb, dest, VK_IMAGE_ASPECT_COLOR_BIT, dest_layout,
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
      uint32_t pad;
    } pc = {{1.0f / float(source_width), 1.0f / float(source_height)}, 1, 0};
    // The guest output may be smaller than the source image (padded resolves).
    pc.scale[0] = float(width) / float(source_width) / float(width);
    pc.scale[1] = float(height) / float(source_height) / float(height);
    dfn.vkCmdPushConstants(cb, present_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    VkViewport vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {width, height}};
    dfn.vkCmdSetViewport(cb, 0, 1, &vp);
    dfn.vkCmdSetScissor(cb, 0, 1, &sc);
    dfn.vkCmdDraw(cb, 3, 1, 0, 0);
  }
  dfn.vkCmdEndRendering(cb);
  TransitionImage(cb, dest, VK_IMAGE_ASPECT_COLOR_BIT, dest_layout,
                  rex::ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
}

void Renderer::Swap(const RegisterFile& regs, uint32_t frontbuffer_ptr, uint32_t width,
                    uint32_t height, const GammaRamp& gamma) {
  if (!BeginFrame()) {
    return;
  }
  EndRendering();
  std::memcpy(gamma_mapped_ + frame_index_ * 256, gamma.table, 1024);

  rex::system::X_VIDEO_MODE video_mode;
  rex::kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_w = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_h = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  // The front buffer is normally the destination of the last resolve.
  Texture* front = FindResolvedTexture(frontbuffer_ptr & 0x1FFFFFFF, width, height);
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
  uint32_t out_w = width ? width : source_w;
  uint32_t out_h = height ? height : source_h;

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
            RecordPresent(cb, source_view, source_w, source_h, vk_context.image(),
                          vk_context.image_view(), out_w, out_h,
                          vk_context.image_ever_written_previously());
          });
          submitted = true;
          return true;
        });
  }
  if (!submitted) {
    EndFrame(false, nullptr);
  }

  // Statistics.
  ++stats_frames_;
  double now = NowSeconds();
  if (now - stats_start_ >= 5.0) {
    if (REXCVAR_GET(carbon_gpu_stats)) {
      double secs = now - stats_start_;
      double f = double(std::max<uint64_t>(stats_frames_, 1));
      REXGPU_INFO(
          "[carbon-gpu] {:.1f} fps | per frame: draws {:.0f} (skipped {:.0f}), passes {:.1f}, "
          "resolves {:.1f}, upload {:.2f} MiB | new: pipelines {}, shaders {}, textures {}",
          f / secs, stats_.draws / f, stats_.draws_skipped / f, stats_.passes / f,
          stats_.resolves / f, double(stats_.upload_bytes) / f / (1024.0 * 1024.0),
          stats_.pipelines_created, stats_.shaders_compiled, stats_.textures_uploaded);
    }
    if (REXCVAR_GET(carbon_gpu_stats) && stats_.draws_skipped) {
      static const char* kNames[kSkipReasonCount] = {
          "no-vs", "edram-mode", "ps-failed", "memexport", "primitive", "no-pitch", "no-target",
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
    stats_ = Stats();
    stats_frames_ = 0;
    stats_start_ = now;
  }
}

}  // namespace carbon::gpu
