// Carbon native renderer: Xenos shader microcode -> GLSL 4.50 (Vulkan).
//
// The translator runs once per unique microcode blob (hashed), producing a
// GLSL body plus the metadata the renderer needs to bind resources. Small
// per-draw differences (interpolator linkage, rectangle expansion, pixel
// parameter generation) are compiled as variants of the same body.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu_common.h"

namespace carbon::gpu {

struct TextureBinding {
  uint32_t fetch_index = 0;
  xenos::FetchOpDimension dimension = xenos::FetchOpDimension::k2D;
  // Instruction overrides (xenos::TextureFilter::kUseFetchConst = keep).
  xenos::TextureFilter mag_filter = xenos::TextureFilter::kUseFetchConst;
  xenos::TextureFilter min_filter = xenos::TextureFilter::kUseFetchConst;
  xenos::TextureFilter mip_filter = xenos::TextureFilter::kUseFetchConst;
  xenos::AnisoFilter aniso_filter = xenos::AnisoFilter::kUseFetchConst;
  // Descriptor binding within the stage's texture range. 3D fetches use two
  // consecutive bindings: a 3D texture and a 2D array (stacked textures).
  uint32_t binding = 0;
};

struct TranslatedShader {
  xenos::ShaderType type = xenos::ShaderType::kVertex;
  bool valid = false;
  std::string error;

  // GLSL: helper-free body of `void xe_shader(...)`, and its declarations.
  std::string body;
  uint32_t temp_count = 1;

  // Resources.
  uint32_t vfetch_used[3] = {};  // 96 bits
  // Per fetch constant: vertex stride and the dwords read past the vertex start
  // (both in dwords); `vfetch_computed` bits mark constants fetched with an index
  // other than the plain vertex index (r0.x), whose range is not known.
  uint32_t vfetch_stride[96] = {};
  uint32_t vfetch_extent[96] = {};
  uint32_t vfetch_computed[3] = {};
  std::vector<TextureBinding> textures;
  uint32_t texture_descriptor_count = 0;
  // Highest float constant index read (+1), 256 if relatively addressed.
  uint32_t float_constant_count = 0;

  // Outputs.
  uint32_t interpolators_written = 0;  // VS, bit per interpolator
  uint32_t colors_written = 0;         // PS, bit per render target
  bool writes_depth = false;
  bool writes_point_size = false;
  bool kills = false;
  bool uses_memexport = false;
  bool has_complex_control_flow = false;
  uint32_t instruction_count = 0;
};

// `ucode` is the microcode in host byte order.
TranslatedShader TranslateShader(xenos::ShaderType type, const uint32_t* ucode,
                                 uint32_t dword_count);

// Variant parameters that change the generated GLSL around the shared body.
struct VertexShaderVariant {
  uint32_t interpolator_count = 0;  // PS inputs to feed (0-16).
  bool rect_list = false;           // 3 guest vertices -> 2 host triangles.
  bool operator==(const VertexShaderVariant&) const = default;
};

struct PixelShaderVariant {
  uint32_t interpolator_count = 0;
  uint32_t flat_mask = 0;         // SQ_INTERPOLATOR_CNTL::param_shade
  uint32_t param_gen_index = 0;   // valid if param_gen
  bool param_gen = false;
  bool operator==(const PixelShaderVariant&) const = default;
};

std::string BuildVertexShaderGlsl(const TranslatedShader& vs, const VertexShaderVariant& variant);
std::string BuildPixelShaderGlsl(const TranslatedShader& ps, const PixelShaderVariant& variant);
// A pixel shader for depth-only draws (no guest pixel shader).
std::string BuildNullPixelShaderGlsl();

// Layout of the per-draw uniform block (set 0, binding 2), shared with the
// renderer. std140: everything is 16-byte aligned.
struct DrawConstants {
  uint32_t bools[8];            // 256 bool constants
  uint32_t loops[32];           // loop constants (one per uvec4 component group)
  float ndc_scale[4];           // xyz used
  float ndc_offset[4];          // xyz used
  uint32_t vtx[4];              // x: VTE flags (xy_fmt | z_fmt<<1 | w0_fmt<<2),
                                // y: index offset, z: min index, w: max index
  uint32_t vtx2[4];             // x: rect list index base (dwords in vertex data, or ~0 = auto)
                                // y: host vertices per guest index stream (unused)
  float alpha_test[4];          // x: reference, y: function (0-7), z: enabled
  float point[4];               // x: point size (pixels), y: min, z: max
  float pixel_pos[4];           // xy: scale for param gen (1), zw: offset (window + half pixel)
  uint32_t vfetch[96 * 2];      // per fetch constant: base dword in vertex data, endian
  float tex_size[32 * 4];       // per fetch constant: width, height, depth, exp scale
  uint32_t tex_info[32 * 4];    // x: signs (2 bits each), y: stacked, z: dimension, w: unused
  float tex_uv[32 * 4];         // per fetch constant: xy = scale from the guest size to the
                                // host image (resolved images can be larger), zw unused
};
static_assert(sizeof(DrawConstants) % 16 == 0);

}  // namespace carbon::gpu
