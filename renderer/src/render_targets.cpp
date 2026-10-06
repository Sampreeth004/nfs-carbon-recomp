// Carbon native renderer: render targets and resolves.
//
// EDRAM is not emulated. Each (EDRAM base, pitch, format) the game renders to
// is a real Vulkan image; a resolve copies a rectangle of it into the image
// that stands for the destination texture, and optionally clears it.

#include <algorithm>
#include <cmath>

#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>

#include "renderer.h"
#include "shader_compiler.h"

namespace carbon::gpu {

namespace {

float HalfToFloat(uint16_t h) {
  uint32_t sign = uint32_t(h >> 15) << 31;
  uint32_t exp = (h >> 10) & 0x1F;
  uint32_t mant = h & 0x3FF;
  uint32_t bits;
  if (!exp) {
    if (!mant) {
      bits = sign;
    } else {
      exp = 127 - 15 + 1;
      while (!(mant & 0x400)) {
        mant <<= 1;
        --exp;
      }
      mant &= 0x3FF;
      bits = sign | (exp << 23) | (mant << 13);
    }
  } else {
    // Xenos half has no infinities/NaNs (extended range); treat 31 as normal.
    bits = sign | ((exp + 112) << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

float Float20e4ToFloat(uint32_t f24) {
  f24 &= 0xFFFFFF;
  if (!f24) return 0.0f;
  uint32_t mantissa = f24 & 0xFFFFF;
  uint32_t exponent = f24 >> 20;
  if (!exponent) {
    uint32_t shift = 0;
    while (!(mantissa & 0x100000)) {
      mantissa <<= 1;
      ++shift;
    }
    exponent = 1 - shift;
    mantissa &= 0xFFFFF;
  }
  uint32_t bits = ((exponent + 112) << 23) | (mantissa << 3);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

float Float7e3ToFloat(uint32_t v) {
  v &= 0x3FF;
  uint32_t exp = v >> 7, mant = v & 0x7F;
  if (!exp) {
    return std::ldexp(float(mant) / 128.0f, -2);  // denormal
  }
  return std::ldexp(1.0f + float(mant) / 128.0f, int(exp) - 3);
}

}  // namespace

VkFormat Renderer::ColorTargetFormat(xenos::ColorRenderTargetFormat f) const {
  using F = xenos::ColorRenderTargetFormat;
  switch (f) {
    case F::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case F::k_8_8_8_8_GAMMA:
      return VK_FORMAT_R8G8B8A8_SRGB;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case F::k_16_16:
    case F::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case F::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case F::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

VkFormat Renderer::DepthTargetFormat(xenos::DepthRenderTargetFormat f) const {
  static VkFormat d24s8 = VK_FORMAT_UNDEFINED;
  if (d24s8 == VK_FORMAT_UNDEFINED) {
    const auto& ifn = vulkan_device_->vulkan_instance()->functions();
    VkFormatProperties fp = {};
    ifn.vkGetPhysicalDeviceFormatProperties(vulkan_device_->physical_device(),
                                            VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    d24s8 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                ? VK_FORMAT_D24_UNORM_S8_UINT
                : VK_FORMAT_D32_SFLOAT_S8_UINT;
  }
  return f == xenos::DepthRenderTargetFormat::kD24FS8 ? VK_FORMAT_D32_SFLOAT_S8_UINT : d24s8;
}

bool Renderer::CreateRenderTargetImage(RenderTarget& rt, uint32_t width, uint32_t height) {
  const auto& dfn = vulkan_device_->functions();
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = rt.format;
  ici.extent = {width, height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
              VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              (rt.is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  VkFormat raw_format = rt.format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : rt.format;
  if (raw_format != rt.format) {
    ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_, &ici, &aci, &rt.image, &rt.allocation, nullptr) != VK_SUCCESS) {
    REXGPU_ERROR("[carbon-gpu] failed to create a {}x{} render target", width, height);
    return false;
  }
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = rt.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = rt.format;
  vci.subresourceRange = {rt.is_depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                           VK_IMAGE_ASPECT_STENCIL_BIT)
                                      : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                          0, 1, 0, 1};
  dfn.vkCreateImageView(vk_device_, &vci, nullptr, &rt.view);
  vci.format = raw_format;
  vci.subresourceRange.aspectMask =
      rt.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  dfn.vkCreateImageView(vk_device_, &vci, nullptr, &rt.sample_view);
  rt.width = width;
  rt.height = height;
  rt.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

RenderTarget* Renderer::GetRenderTarget(bool depth, uint32_t edram_base, uint32_t pitch,
                                        uint32_t format, uint32_t min_height) {
  uint64_t key = (uint64_t(depth) << 40) | (uint64_t(format & 0xF) << 32) |
                 (uint64_t(pitch & 0x3FFF) << 12) | (edram_base & 0xFFF);
  auto it = render_targets_.find(key);
  RenderTarget* rt = it != render_targets_.end() ? it->second.get() : nullptr;
  if (rt && rt->height >= min_height) {
    return rt;
  }
  uint32_t width = std::max<uint32_t>(pitch, 1);
  uint32_t height = AlignUp(std::max<uint32_t>(min_height, 16), 16);
  if (!rt) {
    auto created = std::make_unique<RenderTarget>();
    created->edram_base = edram_base;
    created->pitch = pitch;
    created->guest_format = format;
    created->is_depth = depth;
    created->format = depth ? DepthTargetFormat(xenos::DepthRenderTargetFormat(format))
                            : ColorTargetFormat(xenos::ColorRenderTargetFormat(format));
    if (!CreateRenderTargetImage(*created, width, height)) {
      return nullptr;
    }
    rt = created.get();
    render_targets_.emplace(key, std::move(created));
    REXGPU_DEBUG("[carbon-gpu] new {} render target base {} pitch {} format {} -> {}x{}",
                 depth ? "depth" : "color", edram_base, pitch, format, width, height);
    return rt;
  }
  // Grow: keep the old contents (copied into the top of the new image).
  height = std::max(height, rt->height + rt->height / 4);
  height = std::min<uint32_t>(AlignUp(height, 16), 8192);
  EndRendering();
  RenderTarget old = *rt;
  if (!CreateRenderTargetImage(*rt, width, height)) {
    *rt = old;
    return rt;
  }
  VkCommandBuffer cb = frame().cb;
  VkImageAspectFlags aspect =
      depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
  if (old.layout != VK_IMAGE_LAYOUT_UNDEFINED && pfn_copy_image_) {
    TransitionImage(cb, old.image, aspect, old.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    TransitionImage(cb, rt->image, aspect, rt->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region = {};
    region.srcSubresource = {aspect, 0, 0, 1};
    region.dstSubresource = {aspect, 0, 0, 1};
    region.extent = {old.width, std::min(old.height, height), 1};
    pfn_copy_image_(cb, old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  VkImage old_image = old.image;
  VmaAllocation old_alloc = old.allocation;
  VkImageView old_view = old.view, old_sample_view = old.sample_view;
  DeferDestroy([this, old_image, old_alloc, old_view, old_sample_view]() {
    const auto& dfn = vulkan_device_->functions();
    dfn.vkDestroyImageView(vk_device_, old_view, nullptr);
    dfn.vkDestroyImageView(vk_device_, old_sample_view, nullptr);
    vmaDestroyImage(allocator_, old_image, old_alloc);
  });
  return rt;
}

// ---------------------------------------------------------------------------
// Resolve pipelines: a full-screen triangle that copies texels from the
// source render target into the destination image (any format), applying the
// exponent bias and red/blue swap of the copy.
// ---------------------------------------------------------------------------

bool Renderer::CreateResolvePipelines() {
  const auto& dfn = vulkan_device_->functions();
  static const char* kVs = R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
  static const char* kPsColor = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D xe_source;
layout(push_constant) uniform XeResolve { ivec2 offset; float scale; uint swap_rb; } xe_r;
layout(location = 0) out vec4 xe_out;
void main() {
  vec4 v = texelFetch(xe_source, ivec2(gl_FragCoord.xy) + xe_r.offset, 0) * xe_r.scale;
  xe_out = xe_r.swap_rb != 0u ? v.bgra : v;
}
)";
  static const char* kPsDepth = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D xe_source;
layout(push_constant) uniform XeResolve { ivec2 offset; float scale; uint swap_rb; } xe_r;
layout(location = 0) out vec4 xe_out;
void main() {
  float d = texelFetch(xe_source, ivec2(gl_FragCoord.xy) + xe_r.offset, 0).r;
  xe_out = vec4(d, 0.0, 0.0, 1.0);
}
)";
  resolve_vs_ = CreateModuleFromGlsl(kVs, true, "resolve VS");
  resolve_ps_color_ = CreateModuleFromGlsl(kPsColor, false, "resolve PS");
  resolve_ps_depth_ = CreateModuleFromGlsl(kPsDepth, false, "resolve depth PS");
  if (!resolve_vs_ || !resolve_ps_color_ || !resolve_ps_depth_) {
    return false;
  }
  VkDescriptorSetLayoutBinding b = {};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 1;
  lci.pBindings = &b;
  dfn.vkCreateDescriptorSetLayout(vk_device_, &lci, nullptr, &resolve_set_layout_);
  VkPushConstantRange pc = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &resolve_set_layout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pc;
  dfn.vkCreatePipelineLayout(vk_device_, &plci, nullptr, &resolve_pipeline_layout_);

  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = VK_FILTER_NEAREST;
  sci.minFilter = VK_FILTER_NEAREST;
  sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  dfn.vkCreateSampler(vk_device_, &sci, nullptr, &point_sampler_);

  // Copies between images of different sizes, for render target growth.
  const auto& ifn = vulkan_device_->vulkan_instance()->functions();
  pfn_copy_image_ = reinterpret_cast<PFN_vkCmdCopyImage>(
      ifn.vkGetDeviceProcAddr(vk_device_, "vkCmdCopyImage"));
  return true;
}

VkPipeline Renderer::GetResolvePipeline(VkFormat dest_format, bool depth_source) {
  uint64_t key = (uint64_t(dest_format) << 1) | (depth_source ? 1 : 0);
  auto it = resolve_pipelines_.find(key);
  if (it != resolve_pipelines_.end()) {
    return it->second;
  }
  const auto& dfn = vulkan_device_->functions();
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_VERTEX_BIT, resolve_vs_, "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
               VK_SHADER_STAGE_FRAGMENT_BIT, depth_source ? resolve_ps_depth_ : resolve_ps_color_,
               "main", nullptr};
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
  VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &dest_format;
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
  ci.layout = resolve_pipeline_layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  dfn.vkCreateGraphicsPipelines(vk_device_, pipeline_cache_, 1, &ci, nullptr, &pipeline);
  resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
}

void Renderer::Resolve(const RegisterFile& regs) {
  using namespace rex::graphics;
  if (!BeginFrame()) {
    return;
  }
  ++stats_.resolves;
  auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  if (copy_control.copy_command != xenos::CopyCommand::kRaw &&
      copy_control.copy_command != xenos::CopyCommand::kConvert) {
    return;
  }
  // The resolve rectangle comes from the 3 vertices in vertex fetch 0.
  xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(0);
  if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 6) {
    return;
  }
  const uint32_t* vertices = memory_->TranslatePhysical<const uint32_t*>(fetch.address << 2);
  float half = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero
                   ? 0.5f
                   : 0.0f;
  int32_t fixed[6];
  for (int i = 0; i < 6; ++i) {
    uint32_t bits = GpuSwap32(vertices[i], fetch.endian);
    float f;
    std::memcpy(&f, &bits, 4);
    fixed[i] = int32_t(std::lround((f + half) * 256.0f));
  }
  int32_t x0 = std::min({fixed[0], fixed[2], fixed[4]});
  int32_t y0 = std::min({fixed[1], fixed[3], fixed[5]});
  int32_t x1 = std::max({fixed[0], fixed[2], fixed[4]});
  int32_t y1 = std::max({fixed[1], fixed[3], fixed[5]});
  x0 = (x0 + 127) >> 8;
  y0 = (y0 + 127) >> 8;
  x1 = (x1 + 127) >> 8;
  y1 = (y1 + 127) >> 8;
  auto window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {
    x0 += window_offset.window_x_offset;
    x1 += window_offset.window_x_offset;
    y0 += window_offset.window_y_offset;
    y1 += window_offset.window_y_offset;
  }
  {
    auto wtl = regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
    auto wbr = regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
    int32_t sx0 = int32_t(wtl.tl_x), sy0 = int32_t(wtl.tl_y);
    int32_t sx1 = int32_t(wbr.br_x), sy1 = int32_t(wbr.br_y);
    if (!wtl.window_offset_disable) {
      sx0 += window_offset.window_x_offset;
      sx1 += window_offset.window_x_offset;
      sy0 += window_offset.window_y_offset;
      sy1 += window_offset.window_y_offset;
    }
    auto stl = regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>();
    auto sbr = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();
    sx0 = std::max(std::max(sx0, int32_t(stl.tl_x)), 0);
    sy0 = std::max(std::max(sy0, int32_t(stl.tl_y)), 0);
    sx1 = std::min(sx1, int32_t(sbr.br_x));
    sy1 = std::min(sy1, int32_t(sbr.br_y));
    x0 = std::clamp(x0, sx0, std::max(sx0, sx1));
    y0 = std::clamp(y0, sy0, std::max(sy0, sy1));
    x1 = std::clamp(x1, sx0, std::max(sx0, sx1));
    y1 = std::clamp(y1, sy0, std::max(sy0, sy1));
  }
  x0 &= ~7;
  y0 &= ~7;
  x1 = (x1 + 7) & ~7;
  y1 = (y1 + 7) & ~7;
  auto surface = regs.Get<reg::RB_SURFACE_INFO>();
  int32_t pitch = int32_t(surface.surface_pitch);
  x1 = std::min(x1, pitch & ~7);
  x0 = std::min(x0, x1);
  if (x0 >= x1 || y0 >= y1) {
    return;
  }

  EndRendering();
  VkCommandBuffer cb = frame().cb;
  const auto& dfn = vulkan_device_->functions();
  bool is_depth = copy_control.copy_src_select >= 4;
  auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();

  // ---- Copy ----
  RenderTarget* src = nullptr;
  if (is_depth) {
    src = GetRenderTarget(true, depth_info.depth_base, uint32_t(pitch),
                          uint32_t(depth_info.depth_format), uint32_t(y1));
  } else {
    static constexpr uint32_t kColorInfoRegs[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    reg::RB_COLOR_INFO ci;
    ci.value = regs[kColorInfoRegs[copy_control.copy_src_select & 3]];
    src = GetRenderTarget(false, ci.color_base, uint32_t(pitch), uint32_t(ci.color_format),
                          uint32_t(y1));
  }
  auto dest_info = regs.Get<reg::RB_COPY_DEST_INFO>();
  auto dest_pitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
  uint32_t dest_base = regs[XE_GPU_REG_RB_COPY_DEST_BASE] & 0x1FFFFFFF;
  xenos::TextureFormat dest_format;
  if (is_depth) {
    dest_format = depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
                      ? xenos::TextureFormat::k_24_8_FLOAT
                      : xenos::TextureFormat::k_24_8;
  } else {
    dest_format = xenos::TextureFormat(uint32_t(dest_info.copy_dest_format));
  }
  if (tracing_) {
    REXGPU_WARN(
        "[carbon-gpu] trace: resolve src {} rect {},{}-{},{} pitch {} -> dest {:08X} pitch {} "
        "height {} fmt {} exp {} swap {} clear c{} d{} cmd {}",
        is_depth ? std::string("depth") : fmt::format("color{}", uint32_t(copy_control.copy_src_select)),
        x0, y0, x1, y1, pitch, regs[XE_GPU_REG_RB_COPY_DEST_BASE], uint32_t(dest_pitch.copy_dest_pitch),
        uint32_t(dest_pitch.copy_dest_height), uint32_t(dest_info.copy_dest_format),
        int32_t(dest_info.copy_dest_exp_bias), uint32_t(dest_info.copy_dest_swap),
        uint32_t(copy_control.color_clear_enable), uint32_t(copy_control.depth_clear_enable),
        uint32_t(copy_control.copy_command));
  }
  uint32_t dest_w = dest_pitch.copy_dest_pitch ? dest_pitch.copy_dest_pitch : uint32_t(x1);
  uint32_t dest_h = dest_pitch.copy_dest_height ? dest_pitch.copy_dest_height : uint32_t(y1);
  Texture* dest = nullptr;
  if (src && dest_base) {
    dest = GetOrCreateResolveTexture(dest_base, dest_w, dest_h, dest_format);
  }
  if (src && dest) {
    int32_t dx0 = std::min<int32_t>(x0, int32_t(dest->width));
    int32_t dy0 = std::min<int32_t>(y0, int32_t(dest->height));
    int32_t dx1 = std::min<int32_t>(x1, int32_t(dest->width));
    int32_t dy1 = std::min<int32_t>(y1, int32_t(dest->height));
    if (dx1 > dx0 && dy1 > dy0) {
      VkImageAspectFlags src_aspect =
          is_depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                   : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
      TransitionImage(cb, src->image, src_aspect, src->layout,
                      is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                               : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      TransitionImage(cb, dest->image, VK_IMAGE_ASPECT_COLOR_BIT, dest->layout,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
      VkImageView dest_view = GetTextureView(*dest, kSwizzleRgba, false, VK_IMAGE_VIEW_TYPE_2D);
      VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      att.imageView = dest_view;
      att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
      ri.renderArea = {{0, 0}, {dest->width, dest->height}};
      ri.layerCount = 1;
      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = &att;
      dfn.vkCmdBeginRendering(cb, &ri);
      VkPipeline pipeline = GetResolvePipeline(dest->format.host_format, is_depth);
      VkDescriptorSet set = AllocateDescriptorSet(resolve_set_layout_);
      VkDescriptorImageInfo ii = {point_sampler_, src->sample_view,
                                  is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      w.dstSet = set;
      w.dstBinding = 0;
      w.descriptorCount = 1;
      w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w.pImageInfo = &ii;
      dfn.vkUpdateDescriptorSets(vk_device_, 1, &w, 0, nullptr);
      dfn.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      dfn.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline_layout_,
                                  0, 1, &set, 0, nullptr);
      struct {
        int32_t offset[2];
        float scale;
        uint32_t swap_rb;
      } pc = {{0, 0},
              is_depth ? 1.0f : std::ldexp(1.0f, int(dest_info.copy_dest_exp_bias)),
              (!is_depth && dest_info.copy_dest_swap) ? 1u : 0u};
      dfn.vkCmdPushConstants(cb, resolve_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16,
                             &pc);
      VkViewport vp = {0.0f, 0.0f, float(dest->width), float(dest->height), 0.0f, 1.0f};
      VkRect2D sc = {{dx0, dy0}, {uint32_t(dx1 - dx0), uint32_t(dy1 - dy0)}};
      dfn.vkCmdSetViewport(cb, 0, 1, &vp);
      dfn.vkCmdSetScissor(cb, 0, 1, &sc);
      dfn.vkCmdDraw(cb, 3, 1, 0, 0);
      dfn.vkCmdEndRendering(cb);
      TransitionImage(cb, dest->image, VK_IMAGE_ASPECT_COLOR_BIT, dest->layout,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, 1);
      dest->resolved = true;
      dest->last_used_frame = frame_number_;
    }
  }

  // ---- Clears ----
  auto clear_target = [&](RenderTarget* rt, const VkClearAttachment& clear) {
    if (!rt) return;
    PassState pass;
    if (rt->is_depth) {
      pass.depth = rt;
    } else {
      pass.color[0] = rt;
    }
    pass.width = rt->width;
    pass.height = rt->height;
    BeginRendering(pass);
    int32_t cx1 = std::min<int32_t>(x1, int32_t(rt->width));
    int32_t cy1 = std::min<int32_t>(y1, int32_t(rt->height));
    if (cx1 > x0 && cy1 > y0) {
      VkClearRect rect = {{{x0, y0}, {uint32_t(cx1 - x0), uint32_t(cy1 - y0)}}, 0, 1};
      dfn.vkCmdClearAttachments(frame().cb, 1, &clear, 1, &rect);
    }
    EndRendering();
  };
  if (copy_control.depth_clear_enable) {
    RenderTarget* rt = GetRenderTarget(true, depth_info.depth_base, uint32_t(pitch),
                                       uint32_t(depth_info.depth_format), uint32_t(y1));
    uint32_t value = regs[XE_GPU_REG_RB_DEPTH_CLEAR];
    VkClearAttachment clear = {};
    clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    uint32_t d24 = (value >> 8) & 0xFFFFFF;
    clear.clearValue.depthStencil.depth =
        depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8
            ? std::min(Float20e4ToFloat(d24), 1.0f)
            : float(d24) / 16777215.0f;
    clear.clearValue.depthStencil.stencil = value & 0xFF;
    clear_target(rt, clear);
  }
  if (copy_control.color_clear_enable && !is_depth) {
    static constexpr uint32_t kColorInfoRegs[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    reg::RB_COLOR_INFO ci;
    ci.value = regs[kColorInfoRegs[copy_control.copy_src_select & 3]];
    RenderTarget* rt = GetRenderTarget(false, ci.color_base, uint32_t(pitch),
                                       uint32_t(ci.color_format), uint32_t(y1));
    uint64_t value = uint64_t(regs[XE_GPU_REG_RB_COLOR_CLEAR]) |
                     (uint64_t(regs[XE_GPU_REG_RB_COLOR_CLEAR_LO]) << 32);
    VkClearAttachment clear = {};
    clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clear.colorAttachment = 0;
    float* c = clear.clearValue.color.float32;
    using F = xenos::ColorRenderTargetFormat;
    switch (ci.color_format) {
      case F::k_8_8_8_8:
      case F::k_8_8_8_8_GAMMA:
        for (int j = 0; j < 4; ++j) c[j] = float((value >> (j * 8)) & 0xFF) / 255.0f;
        if (ci.color_format == F::k_8_8_8_8_GAMMA) {
          // The attachment is sRGB: clear with linear values.
          for (int j = 0; j < 3; ++j) {
            c[j] = c[j] <= 0.04045f ? c[j] / 12.92f : std::pow((c[j] + 0.055f) / 1.055f, 2.4f);
          }
        }
        break;
      case F::k_2_10_10_10:
      case F::k_2_10_10_10_AS_10_10_10_10:
        for (int j = 0; j < 3; ++j) c[j] = float((value >> (j * 10)) & 0x3FF) / 1023.0f;
        c[3] = float((value >> 30) & 3) / 3.0f;
        break;
      case F::k_2_10_10_10_FLOAT:
      case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        for (int j = 0; j < 3; ++j) c[j] = Float7e3ToFloat(uint32_t(value >> (j * 10)));
        c[3] = float((value >> 30) & 3) / 3.0f;
        break;
      case F::k_16_16:
      case F::k_16_16_16_16:
        for (int j = 0; j < 4; ++j) {
          c[j] = float(int16_t((value >> (j * 16)) & 0xFFFF)) * (32.0f / 32767.0f);
        }
        break;
      case F::k_16_16_FLOAT:
      case F::k_16_16_16_16_FLOAT:
        for (int j = 0; j < 4; ++j) c[j] = HalfToFloat(uint16_t(value >> (j * 16)));
        break;
      case F::k_32_FLOAT:
      case F::k_32_32_FLOAT: {
        uint32_t lo = uint32_t(value), hi = uint32_t(value >> 32);
        std::memcpy(&c[0], &lo, 4);
        std::memcpy(&c[1], &hi, 4);
      } break;
      default:
        break;
    }
    clear_target(rt, clear);
  }
}

}  // namespace carbon::gpu
