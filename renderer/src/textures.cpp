// Carbon native renderer: texture cache, samplers and texture descriptors.
//
// Guest textures are decoded on the CPU into host images and re-uploaded when
// the fingerprint of their guest memory changes (checked less often the
// longer a texture stays unchanged). Textures written by resolves live only
// on the GPU and are found by their guest address.

#include <algorithm>
#include <chrono>
#include <cmath>

#include <rex/ui/vulkan/device.h>

#include "renderer.h"

namespace carbon::gpu {

namespace {

constexpr uint32_t kMaxHashInterval = 32;

VkComponentSwizzle SwizzleComponent(uint32_t s) {
  switch (s) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
  }
}

VkSamplerAddressMode ToAddressMode(xenos::ClampMode m) {
  switch (m) {
    case xenos::ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xenos::ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
      return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case xenos::ClampMode::kClampToBorder:
    case xenos::ClampMode::kMirrorClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

uint64_t HashExtent(const uint8_t* mem, const GuestTextureExtent& e) {
  uint64_t h = 0;
  if (e.base_size) {
    h = HashBytes(mem + (e.base_start & 0x1FFFFFFF),
                  std::min<uint32_t>(e.base_size, 0x20000000 - (e.base_start & 0x1FFFFFFF)), h);
  }
  if (e.mip_size) {
    h = HashBytes(mem + (e.mip_start & 0x1FFFFFFF),
                  std::min<uint32_t>(e.mip_size, 0x20000000 - (e.mip_start & 0x1FFFFFFF)), h);
  }
  return h;
}

}  // namespace

void Renderer::CreateNullTextures() {
  const auto& dfn = vulkan_device_->functions();
  auto make = [&](NullTexture& n, VkImageType type, VkImageViewType view_type, uint32_t layers,
                  VkImageCreateFlags flags) {
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = flags;
    ici.imageType = type;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {1, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vmaCreateImage(allocator_, &ici, &aci, &n.image, &n.allocation, nullptr);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = n.image;
    vci.viewType = view_type;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    dfn.vkCreateImageView(vk_device_, &vci, nullptr, &n.view);
  };
  make(null_2d_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1, 0);
  make(null_3d_, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1, 0);
  make(null_2d_array_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 0);
  make(null_cube_, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);

  // Clear them once, synchronously.
  Frame& f = frames_[0];
  VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  dfn.vkBeginCommandBuffer(f.cb, &bi);
  for (NullTexture* n : {&null_2d_, &null_3d_, &null_2d_array_, &null_cube_}) {
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t layers = n == &null_cube_ ? 6 : 1;
    TransitionImage(f.cb, n->image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, layers);
    VkClearColorValue black = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    dfn.vkCmdClearColorImage(f.cb, n->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                             &range);
    TransitionImage(f.cb, n->image, VK_IMAGE_ASPECT_COLOR_BIT, layout,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1, layers);
  }
  dfn.vkEndCommandBuffer(f.cb);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &f.cb;
  {
    auto queue = vulkan_device_->AcquireQueue(vulkan_device_->queue_family_graphics_compute(), 0);
    dfn.vkQueueSubmit(queue.queue(), 1, &si, f.fence);
  }
  dfn.vkWaitForFences(vk_device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
  dfn.vkResetFences(vk_device_, 1, &f.fence);
  dfn.vkResetCommandPool(vk_device_, f.pool, 0);
}

bool Renderer::CreateTextureImage(Texture& t, VkImageUsageFlags extra_usage) {
  const GuestTexture& g = t.guest;
  const bool is_3d = g.dimension == xenos::DataDimension::k3D;
  const bool is_cube = g.dimension == xenos::DataDimension::kCube;
  t.width = std::max<uint32_t>(g.width >> g.mip_min_level, 1);
  t.height = std::max<uint32_t>(g.height >> g.mip_min_level, 1);
  t.depth = is_3d ? std::max<uint32_t>(g.depth >> g.mip_min_level, 1) : 1;
  t.layers = is_3d ? 1 : g.depth;
  t.levels = g.mip_max_level - g.mip_min_level + 1;
  if (t.res_scale != 1.0f) {
    t.width = std::max(1u, uint32_t(std::ceil(float(t.width) * t.res_scale)));
    t.height = std::max(1u, uint32_t(std::ceil(float(t.height) * t.res_scale)));
  }
  VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  ici.format = t.format.host_format;
  ici.extent = {t.width, t.height, t.depth};
  ici.mipLevels = t.levels;
  ici.arrayLayers = t.layers;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | extra_usage;
  if (is_cube) {
    ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  }
  if (t.format.host_format_signed != VK_FORMAT_UNDEFINED) {
    ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  }
  VmaAllocationCreateInfo aci = {};
  aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  if (vmaCreateImage(allocator_, &ici, &aci, &t.image, &t.allocation, nullptr) != VK_SUCCESS) {
    REXGPU_ERROR("[carbon-gpu] failed to create a {}x{}x{} texture (format {})", t.width,
                 t.height, t.depth, uint32_t(g.format));
    return false;
  }
  {
    uint64_t texels = uint64_t(t.width) * t.height * t.depth * t.layers;
    uint64_t blocks = texels / (uint64_t(t.format.host_block_width) * t.format.host_block_height);
    t.memory_bytes = blocks * t.format.host_bytes_per_block;
    if (t.levels > 1) t.memory_bytes += t.memory_bytes / 3;
    texture_bytes_ += t.memory_bytes;
  }
  t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  t.view_type = is_3d ? VK_IMAGE_VIEW_TYPE_3D
                      : (is_cube ? VK_IMAGE_VIEW_TYPE_CUBE
                                 : (t.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                 : VK_IMAGE_VIEW_TYPE_2D));
  return true;
}

void Renderer::UploadTexture(Texture& t) {
  auto now = []() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  const double start = now();
  struct Done {
    Renderer* self;
    double start;
    double (*clock)();
    ~Done() {
      self->frame_texture_s_ += clock() - start;
      ++self->frame_textures_;
    }
  } done{this, start, +now};
  std::vector<HostTextureRegion> regions;
  uint64_t size = GetHostTextureSize(t.guest, t.format, regions);
  if (!size || regions.empty()) {
    return;
  }
  const auto& dfn = vulkan_device_->functions();
  uint8_t* dst;
  VkBuffer buffer;
  VkDeviceSize base_offset;
  if (size <= chunk_size_ / 2) {
    UploadAllocation a = Upload(size, 16);
    dst = a.ptr;
    buffer = a.buffer;
    base_offset = a.offset;
  } else {
    // Too big for the upload ring: a dedicated staging buffer.
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocation allocation;
    VmaAllocationInfo info;
    if (vmaCreateBuffer(allocator_, &bci, &aci, &buffer, &allocation, &info) != VK_SUCCESS) {
      return;
    }
    dst = static_cast<uint8_t*>(info.pMappedData);
    base_offset = 0;
    DeferDestroy([this, buffer, allocation]() { vmaDestroyBuffer(allocator_, buffer, allocation); });
  }
  if (t.format.conversion == TextureConversion::kUnsupported) {
    std::memset(dst, 0, size_t(size));
  } else {
    DecodeGuestTexture(physical_base_, t.guest, t.format, regions, dst);
  }
  std::vector<VkBufferImageCopy> copies;
  copies.reserve(regions.size());
  for (const HostTextureRegion& r : regions) {
    VkBufferImageCopy c = {};
    c.bufferOffset = base_offset + r.buffer_offset;
    c.bufferRowLength = r.row_length_texels;
    c.bufferImageHeight = t.format.host_block_height > 1
                              ? AlignUp(r.height, t.format.host_block_height)
                              : r.height;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, r.level,
                          t.guest.dimension == xenos::DataDimension::k3D ? 0 : r.layer, 1};
    c.imageExtent = {r.width, r.height, r.depth};
    copies.push_back(c);
  }
  VkCommandBuffer cb = frame().upload_cb;
  TransitionImage(cb, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  dfn.vkCmdCopyBufferToImage(cb, buffer, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             uint32_t(copies.size()), copies.data());
  TransitionImage(cb, t.image, VK_IMAGE_ASPECT_COLOR_BIT, t.layout,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  ++stats_.textures_uploaded;
}

Texture* Renderer::FindResolvedTexture(uint32_t base_address, uint32_t width, uint32_t height) {
  auto range = textures_.equal_range(base_address & 0x1FFFFFFF);
  Texture* best = nullptr;
  for (auto it = range.first; it != range.second; ++it) {
    Texture* t = it->second.get();
    if (!t->resolved) continue;
    if (width && (t->guest.width < width || t->guest.height < height)) continue;
    if (!best || t->last_used_frame > best->last_used_frame) {
      best = t;
    }
  }
  return best;
}

Texture* Renderer::GetOrCreateResolveTexture(uint32_t base_address, uint32_t width,
                                             uint32_t height, xenos::TextureFormat format,
                                             uint32_t& y_offset) {
  base_address &= 0x1FFFFFFF;
  y_offset = 0;
  // Predicated tiling resolves each screen tile to the address of its first
  // row inside one texture. Tiled textures store 32-row strips contiguously,
  // so a strip boundary is at row * aligned pitch * bytes per texel.
  uint32_t bytes_per_texel = GetTextureFormatInfo(format, true).bytes_per_block;
  uint32_t strip_bytes = AlignUp(width, 32u) * 32 * bytes_per_texel;
  for (Texture* t : resolved_textures_) {
    uint32_t parent = t->guest.base_address;
    if (parent >= base_address || t->guest.format != format || t->guest.width != width) {
      continue;
    }
    uint32_t delta = base_address - parent;
    if (delta % strip_bytes) {
      continue;
    }
    // Inside the parent (a texture placed right after it starts at or past
    // its 32-aligned height). Depth tiles keep the full height in the copy
    // registers, color tiles the remaining height; the copy is clamped anyway.
    uint32_t rows = delta / strip_bytes * 32;
    if (rows < t->guest.height) {
      y_offset = rows;
      t->last_used_frame = frame_number_;
      return t;
    }
  }
  auto range = textures_.equal_range(base_address);
  for (auto it = range.first; it != range.second; ++it) {
    Texture* t = it->second.get();
    if (t->resolved && t->guest.format == format && t->guest.width == width &&
        t->guest.height == height) {
      return t;
    }
  }
  auto t = std::make_unique<Texture>();
  t->guest.base_address = base_address;
  t->guest.format = format;
  t->guest.dimension = xenos::DataDimension::k2DOrStacked;
  t->guest.width = width;
  t->guest.height = height;
  t->guest.depth = 1;
  t->guest.pitch_texels = width;
  t->guest.tiled = true;
  t->format = GetTextureFormatInfo(format, bc_supported_);
  if (t->format.host_block_width > 1 || t->format.conversion == TextureConversion::kUnsupported) {
    // Not a color format a render target can write; resolve into RGBA8.
    t->format = GetTextureFormatInfo(xenos::TextureFormat::k_8_8_8_8, bc_supported_);
  }
  t->format.host_format_signed = VK_FORMAT_UNDEFINED;
  t->res_scale = res_scale_;
  if (!CreateTextureImage(*t, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
    return nullptr;
  }
  t->resolved = true;
  t->last_used_frame = frame_number_;
  Texture* raw = t.get();
  textures_.emplace(base_address, std::move(t));
  resolved_textures_.push_back(raw);
  return raw;
}

Texture* Renderer::GetTexture(const GuestTexture& guest, bool allow_upload) {
  uint32_t key = (guest.base_address ? guest.base_address : guest.mip_address) & 0x1FFFFFFF;
  // Resolved images at this address take priority: their data never reaches
  // guest memory.
  if (guest.base_address && guest.dimension == xenos::DataDimension::k2DOrStacked &&
      guest.depth == 1) {
    if (Texture* r = FindResolvedTexture(guest.base_address, 0, 0)) {
      r->last_used_frame = frame_number_;
      return r;
    }
  }
  auto range = textures_.equal_range(key);
  Texture* t = nullptr;
  for (auto it = range.first; it != range.second; ++it) {
    if (!it->second->resolved && it->second->guest == guest) {
      t = it->second.get();
      break;
    }
  }
  if (!t) {
    if (!allow_upload) {
      return nullptr;
    }
    auto created = std::make_unique<Texture>();
    created->guest = guest;
    created->format = GetTextureFormatInfo(guest.format, bc_supported_);
    if (created->format.conversion == TextureConversion::kUnsupported) {
      static uint32_t logged = 0;
      if (logged < 16) {
        ++logged;
        REXGPU_WARN("[carbon-gpu] unsupported texture format {}", uint32_t(guest.format));
      }
    }
    if (!CreateTextureImage(*created, 0)) {
      return nullptr;
    }
    created->extent = GetGuestTextureExtent(guest);
    created->content_hash = HashExtent(physical_base_, created->extent);
    created->last_hash_frame = frame_number_;
    UploadTexture(*created);
    t = created.get();
    textures_.emplace(key, std::move(created));
  } else if (frame_number_ - t->last_hash_frame >= t->hash_interval &&
             t->last_used_frame != frame_number_) {
    // Re-check the guest memory: textures that stay the same are checked
    // less and less often.
    uint64_t hash = HashExtent(physical_base_, t->extent);
    t->last_hash_frame = frame_number_;
    if (hash != t->content_hash) {
      t->content_hash = hash;
      t->hash_interval = 1;
      UploadTexture(*t);
    } else {
      t->hash_interval = std::min(t->hash_interval * 2, kMaxHashInterval);
    }
  }
  t->last_used_frame = frame_number_;
  return t;
}

VkImageView Renderer::GetTextureView(Texture& t, uint32_t swizzle, bool use_signed,
                                     VkImageViewType view_type) {
  uint64_t key = uint64_t(swizzle & 0xFFF) | (uint64_t(use_signed) << 12) |
                 (uint64_t(view_type) << 16);
  auto it = t.views.find(key);
  if (it != t.views.end()) {
    return it->second;
  }
  VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t.image;
  vci.viewType = view_type;
  vci.format = use_signed && t.format.host_format_signed != VK_FORMAT_UNDEFINED
                   ? t.format.host_format_signed
                   : t.format.host_format;
  vci.components = {SwizzleComponent(swizzle & 7), SwizzleComponent((swizzle >> 3) & 7),
                    SwizzleComponent((swizzle >> 6) & 7), SwizzleComponent((swizzle >> 9) & 7)};
  uint32_t layers = 1;
  if (view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) layers = t.layers;
  if (view_type == VK_IMAGE_VIEW_TYPE_CUBE) layers = 6;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0, layers};
  VkImageView view = VK_NULL_HANDLE;
  vulkan_device_->functions().vkCreateImageView(vk_device_, &vci, nullptr, &view);
  t.views.emplace(key, view);
  return view;
}

VkSampler Renderer::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                               const TextureBinding& binding, bool render_source) {
  auto pick = [](xenos::TextureFilter instr, xenos::TextureFilter fetch_value) {
    return instr == xenos::TextureFilter::kUseFetchConst ? fetch_value : instr;
  };
  xenos::TextureFilter mag = pick(binding.mag_filter, fetch.mag_filter);
  xenos::TextureFilter min = pick(binding.min_filter, fetch.min_filter);
  xenos::TextureFilter mip = pick(binding.mip_filter, fetch.mip_filter);
  xenos::AnisoFilter aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                 ? fetch.aniso_filter
                                 : binding.aniso_filter;
  uint32_t lod_bias = uint32_t(fetch.lod_bias) & 0x3FF;
  uint64_t key = uint64_t(fetch.clamp_x) | (uint64_t(fetch.clamp_y) << 3) |
                 (uint64_t(fetch.clamp_z) << 6) | (uint64_t(mag) << 9) | (uint64_t(min) << 11) |
                 (uint64_t(mip) << 13) | (uint64_t(aniso) << 15) |
                 (uint64_t(fetch.border_color) << 18) | (uint64_t(lod_bias) << 20) |
                 (uint64_t(fetch.mip_min_level) << 30) | (uint64_t(fetch.mip_max_level) << 34) |
                 (uint64_t(render_source) << 40);
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }
  const auto& props = vulkan_device_->properties();
  VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = mag == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sci.minFilter = min == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sci.mipmapMode = mip == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  auto address = [&](xenos::ClampMode m) {
    VkSamplerAddressMode a = ToAddressMode(m);
    if (a == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !props.samplerMirrorClampToEdge) {
      a = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
    return a;
  };
  sci.addressModeU = address(fetch.clamp_x);
  sci.addressModeV = address(fetch.clamp_y);
  sci.addressModeW = address(fetch.clamp_z);
  sci.mipLodBias = float(int32_t(lod_bias << 22) >> 22) * (1.0f / 32.0f);
  // Render-to-texture sources (post-process, blurs) are never worth anisotropic
  // taps; other textures are capped for the GPU's sake.
  if (!render_source && aniso != xenos::AnisoFilter::kDisabled && props.samplerAnisotropy &&
      max_anisotropy_ > 1.0f) {
    float max_aniso = float(1u << (uint32_t(aniso) - 1));
    max_aniso = std::min({max_aniso, props.maxSamplerAnisotropy, max_anisotropy_});
    if (max_aniso > 1.0f) {
      sci.anisotropyEnable = VK_TRUE;
      sci.maxAnisotropy = max_aniso;
    }
  }
  sci.minLod = 0.0f;
  sci.maxLod = mip == xenos::TextureFilter::kBaseMap ? 0.0f : 16.0f;
  sci.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White
                        ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                        : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler sampler = VK_NULL_HANDLE;
  vulkan_device_->functions().vkCreateSampler(vk_device_, &sci, nullptr, &sampler);
  samplers_.emplace(key, sampler);
  return sampler;
}

void Renderer::BindTextures(const RegisterFile& regs, Shader* shader, uint32_t set_index,
                            DrawConstants& c, VkDescriptorSet& set_out) {
  (void)set_index;
  set_out = VK_NULL_HANDLE;
  const TranslatedShader& ts = shader->translated;
  if (!ts.texture_descriptor_count) {
    return;
  }
  constexpr uint32_t kMaxDescriptors = 64;
  const uint32_t descriptor_count = std::min(ts.texture_descriptor_count, kMaxDescriptors);
  VkDescriptorImageInfo infos[kMaxDescriptors];
  std::memset(infos, 0, sizeof(VkDescriptorImageInfo) * descriptor_count);
  for (const TextureBinding& b : ts.textures) {
    xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(b.fetch_index);
    GuestTexture g;
    Texture* t = nullptr;
    if (GuestTextureFromFetch(fetch, g)) {
      t = GetTexture(g, true);
    }
    uint32_t fi = b.fetch_index;
    VkSampler sampler = GetSampler(fetch, b, t && t->resolved);
    // Per-fetch-constant parameters for the shader.
    uint32_t signs_in[4] = {uint32_t(fetch.sign_x), uint32_t(fetch.sign_y), uint32_t(fetch.sign_z),
                            uint32_t(fetch.sign_w)};
    uint32_t swizzle = fetch.swizzle;
    bool all_signed = signs_in[0] == 1 && signs_in[1] == 1 && signs_in[2] == 1 && signs_in[3] == 1;
    bool use_signed_view = t && all_signed && t->format.host_format_signed != VK_FORMAT_UNDEFINED;
    uint32_t signs = 0;
    if (!use_signed_view) {
      for (uint32_t comp = 0; comp < 4; ++comp) {
        uint32_t src = (swizzle >> (comp * 3)) & 7;
        uint32_t s = src < 4 ? signs_in[src] : 0;
        signs |= (s & 3) << (comp * 2);
      }
    }
    if (t && t->resolved) {
      // Resolved data already went through the render target's own encoding.
      signs = 0;
      use_signed_view = false;
    }
    c.tex_info[fi * 4 + 0] = signs;
    c.tex_info[fi * 4 + 1] = (t && t->layers > 1 && t->view_type != VK_IMAGE_VIEW_TYPE_CUBE) ? 1 : 0;
    c.tex_info[fi * 4 + 2] = uint32_t(g.dimension);
    c.tex_size[fi * 4 + 0] = float(std::max<uint32_t>(g.width, 1));
    c.tex_size[fi * 4 + 1] = float(std::max<uint32_t>(g.height, 1));
    c.tex_size[fi * 4 + 2] = float(std::max<uint32_t>(g.depth, 1));
    c.tex_size[fi * 4 + 3] = std::ldexp(1.0f, int(fetch.exp_adjust));
    c.tex_uv[fi * 4 + 0] = 1.0f;
    c.tex_uv[fi * 4 + 1] = 1.0f;
    if (t && t->resolved) {
      c.tex_uv[fi * 4 + 0] = float(g.width) * t->res_scale / float(t->width);
      c.tex_uv[fi * 4 + 1] = float(g.height) * t->res_scale / float(t->height);
    }
    if (t && t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      // Resolved textures are left readable after the copy; anything else is
      // uploaded in the upload command buffer.
      EndRendering();
      TransitionImage(frame().cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, t->layout,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    auto view_or_null = [&](VkImageViewType type, VkImageView null_view) {
      if (!t) return null_view;
      bool ok = false;
      switch (type) {
        case VK_IMAGE_VIEW_TYPE_2D:
          ok = t->view_type != VK_IMAGE_VIEW_TYPE_3D;
          break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
          ok = t->view_type == VK_IMAGE_VIEW_TYPE_2D_ARRAY || t->view_type == VK_IMAGE_VIEW_TYPE_2D;
          break;
        case VK_IMAGE_VIEW_TYPE_3D:
          ok = t->view_type == VK_IMAGE_VIEW_TYPE_3D;
          break;
        case VK_IMAGE_VIEW_TYPE_CUBE:
          ok = t->view_type == VK_IMAGE_VIEW_TYPE_CUBE;
          break;
        default:
          break;
      }
      return ok ? GetTextureView(*t, swizzle, use_signed_view, type) : null_view;
    };
    switch (b.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        infos[b.binding] = {sampler, view_or_null(VK_IMAGE_VIEW_TYPE_2D, null_2d_.view),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        infos[b.binding] = {sampler, view_or_null(VK_IMAGE_VIEW_TYPE_3D, null_3d_.view),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        infos[b.binding + 1] = {sampler,
                                view_or_null(VK_IMAGE_VIEW_TYPE_2D_ARRAY, null_2d_array_.view),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        break;
      case xenos::FetchOpDimension::kCube:
        infos[b.binding] = {sampler, view_or_null(VK_IMAGE_VIEW_TYPE_CUBE, null_cube_.view),
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        break;
    }
  }
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    if (!infos[i].imageView) {
      infos[i] = {point_sampler_, null_2d_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
  }
  // Draws that bind the same textures and samplers share one descriptor set.
  uint64_t cache_key = HashBytes(infos, sizeof(VkDescriptorImageInfo) * descriptor_count,
                                 descriptor_count);
  auto cached = texture_set_cache_.find(cache_key);
  if (cached != texture_set_cache_.end()) {
    set_out = cached->second;
    return;
  }
  VkDescriptorSet set = AllocateDescriptorSet(GetTextureSetLayout(ts.texture_descriptor_count));
  if (!set) {
    return;
  }
  VkWriteDescriptorSet writes[kMaxDescriptors];
  for (uint32_t i = 0; i < descriptor_count; ++i) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &infos[i];
  }
  vulkan_device_->functions().vkUpdateDescriptorSets(vk_device_, descriptor_count, writes, 0,
                                                     nullptr);
  texture_set_cache_.emplace(cache_key, set);
  set_out = set;
}

void Renderer::EvictResources() {
  const auto& dfn = vulkan_device_->functions();
  constexpr uint64_t kMinAge = 8;         // frames; longer than the frames in flight
  constexpr uint64_t kStaleAge = 1800;    // frames; always freed

  // Render targets that nothing has drawn to or resolved from for a while.
  for (auto it = render_targets_.begin(); it != render_targets_.end();) {
    RenderTarget& rt = *it->second;
    if (frame_number_ > rt.last_used_frame + 900) {
      VkImage image = rt.image;
      VmaAllocation allocation = rt.allocation;
      VkImageView view = rt.view, sample_view = rt.sample_view;
      render_target_bytes_ -= std::min(render_target_bytes_, rt.memory_bytes);
      DeferDestroy([this, image, allocation, view, sample_view]() {
        const auto& d = vulkan_device_->functions();
        d.vkDestroyImageView(vk_device_, view, nullptr);
        d.vkDestroyImageView(vk_device_, sample_view, nullptr);
        vmaDestroyImage(allocator_, image, allocation);
      });
      it = render_targets_.erase(it);
      ++stats_.evicted;
    } else {
      ++it;
    }
  }

  // Textures: anything stale, then least recently used while over budget.
  const uint64_t budget = texture_budget_bytes_;
  const uint64_t target = budget / 100 * 85;
  const bool trimming = texture_bytes_ > budget;
  std::vector<Texture*> candidates;
  for (auto& entry : textures_) {
    Texture* t = entry.second.get();
    if (frame_number_ > t->last_used_frame + kMinAge) {
      candidates.push_back(t);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Texture* a, const Texture* b) { return a->last_used_frame < b->last_used_frame; });
  for (Texture* t : candidates) {
    bool stale = frame_number_ > t->last_used_frame + kStaleAge;
    if (!stale && !(trimming && texture_bytes_ > target)) {
      break;
    }
    VkImage image = t->image;
    VmaAllocation allocation = t->allocation;
    std::vector<VkImageView> views;
    for (auto& v : t->views) views.push_back(v.second);
    texture_bytes_ -= std::min(texture_bytes_, t->memory_bytes);
    DeferDestroy([this, image, allocation, views]() {
      const auto& d = vulkan_device_->functions();
      for (VkImageView v : views) d.vkDestroyImageView(vk_device_, v, nullptr);
      vmaDestroyImage(allocator_, image, allocation);
    });
    if (t->resolved) {
      resolved_textures_.erase(std::remove(resolved_textures_.begin(), resolved_textures_.end(), t),
                               resolved_textures_.end());
    }
    uint32_t key = (t->guest.base_address ? t->guest.base_address : t->guest.mip_address) & 0x1FFFFFFF;
    auto range = textures_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
      if (it->second.get() == t) {
        textures_.erase(it);
        break;
      }
    }
    ++stats_.evicted;
  }
  (void)dfn;
}

}  // namespace carbon::gpu
