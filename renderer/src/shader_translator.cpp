// Carbon native renderer: Xenos shader microcode -> GLSL 4.50 (Vulkan).
//
// Instruction encodings come from the SDK's ucode.h. Operation semantics
// (including the Direct3D 9 "0 * anything = 0" multiplication rule, predicate
// and loop behaviour, vertex fetch format unpacking and cube coordinates)
// follow the documentation in ucode.h and Xenia's BSD-licensed translator.
//
// Control flow:
//  - Shaders without jumps, calls or loops are emitted as straight code, each
//    exec block guarded by its bool-constant or predicate condition.
//  - Otherwise the control flow program becomes a `while (true) switch (pc)`
//    state machine, one case per control flow instruction.

#include "shader_translator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include <fmt/format.h>

#include <rex/graphics/format/ucode.h>

namespace carbon::gpu {

namespace ucode = rex::graphics::ucode;
using ucode::AluInstruction;
using ucode::AluScalarOpcode;
using ucode::AluVectorOpcode;
using ucode::ControlFlowInstruction;
using ucode::ControlFlowOpcode;
using ucode::FetchOpcode;

namespace {

const char kComp[] = "xyzw";

// Operand count of each vector opcode (from the opcode descriptions in ucode.h).
uint32_t VectorOperandCount(AluVectorOpcode op) {
  switch (op) {
    case AluVectorOpcode::kFrc:
    case AluVectorOpcode::kTrunc:
    case AluVectorOpcode::kFloor:
    case AluVectorOpcode::kMax4:
      return 1;
    case AluVectorOpcode::kMad:
    case AluVectorOpcode::kCndEq:
    case AluVectorOpcode::kCndGe:
    case AluVectorOpcode::kCndGt:
    case AluVectorOpcode::kDp2Add:
      return 3;
    default:
      return 2;
  }
}

// 0: no operands, 1: one operand (a, or a and b), 2: constant a + temp b.
uint32_t ScalarOperandKind(AluScalarOpcode op) {
  switch (op) {
    case AluScalarOpcode::kSetpClr:
    case AluScalarOpcode::kRetainPrev:
      return 0;
    case AluScalarOpcode::kMulsc0:
    case AluScalarOpcode::kMulsc1:
    case AluScalarOpcode::kAddsc0:
    case AluScalarOpcode::kAddsc1:
    case AluScalarOpcode::kSubsc0:
    case AluScalarOpcode::kSubsc1:
      return 2;
    default:
      return uint32_t(op) <= uint32_t(AluScalarOpcode::kSqrt) ||
                     op == AluScalarOpcode::kSin || op == AluScalarOpcode::kCos
                 ? 1
                 : 0;
  }
}

class Translator {
 public:
  Translator(xenos::ShaderType type, const uint32_t* ucode, uint32_t dword_count)
      : type_(type), ucode_(ucode), dword_count_(dword_count) {}

  TranslatedShader Run();

 private:
  uint32_t cur_vfetch_ = 96;
  bool is_vs() const { return type_ == xenos::ShaderType::kVertex; }

  void Line(const std::string& s) {
    out_.append(indent_ * 2, ' ');
    out_ += s;
    out_ += '\n';
  }

  std::string TempRef(uint32_t reg, bool relative) {
    if (relative) {
      uses_relative_temps_ = true;
      return fmt::format("r[(aL + {}) & 63]", reg);
    }
    max_temp_ = std::max(max_temp_, reg + 1);
    return fmt::format("r[{}]", reg);
  }

  std::string ConstRef(uint32_t index, bool addressed, bool a0_relative) {
    const char* block = is_vs() ? "xe_vsc.c" : "xe_psc.c";
    if (addressed) {
      float_const_relative_ = true;
      return fmt::format("{}[({} + {}) & 255]", block, a0_relative ? "a0" : "aL", index);
    }
    max_const_ = std::max(max_const_, index + 1);
    return fmt::format("{}[{}]", block, index);
  }

  // Source register expression for ALU operand i (1-3), before swizzling.
  std::string AluSource(const AluInstruction& op, uint32_t i) {
    uint32_t reg = op.src_reg(i);
    std::string base;
    bool absolute;
    if (op.src_is_temp(i)) {
      base = TempRef(AluInstruction::src_temp_reg(reg), AluInstruction::is_src_temp_relative(reg));
      absolute = AluInstruction::is_src_temp_value_absolute(reg);
    } else {
      base = ConstRef(reg, op.src_const_is_addressed(i), op.is_const_address_register_relative());
      absolute = op.abs_constants();
    }
    if (absolute) {
      base = "abs(" + base + ")";
    }
    return base;
  }

  std::string ApplyNegate(const AluInstruction& op, uint32_t i, const std::string& e) {
    return op.src_negate(i) ? "(-" + e + ")" : e;
  }

  std::string VectorOperand(const AluInstruction& op, uint32_t i) {
    std::string src = AluSource(op, i);
    uint32_t swz = op.src_swizzle(i);
    std::string s = src;
    if (swz != 0) {
      s += '.';
      for (uint32_t c = 0; c < 4; ++c) {
        s += kComp[AluInstruction::GetSwizzledComponentIndex(swz, c)];
      }
    }
    return ApplyNegate(op, i, s);
  }

  // Scalar operand component (comp 3 = `a`, comp 0 = `b`).
  std::string ScalarOperand(const AluInstruction& op, uint32_t comp) {
    std::string src = AluSource(op, 3);
    uint32_t swz = op.src_swizzle(3);
    std::string s = src + '.' + kComp[AluInstruction::GetSwizzledComponentIndex(swz, comp)];
    return ApplyNegate(op, 3, s);
  }

  void EmitAlu(const AluInstruction& op);
  void EmitFetch(const uint32_t* words);
  void EmitVertexFetch(const ucode::VertexFetchInstruction& vf);
  void EmitTextureFetch(const ucode::TextureFetchInstruction& tf);
  void EmitExecBody(uint32_t address, uint32_t count, uint32_t sequence);
  std::string FetchResultStore(uint32_t dest, bool relative, uint32_t swizzle,
                               const std::string& value);
  uint32_t TextureBindingFor(const ucode::TextureFetchInstruction& tf);
  std::string BoolCondition(uint32_t bool_index, bool condition) {
    return fmt::format("(((xe_draw.bools[{}][{}] >> {}u) & 1u) {} 0u)", bool_index >> 7,
                       (bool_index >> 5) & 3, bool_index & 31, condition ? "!=" : "==");
  }

  xenos::ShaderType type_;
  const uint32_t* ucode_;
  uint32_t dword_count_;

  std::string out_;
  int indent_ = 1;

  uint32_t max_temp_ = 1;
  bool uses_relative_temps_ = false;
  uint32_t max_const_ = 0;
  bool float_const_relative_ = false;

  TranslatedShader result_;
};

std::string Translator::FetchResultStore(uint32_t dest, bool relative, uint32_t swizzle,
                                         const std::string& value) {
  // Fetch destination swizzles are absolute: X/Y/Z/W of the fetched value,
  // 0, 1 or keep.
  std::string target = TempRef(dest, relative);
  std::string code = "{ vec4 f = " + value + "; ";
  for (uint32_t c = 0; c < 4; ++c) {
    auto s = ucode::GetFetchDestinationComponentSwizzle(swizzle, c);
    switch (s) {
      case ucode::FetchDestinationSwizzle::kX:
      case ucode::FetchDestinationSwizzle::kY:
      case ucode::FetchDestinationSwizzle::kZ:
      case ucode::FetchDestinationSwizzle::kW:
        code += fmt::format("{}.{} = f.{}; ", target, kComp[c], kComp[uint32_t(s)]);
        break;
      case ucode::FetchDestinationSwizzle::k0:
        code += fmt::format("{}.{} = 0.0; ", target, kComp[c]);
        break;
      case ucode::FetchDestinationSwizzle::k1:
        code += fmt::format("{}.{} = 1.0; ", target, kComp[c]);
        break;
      default:
        break;
    }
  }
  code += "}";
  return code;
}

void Translator::EmitVertexFetch(const ucode::VertexFetchInstruction& vf) {
  // Which result components are written.
  uint32_t used = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    auto s = ucode::GetFetchDestinationComponentSwizzle(vf.dest_swizzle(), c);
    if (s <= ucode::FetchDestinationSwizzle::kW) {
      used |= 1u << uint32_t(s);
    }
  }
  if (!vf.is_mini_fetch()) {
    uint32_t fetch_index = vf.fetch_constant_index();
    result_.vfetch_used[fetch_index >> 5] |= 1u << (fetch_index & 31);
    cur_vfetch_ = fetch_index;
    result_.vfetch_stride[fetch_index] = std::max<uint32_t>(result_.vfetch_stride[fetch_index], vf.stride());
    if (vf.src() != 0 || vf.is_src_relative() || (vf.src_swizzle() & 3) != 0 || vf.is_index_rounded()) {
      result_.vfetch_computed[fetch_index >> 5] |= 1u << (fetch_index & 31);
    }
    std::string src = TempRef(vf.src(), vf.is_src_relative());
    std::string index = fmt::format("{}.{}", src, kComp[vf.src_swizzle() & 3]);
    if (vf.is_index_rounded()) {
      index = "(" + index + " + 0.5)";
    }
    // Base dword of this fetch constant's data in the vertex buffer.
    Line(fmt::format("xe_vf_addr = int(xe_draw.vfetch[{}][{}]) + int(floor({})) * {};",
                     fetch_index >> 1, (fetch_index & 1) * 2, index, vf.stride()));
    Line(fmt::format("xe_vf_endian = xe_draw.vfetch[{}][{}];", fetch_index >> 1,
                     (fetch_index & 1) * 2 + 1));
  }
  auto format = vf.data_format();
  uint32_t comps = 0;
  switch (format) {
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_FLOAT:
      comps = 1;
      break;
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_FLOAT:
      comps = 2;
      break;
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      comps = 3;
      break;
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      comps = 4;
      break;
    default:
      comps = 0;
      break;
  }
  if (!used || !comps) {
    if (used || vf.dest_swizzle() != 0xFFF) {
      // Constant writes only (or an unknown format: zeros).
      Line(FetchResultStore(vf.dest(), vf.is_dest_relative(), vf.dest_swizzle(), "vec4(0.0)"));
    }
    return;
  }
  int32_t offset = vf.offset();
  if (cur_vfetch_ < 96) {
    result_.vfetch_extent[cur_vfetch_] =
        std::max<uint32_t>(result_.vfetch_extent[cur_vfetch_], uint32_t(std::max(offset, 0)) + 4);
  }
  auto word = [&](int32_t i) {
    return fmt::format("xe_vword(xe_vf_addr + {})", offset + i);
  };
  const bool is_signed = vf.is_signed();
  const bool normalized = vf.is_normalized();
  const bool no_zero = vf.signed_rf_mode() == xenos::SignedRepeatingFractionMode::kNoZero;
  std::string value;
  auto packed = [&](std::initializer_list<std::pair<int, int>> fields, uint32_t word_count) {
    // fields: (word index, (offset << 8) | width) per component.
    std::string w0 = word(0);
    std::string w1 = word_count > 1 ? word(1) : w0;
    std::string comps_expr[4] = {"0.0", "0.0", "0.0", "0.0"};
    uint32_t i = 0;
    for (auto [wi, ow] : fields) {
      int off = ow >> 8;
      int width = ow & 0xFF;
      std::string src = wi ? "w1" : "w0";
      std::string e;
      if (is_signed) {
        e = fmt::format("float(bitfieldExtract(int({}), {}, {}))", src, off, width);
      } else {
        e = fmt::format("float(bitfieldExtract({}, {}, {}))", src, off, width);
      }
      if (normalized) {
        if (is_signed) {
          float scale = float((1u << (width - 1)) - 1) + (no_zero ? 0.5f : 0.0f);
          if (no_zero) {
            e = fmt::format("(({} + 0.5) / {:.1f})", e, scale);
          } else {
            e = fmt::format("max({} / {:.1f}, -1.0)", e, scale);
          }
        } else {
          e = fmt::format("({} / {:.1f})", e, float((1u << width) - 1));
        }
      }
      comps_expr[i++] = e;
    }
    // Use locals for the words so each is loaded once.
    std::string pre = fmt::format("{{ uint w0 = {}; uint w1 = {}; ", w0, w1);
    value = pre + "xe_vtmp = vec4(" + comps_expr[0] + ", " + comps_expr[1] + ", " + comps_expr[2] +
            ", " + comps_expr[3] + "); }";
  };
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
      packed({{0, (0 << 8) | 8}, {0, (8 << 8) | 8}, {0, (16 << 8) | 8}, {0, (24 << 8) | 8}}, 1);
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      packed({{0, (0 << 8) | 10}, {0, (10 << 8) | 10}, {0, (20 << 8) | 10}, {0, (30 << 8) | 2}},
             1);
      break;
    case xenos::VertexFormat::k_10_11_11:
      packed({{0, (0 << 8) | 11}, {0, (11 << 8) | 11}, {0, (22 << 8) | 10}}, 1);
      break;
    case xenos::VertexFormat::k_11_11_10:
      packed({{0, (0 << 8) | 10}, {0, (10 << 8) | 11}, {0, (21 << 8) | 11}}, 1);
      break;
    case xenos::VertexFormat::k_16_16:
      packed({{0, (0 << 8) | 16}, {0, (16 << 8) | 16}}, 1);
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      packed({{0, (0 << 8) | 16}, {0, (16 << 8) | 16}, {1, (0 << 8) | 16}, {1, (16 << 8) | 16}},
             2);
      break;
    case xenos::VertexFormat::k_16_16_FLOAT:
      value = fmt::format("{{ xe_vtmp = vec4(unpackHalf2x16({}), 0.0, 0.0); }}", word(0));
      break;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      value = fmt::format("{{ xe_vtmp = vec4(unpackHalf2x16({}), unpackHalf2x16({})); }}", word(0),
                          word(1));
      break;
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32: {
      std::string c[4] = {"0.0", "0.0", "0.0", "0.0"};
      for (uint32_t i = 0; i < comps; ++i) {
        std::string w = word(int32_t(i));
        std::string e = is_signed ? fmt::format("float(int({}))", w) : fmt::format("float({})", w);
        if (normalized) {
          if (is_signed) {
            e = no_zero ? fmt::format("(({} + 0.5) / 2147483647.5)", e)
                        : fmt::format("({} / 2147483647.0)", e);
          } else {
            e = fmt::format("({} / 4294967295.0)", e);
          }
        }
        c[i] = e;
      }
      value = fmt::format("{{ xe_vtmp = vec4({}, {}, {}, {}); }}", c[0], c[1], c[2], c[3]);
    } break;
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT: {
      std::string c[4] = {"0.0", "0.0", "0.0", "0.0"};
      for (uint32_t i = 0; i < comps; ++i) {
        if (used & (1u << i)) {
          c[i] = fmt::format("uintBitsToFloat({})", word(int32_t(i)));
        }
      }
      value = fmt::format("{{ xe_vtmp = vec4({}, {}, {}, {}); }}", c[0], c[1], c[2], c[3]);
    } break;
    default:
      value = "{ xe_vtmp = vec4(0.0); }";
      break;
  }
  Line(value);
  if (vf.exp_adjust()) {
    Line(fmt::format("xe_vtmp *= {:.9g};", std::ldexp(1.0f, vf.exp_adjust())));
  }
  Line(FetchResultStore(vf.dest(), vf.is_dest_relative(), vf.dest_swizzle(), "xe_vtmp"));
}

uint32_t Translator::TextureBindingFor(const ucode::TextureFetchInstruction& tf) {
  TextureBinding b;
  b.fetch_index = tf.fetch_constant_index();
  b.dimension = tf.dimension();
  b.mag_filter = tf.mag_filter();
  b.min_filter = tf.min_filter();
  b.mip_filter = tf.mip_filter();
  b.aniso_filter = tf.aniso_filter();
  for (size_t i = 0; i < result_.textures.size(); ++i) {
    const TextureBinding& e = result_.textures[i];
    if (e.fetch_index == b.fetch_index && e.dimension == b.dimension &&
        e.mag_filter == b.mag_filter && e.min_filter == b.min_filter &&
        e.mip_filter == b.mip_filter && e.aniso_filter == b.aniso_filter) {
      return uint32_t(i);
    }
  }
  b.binding = result_.texture_descriptor_count;
  result_.texture_descriptor_count += b.dimension == xenos::FetchOpDimension::k3DOrStacked ? 2 : 1;
  result_.textures.push_back(b);
  return uint32_t(result_.textures.size() - 1);
}

void Translator::EmitTextureFetch(const ucode::TextureFetchInstruction& tf) {
  std::string src = TempRef(tf.src(), tf.is_src_relative());
  uint32_t swz = tf.src_swizzle();
  auto coord = [&](uint32_t c) { return fmt::format("{}.{}", src, kComp[(swz >> (c * 2)) & 3]); };
  switch (tf.opcode()) {
    case FetchOpcode::kSetTextureLod:
      Line(fmt::format("xe_lod = {};", coord(0)));
      return;
    case FetchOpcode::kSetTextureGradientsHorz:
      Line(fmt::format("xe_grad_h = vec3({}, {}, {});", coord(0), coord(1), coord(2)));
      return;
    case FetchOpcode::kSetTextureGradientsVert:
      Line(fmt::format("xe_grad_v = vec3({}, {}, {});", coord(0), coord(1), coord(2)));
      return;
    case FetchOpcode::kGetTextureGradients: {
      if (is_vs()) {
        Line(FetchResultStore(tf.dest(), tf.is_dest_relative(), tf.dest_swizzle(), "vec4(0.0)"));
      } else {
        Line(fmt::format(
            "{{ vec2 gc = vec2({}, {}); vec2 gx = dFdx(gc); vec2 gy = dFdy(gc); {} }}", coord(0),
            coord(1),
            FetchResultStore(tf.dest(), tf.is_dest_relative(), tf.dest_swizzle(),
                             "vec4(gx.x, gy.x, gx.y, gy.y)")));
      }
      return;
    }
    case FetchOpcode::kGetTextureBorderColorFrac:
      Line(FetchResultStore(tf.dest(), tf.is_dest_relative(), tf.dest_swizzle(), "vec4(0.0)"));
      return;
    default:
      break;
  }

  uint32_t binding_index = TextureBindingFor(tf);
  const TextureBinding& binding = result_.textures[binding_index];
  uint32_t fi = binding.fetch_index;
  std::string prefix = is_vs() ? "xe_vt" : "xe_pt";
  std::string sampler = fmt::format("{}{}", prefix, binding.binding);
  std::string sampler_array = fmt::format("{}{}", prefix, binding.binding + 1);
  std::string size = fmt::format("xe_draw.tex_size[{}]", fi);

  // Coordinates, with unnormalized coordinates and offsets folded in.
  std::string c0 = coord(0), c1 = coord(1), c2 = coord(2);
  float ox = tf.offset_x(), oy = tf.offset_y(), oz = tf.offset_z();
  auto norm = [&](const std::string& c, float o, const char* dim) {
    std::string e = c;
    if (o != 0.0f) {
      if (tf.unnormalized_coordinates()) {
        e = fmt::format("({} + {:.9g})", e, o);
      } else {
        e = fmt::format("({} + {:.9g} / {}.{})", e, o, size, dim);
      }
    }
    if (tf.unnormalized_coordinates()) {
      e = fmt::format("({} / {}.{})", e, size, dim);
    }
    return e;
  };

  std::string lod_expr;
  bool explicit_lod = !tf.use_computed_lod() || is_vs();
  float bias = tf.lod_bias();
  if (tf.use_register_lod()) {
    lod_expr = fmt::format("(xe_lod + {:.9g})", bias);
    explicit_lod = true;
  } else if (explicit_lod) {
    lod_expr = fmt::format("{:.9g}", bias);
  }

  std::string sample;
  bool want_lod_query = tf.opcode() == FetchOpcode::kGetTextureComputedLod;
  bool want_weights = tf.opcode() == FetchOpcode::kGetTextureWeights;
  switch (binding.dimension) {
    case xenos::FetchOpDimension::k1D:
    case xenos::FetchOpDimension::k2D: {
      std::string uv = binding.dimension == xenos::FetchOpDimension::k1D
                           ? fmt::format("vec2({}, 0.5)", norm(c0, ox, "x"))
                           : fmt::format("vec2({}, {})", norm(c0, ox, "x"), norm(c1, oy, "y"));
      uv = fmt::format("({} * xe_draw.tex_uv[{}].xy)", uv, fi);
      if (want_lod_query) {
        sample = is_vs() ? "vec4(0.0)"
                         : fmt::format("vec4(textureQueryLod({}, {}).y, 0.0, 0.0, 0.0)", sampler, uv);
      } else if (want_weights) {
        sample = fmt::format("vec4(fract({} * {}.xy - 0.5), 0.0, 0.0)", uv, size);
      } else if (explicit_lod) {
        sample = fmt::format("textureLod({}, {}, {})", sampler, uv, lod_expr);
      } else if (bias != 0.0f) {
        sample = fmt::format("texture({}, {}, {:.9g})", sampler, uv, bias);
      } else {
        sample = fmt::format("texture({}, {})", sampler, uv);
      }
    } break;
    case xenos::FetchOpDimension::k3DOrStacked: {
      std::string uvw = fmt::format("vec3({}, {}, {})", norm(c0, ox, "x"), norm(c1, oy, "y"),
                                    norm(c2, oz, "z"));
      // Stacked 2D textures: the layer is W in [0, 1) times the depth.
      std::string uvl = fmt::format(
          "vec3({}, {}, floor(clamp({} * {}.z, 0.0, {}.z - 1.0) + 0.5))", norm(c0, ox, "x"),
          norm(c1, oy, "y"), tf.unnormalized_coordinates() ? c2 : c2, size, size);
      if (tf.unnormalized_coordinates()) {
        uvl = fmt::format("vec3({}, {}, floor({} + 0.5))", norm(c0, ox, "x"), norm(c1, oy, "y"),
                          c2);
      }
      std::string s3d, s2da;
      if (want_lod_query) {
        s3d = is_vs() ? "vec4(0.0)"
                      : fmt::format("vec4(textureQueryLod({}, {}).y, 0.0, 0.0, 0.0)", sampler, uvw);
        s2da = is_vs() ? "vec4(0.0)"
                       : fmt::format("vec4(textureQueryLod({}, {}.xy).y, 0.0, 0.0, 0.0)",
                                     sampler_array, uvl);
      } else if (explicit_lod) {
        s3d = fmt::format("textureLod({}, {}, {})", sampler, uvw, lod_expr);
        s2da = fmt::format("textureLod({}, {}, {})", sampler_array, uvl, lod_expr);
      } else {
        s3d = fmt::format("texture({}, {})", sampler, uvw);
        s2da = fmt::format("texture({}, {})", sampler_array, uvl);
      }
      sample = fmt::format("(xe_draw.tex_info[{}].y != 0u ? {} : {})", fi, s2da, s3d);
    } break;
    case xenos::FetchOpDimension::kCube: {
      std::string dir = fmt::format("xe_cube_dir(vec3({}, {}, {}))", c0, c1,
                                    oz != 0.0f ? fmt::format("({} + {:.9g})", c2, oz) : c2);
      if (want_lod_query) {
        sample = is_vs() ? "vec4(0.0)"
                         : fmt::format("vec4(textureQueryLod({}, {}).y, 0.0, 0.0, 0.0)", sampler, dir);
      } else if (explicit_lod) {
        sample = fmt::format("textureLod({}, {}, {})", sampler, dir, lod_expr);
      } else {
        sample = fmt::format("texture({}, {})", sampler, dir);
      }
    } break;
  }
  if (!want_lod_query && !want_weights) {
    sample = fmt::format("xe_tex_post({}, {}u)", sample, fi);
  }
  Line(FetchResultStore(tf.dest(), tf.is_dest_relative(), tf.dest_swizzle(), sample));
}

void Translator::EmitFetch(const uint32_t* words) {
  const auto& fetch = *reinterpret_cast<const ucode::FetchInstruction*>(words);
  bool predicated = fetch.is_predicated();
  if (predicated) {
    Line(fmt::format("if (p0 == {}) {{", fetch.predicate_condition() ? "true" : "false"));
    ++indent_;
  }
  if (fetch.opcode() == FetchOpcode::kVertexFetch) {
    if (is_vs()) {
      EmitVertexFetch(fetch.vertex_fetch());
    }
  } else {
    EmitTextureFetch(fetch.texture_fetch());
  }
  if (predicated) {
    --indent_;
    Line("}");
  }
}

void Translator::EmitAlu(const AluInstruction& op) {
  const AluVectorOpcode vop = op.vector_opcode();
  const AluScalarOpcode sop = op.scalar_opcode();
  const bool is_export = op.is_export();

  const uint32_t vmask = op.GetVectorOpResultWriteMask();
  const uint32_t smask = op.GetScalarOpResultWriteMask();
  const uint32_t const0_mask = op.GetConstant0WriteMask();
  const uint32_t const1_mask = op.GetConstant1WriteMask();

  // Vector ops that have side effects must run even with an empty mask.
  bool vector_side_effects = false;
  switch (vop) {
    case AluVectorOpcode::kSetpEqPush:
    case AluVectorOpcode::kSetpNePush:
    case AluVectorOpcode::kSetpGtPush:
    case AluVectorOpcode::kSetpGePush:
    case AluVectorOpcode::kKillEq:
    case AluVectorOpcode::kKillGt:
    case AluVectorOpcode::kKillGe:
    case AluVectorOpcode::kKillNe:
    case AluVectorOpcode::kMaxA:
      vector_side_effects = true;
      break;
    default:
      break;
  }
  const bool do_vector = vmask || vector_side_effects;
  // Every scalar op except retain_prev updates ps, so it must run.
  const bool do_scalar = sop != AluScalarOpcode::kRetainPrev || smask;

  if (op.is_predicated()) {
    Line(fmt::format("if (p0 == {}) {{", op.predicate_condition() ? "true" : "false"));
    ++indent_;
  }
  Line("{");
  ++indent_;

  // ---- Vector operation ----
  if (do_vector) {
    uint32_t n = VectorOperandCount(vop);
    std::string a = VectorOperand(op, 1);
    std::string b = n >= 2 ? VectorOperand(op, 2) : "vec4(0.0)";
    std::string c = n >= 3 ? VectorOperand(op, 3) : "vec4(0.0)";
    Line("vec4 va = " + a + ";");
    if (n >= 2) Line("vec4 vb = " + b + ";");
    if (n >= 3) Line("vec4 vc = " + c + ";");
    std::string v;
    switch (vop) {
      case AluVectorOpcode::kAdd: v = "va + vb"; break;
      case AluVectorOpcode::kMul: v = "xe_mul(va, vb)"; break;
      case AluVectorOpcode::kMax: v = "mix(vb, va, greaterThanEqual(va, vb))"; break;
      case AluVectorOpcode::kMin: v = "mix(vb, va, lessThan(va, vb))"; break;
      case AluVectorOpcode::kSeq: v = "vec4(equal(va, vb))"; break;
      case AluVectorOpcode::kSgt: v = "vec4(greaterThan(va, vb))"; break;
      case AluVectorOpcode::kSge: v = "vec4(greaterThanEqual(va, vb))"; break;
      case AluVectorOpcode::kSne: v = "vec4(notEqual(va, vb))"; break;
      case AluVectorOpcode::kFrc: v = "va - floor(va)"; break;
      case AluVectorOpcode::kTrunc: v = "trunc(va)"; break;
      case AluVectorOpcode::kFloor: v = "floor(va)"; break;
      case AluVectorOpcode::kMad: v = "xe_mul(va, vb) + vc"; break;
      case AluVectorOpcode::kCndEq: v = "mix(vc, vb, equal(va, vec4(0.0)))"; break;
      case AluVectorOpcode::kCndGe: v = "mix(vc, vb, greaterThanEqual(va, vec4(0.0)))"; break;
      case AluVectorOpcode::kCndGt: v = "mix(vc, vb, greaterThan(va, vec4(0.0)))"; break;
      case AluVectorOpcode::kDp4: v = "vec4(xe_dot4(va, vb))"; break;
      case AluVectorOpcode::kDp3: v = "vec4(xe_dot3(va.xyz, vb.xyz))"; break;
      case AluVectorOpcode::kDp2Add:
        v = "vec4(xe_mul1(va.x, vb.x) + xe_mul1(va.y, vb.y) + vc.x)";
        break;
      case AluVectorOpcode::kCube: v = "xe_cube(va)"; break;
      case AluVectorOpcode::kMax4: v = "vec4(xe_max4(va))"; break;
      case AluVectorOpcode::kSetpEqPush:
        Line("xe_np = (va.w == 0.0 && vb.w == 0.0);");
        v = "vec4((va.x == 0.0 && vb.x == 0.0) ? 0.0 : va.x + 1.0)";
        break;
      case AluVectorOpcode::kSetpNePush:
        Line("xe_np = (va.w == 0.0 && vb.w != 0.0);");
        v = "vec4((va.x == 0.0 && vb.x != 0.0) ? 0.0 : va.x + 1.0)";
        break;
      case AluVectorOpcode::kSetpGtPush:
        Line("xe_np = (va.w == 0.0 && vb.w > 0.0);");
        v = "vec4((va.x == 0.0 && vb.x > 0.0) ? 0.0 : va.x + 1.0)";
        break;
      case AluVectorOpcode::kSetpGePush:
        Line("xe_np = (va.w == 0.0 && vb.w >= 0.0);");
        v = "vec4((va.x == 0.0 && vb.x >= 0.0) ? 0.0 : va.x + 1.0)";
        break;
      case AluVectorOpcode::kKillEq:
      case AluVectorOpcode::kKillGt:
      case AluVectorOpcode::kKillGe:
      case AluVectorOpcode::kKillNe: {
        const char* f = vop == AluVectorOpcode::kKillEq   ? "equal"
                        : vop == AluVectorOpcode::kKillGt ? "greaterThan"
                        : vop == AluVectorOpcode::kKillGe ? "greaterThanEqual"
                                                          : "notEqual";
        Line(fmt::format("bool vk = any({}(va, vb));", f));
        if (!is_vs()) {
          result_.kills = true;
          Line("if (vk) { discard; }");
        }
        v = "vec4(vk ? 1.0 : 0.0)";
      } break;
      case AluVectorOpcode::kDst: v = "vec4(1.0, xe_mul1(va.y, vb.y), va.z, vb.w)"; break;
      case AluVectorOpcode::kMaxA:
        Line("xe_na0 = int(clamp(floor(va.w + 0.5), -256.0, 255.0));");
        v = "mix(vb, va, greaterThanEqual(va, vb))";
        break;
      default:
        v = "vec4(0.0)";
        break;
    }
    if (op.vector_clamp()) {
      v = "clamp(" + v + ", 0.0, 1.0)";
    }
    Line("vec4 vr = " + v + ";");
  }

  // ---- Scalar operation ----
  if (do_scalar) {
    uint32_t kind = ScalarOperandKind(sop);
    if (kind == 1) {
      Line("float sa = " + ScalarOperand(op, 3) + ";");
      Line("float sb = " + ScalarOperand(op, 0) + ";");
    } else if (kind == 2) {
      // Constant (a, W swizzle) and temporary register (b, X swizzle).
      std::string cref = ConstRef(op.src_reg(3), op.src_const_is_addressed(3),
                                  op.is_const_address_register_relative());
      std::string tref = TempRef(op.scalar_const_reg_op_src_temp_reg(), false);
      if (op.abs_constants()) {
        cref = "abs(" + cref + ")";
        tref = "abs(" + tref + ")";
      }
      uint32_t swz = op.src_swizzle(3);
      std::string ca = cref + '.' + kComp[AluInstruction::GetSwizzledComponentIndex(swz, 3)];
      std::string tb = tref + '.' + kComp[AluInstruction::GetSwizzledComponentIndex(swz, 0)];
      if (op.src_negate(3)) {
        ca = "(-" + ca + ")";
        tb = "(-" + tb + ")";
      }
      Line("float sa = " + ca + ";");
      Line("float sb = " + tb + ";");
    }
    std::string s;
    switch (sop) {
      case AluScalarOpcode::kAdds: s = "sa + sb"; break;
      case AluScalarOpcode::kAddsPrev: s = "sa + ps"; break;
      case AluScalarOpcode::kMuls: s = "xe_mul1(sa, sb)"; break;
      case AluScalarOpcode::kMulsPrev: s = "xe_mul1(sa, ps)"; break;
      case AluScalarOpcode::kMulsPrev2:
        s = "((ps == -3.402823466e+38 || isinf(ps) || isnan(ps) || isinf(sb) || isnan(sb) || "
            "sb <= 0.0) ? -3.402823466e+38 : xe_mul1(sa, ps))";
        break;
      case AluScalarOpcode::kMaxs: s = "(sa >= sb ? sa : sb)"; break;
      case AluScalarOpcode::kMins: s = "(sa < sb ? sa : sb)"; break;
      case AluScalarOpcode::kSeqs: s = "(sa == 0.0 ? 1.0 : 0.0)"; break;
      case AluScalarOpcode::kSgts: s = "(sa > 0.0 ? 1.0 : 0.0)"; break;
      case AluScalarOpcode::kSges: s = "(sa >= 0.0 ? 1.0 : 0.0)"; break;
      case AluScalarOpcode::kSnes: s = "(sa != 0.0 ? 1.0 : 0.0)"; break;
      case AluScalarOpcode::kFrcs: s = "(sa - floor(sa))"; break;
      case AluScalarOpcode::kTruncs: s = "trunc(sa)"; break;
      case AluScalarOpcode::kFloors: s = "floor(sa)"; break;
      case AluScalarOpcode::kExp: s = "exp2(sa)"; break;
      case AluScalarOpcode::kLogc: s = "xe_logc(sa)"; break;
      case AluScalarOpcode::kLog: s = "(sa == 1.0 ? 0.0 : log2(sa))"; break;
      case AluScalarOpcode::kRcpc: s = "xe_clampmax(sa == 1.0 ? 1.0 : 1.0 / sa)"; break;
      case AluScalarOpcode::kRcpf: s = "xe_clampff(sa == 1.0 ? 1.0 : 1.0 / sa)"; break;
      case AluScalarOpcode::kRcp: s = "(sa == 1.0 ? 1.0 : 1.0 / sa)"; break;
      case AluScalarOpcode::kRsqc: s = "xe_clampmax(sa == 1.0 ? 1.0 : inversesqrt(sa))"; break;
      case AluScalarOpcode::kRsqf: s = "xe_clampff(sa == 1.0 ? 1.0 : inversesqrt(sa))"; break;
      case AluScalarOpcode::kRsq: s = "(sa == 1.0 ? 1.0 : inversesqrt(sa))"; break;
      case AluScalarOpcode::kMaxAs:
        Line("xe_na0 = int(clamp(floor(sa + 0.5), -256.0, 255.0));");
        s = "(sa >= sb ? sa : sb)";
        break;
      case AluScalarOpcode::kMaxAsf:
        Line("xe_na0 = int(clamp(floor(sa), -256.0, 255.0));");
        s = "(sa >= sb ? sa : sb)";
        break;
      case AluScalarOpcode::kSubs: s = "sa - sb"; break;
      case AluScalarOpcode::kSubsPrev: s = "sa - ps"; break;
      case AluScalarOpcode::kSetpEq:
        Line("xe_np = (sa == 0.0);");
        s = "(xe_np ? 0.0 : 1.0)";
        break;
      case AluScalarOpcode::kSetpNe:
        Line("xe_np = (sa != 0.0);");
        s = "(xe_np ? 0.0 : 1.0)";
        break;
      case AluScalarOpcode::kSetpGt:
        Line("xe_np = (sa > 0.0);");
        s = "(xe_np ? 0.0 : 1.0)";
        break;
      case AluScalarOpcode::kSetpGe:
        Line("xe_np = (sa >= 0.0);");
        s = "(xe_np ? 0.0 : 1.0)";
        break;
      case AluScalarOpcode::kSetpInv:
        Line("xe_np = (sa == 1.0);");
        s = "(xe_np ? 0.0 : (sa == 0.0 ? 1.0 : sa))";
        break;
      case AluScalarOpcode::kSetpPop:
        Line("xe_np = (sa - 1.0 <= 0.0);");
        s = "(xe_np ? 0.0 : sa - 1.0)";
        break;
      case AluScalarOpcode::kSetpClr:
        Line("xe_np = false;");
        s = "3.402823466e+38";
        break;
      case AluScalarOpcode::kSetpRstr:
        Line("xe_np = (sa == 0.0);");
        s = "(xe_np ? 0.0 : sa)";
        break;
      case AluScalarOpcode::kKillsEq:
      case AluScalarOpcode::kKillsGt:
      case AluScalarOpcode::kKillsGe:
      case AluScalarOpcode::kKillsNe:
      case AluScalarOpcode::kKillsOne: {
        const char* cmp = sop == AluScalarOpcode::kKillsEq   ? "sa == 0.0"
                          : sop == AluScalarOpcode::kKillsGt ? "sa > 0.0"
                          : sop == AluScalarOpcode::kKillsGe ? "sa >= 0.0"
                          : sop == AluScalarOpcode::kKillsNe ? "sa != 0.0"
                                                             : "sa == 1.0";
        Line(fmt::format("bool sk = ({});", cmp));
        if (!is_vs()) {
          result_.kills = true;
          Line("if (sk) { discard; }");
        }
        s = "(sk ? 1.0 : 0.0)";
      } break;
      case AluScalarOpcode::kSqrt: s = "sqrt(sa)"; break;
      case AluScalarOpcode::kMulsc0:
      case AluScalarOpcode::kMulsc1: s = "xe_mul1(sa, sb)"; break;
      case AluScalarOpcode::kAddsc0:
      case AluScalarOpcode::kAddsc1: s = "sa + sb"; break;
      case AluScalarOpcode::kSubsc0:
      case AluScalarOpcode::kSubsc1: s = "sa - sb"; break;
      case AluScalarOpcode::kSin: s = "sin(sa)"; break;
      case AluScalarOpcode::kCos: s = "cos(sa)"; break;
      case AluScalarOpcode::kRetainPrev: s = "ps"; break;
      default: s = "0.0"; break;
    }
    if (op.scalar_clamp()) {
      s = "clamp(" + s + ", 0.0, 1.0)";
    }
    Line("float sr = " + s + ";");
  }

  // ---- Writes (after both operations read their sources) ----
  auto mask_str = [](uint32_t m) {
    std::string s;
    for (uint32_t c = 0; c < 4; ++c) {
      if (m & (1u << c)) s += kComp[c];
    }
    return s;
  };
  if (is_export) {
    using ucode::ExportRegister;
    uint32_t reg = op.vector_dest();
    std::string target;
    if (is_vs()) {
      if (reg <= uint32_t(ExportRegister::kVSInterpolator15)) {
        target = fmt::format("xe_e_interp[{}]", reg);
        result_.interpolators_written |= 1u << reg;
      } else if (reg == uint32_t(ExportRegister::kVSPosition)) {
        target = "xe_e_pos";
      } else if (reg == uint32_t(ExportRegister::kVSPointSizeEdgeFlagKillVertex)) {
        target = "xe_e_misc";
        result_.writes_point_size = true;
      }
    } else {
      if (reg <= uint32_t(ExportRegister::kPSColor3)) {
        target = fmt::format("xe_e_color[{}]", reg);
        result_.colors_written |= 1u << reg;
      } else if (reg == uint32_t(ExportRegister::kPSDepth)) {
        target = "xe_e_depth";
        result_.writes_depth = true;
      }
    }
    if (reg == uint32_t(ExportRegister::kExportAddress) ||
        (reg >= uint32_t(ExportRegister::kExportData0) &&
         reg <= uint32_t(ExportRegister::kExportData4))) {
      result_.uses_memexport = true;
      target.clear();
    }
    if (!target.empty()) {
      for (uint32_t c = 0; c < 4; ++c) {
        uint32_t bit = 1u << c;
        if (const1_mask & bit) {
          Line(fmt::format("{}.{} = 1.0;", target, kComp[c]));
        } else if (const0_mask & bit) {
          Line(fmt::format("{}.{} = 0.0;", target, kComp[c]));
        } else if ((vmask & bit) && do_vector) {
          Line(fmt::format("{}.{} = vr.{};", target, kComp[c], kComp[c]));
        } else if ((smask & bit) && do_scalar) {
          Line(fmt::format("{}.{} = sr;", target, kComp[c]));
        }
      }
    }
  } else {
    if (vmask && do_vector) {
      std::string m = mask_str(vmask);
      Line(fmt::format("{}.{} = vr.{};", TempRef(op.vector_dest(), op.is_vector_dest_relative()),
                       m, m));
    }
    if (smask && do_scalar) {
      std::string m = mask_str(smask);
      std::string splat = m.size() == 1 ? "sr" : fmt::format("vec{}(sr)", m.size());
      Line(fmt::format("{}.{} = {};", TempRef(op.scalar_dest(), op.is_scalar_dest_relative()), m,
                       splat));
    }
  }
  // State updates.
  bool sets_p0_vector = vop >= AluVectorOpcode::kSetpEqPush && vop <= AluVectorOpcode::kSetpGePush;
  bool sets_p0_scalar = sop >= AluScalarOpcode::kSetpEq && sop <= AluScalarOpcode::kSetpRstr;
  if ((sets_p0_vector && do_vector) || (sets_p0_scalar && do_scalar)) {
    Line("p0 = xe_np;");
  }
  if ((vop == AluVectorOpcode::kMaxA && do_vector) ||
      ((sop == AluScalarOpcode::kMaxAs || sop == AluScalarOpcode::kMaxAsf) && do_scalar)) {
    Line("a0 = xe_na0;");
  }
  if (do_scalar && sop != AluScalarOpcode::kRetainPrev) {
    Line("ps = sr;");
  }
  --indent_;
  Line("}");
  if (op.is_predicated()) {
    --indent_;
    Line("}");
  }
}

void Translator::EmitExecBody(uint32_t address, uint32_t count, uint32_t sequence) {
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t dword = (address + i) * 3;
    if (dword + 3 > dword_count_) {
      break;
    }
    ++result_.instruction_count;
    const uint32_t* words = ucode_ + dword;
    bool is_fetch = (sequence >> (i * 2)) & 1;
    if (is_fetch) {
      EmitFetch(words);
    } else {
      EmitAlu(*reinterpret_cast<const AluInstruction*>(words));
    }
  }
}

TranslatedShader Translator::Run() {
  result_.type = type_;

  // Collect control flow instructions: pairs packed in 3 dwords, up to the
  // first ALU/fetch instruction any exec points at.
  std::vector<ControlFlowInstruction> cf;
  uint32_t cf_limit_dwords = dword_count_;
  for (uint32_t i = 0; i + 3 <= cf_limit_dwords; i += 3) {
    ControlFlowInstruction pair[2];
    ucode::UnpackControlFlowInstructions(ucode_ + i, pair);
    for (auto& c : pair) {
      if (ucode::IsControlFlowOpcodeExec(c.opcode())) {
        cf_limit_dwords = std::min(cf_limit_dwords, c.exec.address() * 3);
      }
      cf.push_back(c);
    }
  }
  // Stop after the last instruction that can end the program unconditionally.
  bool complex = false;
  for (const auto& c : cf) {
    switch (c.opcode()) {
      case ControlFlowOpcode::kLoopStart:
      case ControlFlowOpcode::kLoopEnd:
      case ControlFlowOpcode::kCondCall:
      case ControlFlowOpcode::kReturn:
      case ControlFlowOpcode::kCondJmp:
        complex = true;
        break;
      default:
        break;
    }
  }
  result_.has_complex_control_flow = complex;

  auto emit_exec = [&](const ControlFlowInstruction& c, bool in_switch) {
    ControlFlowOpcode opc = c.opcode();
    std::string cond;
    switch (opc) {
      case ControlFlowOpcode::kCondExec:
      case ControlFlowOpcode::kCondExecEnd:
      case ControlFlowOpcode::kCondExecPredClean:
      case ControlFlowOpcode::kCondExecPredCleanEnd:
        cond = BoolCondition(c.cond_exec.bool_address(), c.cond_exec.condition());
        break;
      case ControlFlowOpcode::kCondExecPred:
      case ControlFlowOpcode::kCondExecPredEnd:
        cond = fmt::format("(p0 == {})", c.cond_exec_pred.condition() ? "true" : "false");
        break;
      default:
        break;
    }
    bool ends = ucode::DoesControlFlowOpcodeEndShader(opc);
    if (!cond.empty()) {
      Line("if " + cond + " {");
      ++indent_;
    }
    EmitExecBody(c.exec.address(), c.exec.count(), c.exec.sequence());
    if (ends) {
      Line(in_switch ? "pc = -1; break;" : "break;");
    }
    if (!cond.empty()) {
      --indent_;
      Line("}");
    }
    return ends && cond.empty();
  };

  if (!complex) {
    Line("do {");
    ++indent_;
    for (const auto& c : cf) {
      if (ucode::IsControlFlowOpcodeExec(c.opcode())) {
        if (emit_exec(c, false)) {
          break;
        }
      }
    }
    --indent_;
    Line("} while (false);");
  } else {
    Line("int pc = 0;");
    Line("int xe_loop_count[4] = int[4](0, 0, 0, 0);");
    Line("int xe_loop_al[4] = int[4](0, 0, 0, 0);");
    Line("int xe_call_stack[4] = int[4](0, 0, 0, 0);");
    Line("int xe_csp = 0;");
    Line("for (int xe_guard = 0; xe_guard < 65536; ++xe_guard) {");
    ++indent_;
    Line("switch (pc) {");
    for (uint32_t i = 0; i < cf.size(); ++i) {
      const auto& c = cf[i];
      Line(fmt::format("case {}:", i));
      ++indent_;
      switch (c.opcode()) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd:
          emit_exec(c, true);
          break;
        case ControlFlowOpcode::kLoopStart: {
          const auto& ls = c.loop_start;
          uint32_t id = ls.loop_id();
          // Push the loop counter and aL.
          Line("for (int k = 3; k > 0; --k) { xe_loop_count[k] = xe_loop_count[k - 1]; "
               "xe_loop_al[k] = xe_loop_al[k - 1]; }");
          Line(fmt::format("{{ uint lc = xe_draw.loops[{}][{}];", id >> 2, id & 3));
          Line("  xe_loop_count[0] = int(lc & 0xFFu);");
          if (!ls.is_repeat()) {
            Line("  aL = int((lc >> 8) & 0xFFu);");
          }
          Line("  xe_loop_al[0] = aL; }");
          Line(fmt::format("if (xe_loop_count[0] == 0) {{ for (int k = 0; k < 3; ++k) {{ "
                           "xe_loop_count[k] = xe_loop_count[k + 1]; xe_loop_al[k] = "
                           "xe_loop_al[k + 1]; }} aL = xe_loop_al[0]; pc = {}; continue; }}",
                           ls.address()));
        } break;
        case ControlFlowOpcode::kLoopEnd: {
          const auto& le = c.loop_end;
          uint32_t id = le.loop_id();
          Line("xe_loop_count[0] -= 1;");
          std::string cont = "xe_loop_count[0] != 0";
          if (le.is_predicated_break()) {
            cont = fmt::format("({} && p0 != {})", cont, le.condition() ? "true" : "false");
          }
          Line(fmt::format("if ({}) {{", cont));
          Line(fmt::format("  aL += bitfieldExtract(int(xe_draw.loops[{}][{}]), 16, 8);", id >> 2,
                           id & 3));
          Line("  xe_loop_al[0] = aL;");
          Line(fmt::format("  pc = {}; continue;", le.address()));
          Line("}");
          Line("for (int k = 0; k < 3; ++k) { xe_loop_count[k] = xe_loop_count[k + 1]; "
               "xe_loop_al[k] = xe_loop_al[k + 1]; }");
          Line("xe_loop_count[3] = 0; xe_loop_al[3] = 0; aL = xe_loop_al[0];");
        } break;
        case ControlFlowOpcode::kCondCall: {
          const auto& cc = c.cond_call;
          std::string cond = "true";
          if (!cc.is_unconditional()) {
            cond = cc.is_predicated()
                       ? fmt::format("(p0 == {})", cc.condition() ? "true" : "false")
                       : BoolCondition(cc.bool_address(), cc.condition());
          }
          Line(fmt::format("if ({}) {{ if (xe_csp < 4) {{ xe_call_stack[xe_csp] = {}; ++xe_csp; }} "
                           "pc = {}; continue; }}",
                           cond, i + 1, cc.address()));
        } break;
        case ControlFlowOpcode::kReturn:
          Line("if (xe_csp > 0) { --xe_csp; pc = xe_call_stack[xe_csp]; continue; }");
          break;
        case ControlFlowOpcode::kCondJmp: {
          const auto& cj = c.cond_jmp;
          std::string cond = "true";
          if (!cj.is_unconditional()) {
            cond = cj.is_predicated()
                       ? fmt::format("(p0 == {})", cj.condition() ? "true" : "false")
                       : BoolCondition(cj.bool_address(), cj.condition());
          }
          Line(fmt::format("if ({}) {{ pc = {}; continue; }}", cond, cj.address()));
        } break;
        default:
          break;
      }
      --indent_;
    }
    Line("default:");
    Line("  pc = -1; break;");
    Line("}");
    Line("break;");
    --indent_;
    Line("}");
  }

  result_.temp_count = uses_relative_temps_ ? 64 : std::max<uint32_t>(max_temp_, 1);
  result_.float_constant_count = float_const_relative_ ? 256 : max_const_;
  result_.body = std::move(out_);
  result_.valid = true;
  return result_;
}

// Helpers shared by both stages.
const char* kCommonHelpers = R"(
float xe_mul1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xe_mul(vec4 a, vec4 b) {
  bvec4 z = bvec4(vec4(equal(a, vec4(0.0))) + vec4(equal(b, vec4(0.0))));
  return mix(a * b, vec4(0.0), z);
}
float xe_dot4(vec4 a, vec4 b) { vec4 m = xe_mul(a, b); return ((m.x + m.y) + m.z) + m.w; }
float xe_dot3(vec3 a, vec3 b) { vec4 m = xe_mul(vec4(a, 0.0), vec4(b, 0.0)); return (m.x + m.y) + m.z; }
float xe_max4(vec4 v) {
  if (v.x >= v.y && v.x >= v.z && v.x >= v.w) return v.x;
  if (v.y >= v.z && v.y >= v.w) return v.y;
  if (v.z >= v.w) return v.z;
  return v.w;
}
float xe_logc(float a) { float t = a == 1.0 ? 0.0 : log2(a); return isinf(t) && t < 0.0 ? -3.402823466e+38 : t; }
float xe_clampmax(float t) { return isinf(t) ? (t < 0.0 ? -3.402823466e+38 : 3.402823466e+38) : t; }
float xe_clampff(float t) { return isinf(t) ? (t < 0.0 ? -0.0 : 0.0) : t; }
vec4 xe_cube(vec4 s) {
  float x = s.z, y = s.w, z = s.x;
  vec3 a = abs(vec3(x, y, z));
  float tc, sc, ma, id;
  if (a.z >= a.x && a.z >= a.y) {
    tc = -y; sc = z < 0.0 ? -x : x; ma = 2.0 * z; id = z < 0.0 ? 5.0 : 4.0;
  } else if (a.y >= a.x) {
    tc = y < 0.0 ? -z : z; sc = x; ma = 2.0 * y; id = y < 0.0 ? 3.0 : 2.0;
  } else {
    tc = -y; sc = x < 0.0 ? z : -z; ma = 2.0 * x; id = x < 0.0 ? 1.0 : 0.0;
  }
  return vec4(tc, sc, ma, id);
}
vec3 xe_cube_dir(vec3 c) {
  float s = c.x * 2.0 - 3.0;
  float t = c.y * 2.0 - 3.0;
  uint face = uint(clamp(c.z, 0.0, 5.0));
  uint axis = face >> 1u;
  bool neg = (face & 1u) != 0u;
  float sgn = neg ? -1.0 : 1.0;
  if (axis == 0u) return vec3(sgn, -t, neg ? s : -s);
  if (axis == 1u) return vec3(s, sgn, neg ? -t : t);
  return vec3(neg ? -s : s, -t, sgn);
}
vec4 xe_tex_post(vec4 v, uint fi) {
  uint signs = xe_draw.tex_info[fi].x;
  if (signs != 0u) {
    for (int c = 0; c < 4; ++c) {
      uint m = (signs >> (uint(c) * 2u)) & 3u;
      if (m == 1u) {
        // Signed: the host view is unsigned normalized 8 bits per component.
        float b = floor(v[c] * 255.0 + 0.5);
        v[c] = max((b >= 128.0 ? b - 256.0 : b) / 127.0, -1.0);
      } else if (m == 2u) {
        v[c] = v[c] * 2.0 - 1.0;
      } else if (m == 3u) {
        v[c] = v[c] <= 0.04045 ? v[c] / 12.92 : pow((v[c] + 0.055) / 1.055, 2.4);
      }
    }
  }
  return v * xe_draw.tex_size[fi].w;
}
)";

const char* kUniformDecls = R"(
layout(std140, set = 0, binding = 0) uniform XeVsConsts { vec4 c[256]; } xe_vsc;
layout(std140, set = 0, binding = 1) uniform XePsConsts { vec4 c[256]; } xe_psc;
layout(std140, set = 0, binding = 2) uniform XeDraw {
  uvec4 bools[2];
  uvec4 loops[8];
  vec4 ndc_scale;
  vec4 ndc_offset;
  uvec4 vtx;
  uvec4 vtx2;
  vec4 alpha_test;
  vec4 point;
  vec4 pixel_pos;
  uvec4 vfetch[48];
  vec4 tex_size[32];
  uvec4 tex_info[32];
  vec4 tex_uv[32];
} xe_draw;
)";

std::string TextureDecls(const TranslatedShader& s, uint32_t set) {
  std::string out;
  const char* prefix = s.type == xenos::ShaderType::kVertex ? "xe_vt" : "xe_pt";
  for (const auto& t : s.textures) {
    switch (t.dimension) {
      case xenos::FetchOpDimension::k1D:
      case xenos::FetchOpDimension::k2D:
        out += fmt::format("layout(set = {}, binding = {}) uniform sampler2D {}{};\n", set,
                           t.binding, prefix, t.binding);
        break;
      case xenos::FetchOpDimension::k3DOrStacked:
        out += fmt::format("layout(set = {}, binding = {}) uniform sampler3D {}{};\n", set,
                           t.binding, prefix, t.binding);
        out += fmt::format("layout(set = {}, binding = {}) uniform sampler2DArray {}{};\n", set,
                           t.binding + 1, prefix, t.binding + 1);
        break;
      case xenos::FetchOpDimension::kCube:
        out += fmt::format("layout(set = {}, binding = {}) uniform samplerCube {}{};\n", set,
                           t.binding, prefix, t.binding);
        break;
    }
  }
  return out;
}

std::string LocalDecls(const TranslatedShader& s) {
  return fmt::format(
      "  vec4 r[{}];\n"
      "  for (int i = 0; i < {}; ++i) r[i] = vec4(0.0);\n"
      "  bool p0 = false; bool xe_np = false;\n"
      "  int a0 = 0; int xe_na0 = 0; int aL = 0;\n"
      "  float ps = 0.0;\n"
      "  float xe_lod = 0.0; vec3 xe_grad_h = vec3(0.0); vec3 xe_grad_v = vec3(0.0);\n"
      "  int xe_vf_addr = 0; uint xe_vf_endian = 0u; vec4 xe_vtmp = vec4(0.0);\n",
      s.temp_count, s.temp_count);
}

}  // namespace

TranslatedShader TranslateShader(xenos::ShaderType type, const uint32_t* ucode,
                                 uint32_t dword_count) {
  Translator t(type, ucode, dword_count);
  return t.Run();
}

std::string BuildVertexShaderGlsl(const TranslatedShader& vs, const VertexShaderVariant& v) {
  std::string g = "#version 450\n";
  g += kUniformDecls;
  g += "layout(std430, set = 0, binding = 3) readonly buffer XeVtx { uint d[]; } xe_vtx;\n";
  g += "layout(std430, set = 0, binding = 4) readonly buffer XeArena { uint d[]; } xe_arena;\n";
  g += TextureDecls(vs, 1);
  g += kCommonHelpers;
  // Vertex words are stored as in guest memory (big-endian); swap per the
  // fetch constant endianness.
  g += R"(
uint xe_swap(uint v, uint e) {
  if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
  if (e == 3u) return (v << 16) | (v >> 16);
  return v;
}
)";
  for (uint32_t i = 0; i < v.interpolator_count; ++i) {
    g += fmt::format("layout(location = {}) out vec4 xe_out{};\n", i, i);
  }
  g += "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };\n";
  g += R"(
vec4 xe_e_pos;
vec4 xe_e_interp[16];
vec4 xe_e_misc;
)";
  // The guest program as a function of the vertex index.
  std::string fn = "void xe_shader(float xe_vertex_index) {\n";
  fn += LocalDecls(vs);
  fn += "  r[0].x = xe_vertex_index;\n";
  fn += "  xe_e_pos = vec4(0.0, 0.0, 0.0, 1.0);\n";
  fn += "  for (int i = 0; i < 16; ++i) xe_e_interp[i] = vec4(0.0);\n";
  fn += "  xe_e_misc = vec4(0.0);\n";
  fn += "#define xe_vword(a) xe_swap(((xe_vf_endian & 0x80000000u) != 0u ? xe_arena.d[max(a, 0)] : xe_vtx.d[max(a, 0)]), xe_vf_endian & 3u)\n";
  fn += vs.body;
  fn += "#undef xe_vword\n";
  fn += "}\n";
  g += fn;
  // Guest index -> float index as the Xenos VGT computes it: offset, clamp.
  g += R"(
float xe_guest_index(uint host_index) {
  uint i = (host_index + xe_draw.vtx.y) & 0xFFFFFFu;
  i = clamp(i, xe_draw.vtx.z, xe_draw.vtx.w);
  return float(i);
}
vec4 xe_host_position(vec4 p) {
  // PA_CL_VTE_CNTL: xy/z already divided by W, W given as 1/W.
  uint vte = xe_draw.vtx.x;
  if ((vte & 4u) == 0u) p.w = 1.0 / p.w;
  if ((vte & 1u) != 0u) p.xy *= p.w;
  if ((vte & 2u) != 0u) p.z *= p.w;
  // Guest viewport transform and window offset folded into host NDC.
  p.xyz = p.xyz * xe_draw.ndc_scale.xyz + xe_draw.ndc_offset.xyz * p.w;
  return p;
}
)";
  g += "void main() {\n";
  if (!v.rect_list) {
    g += "  xe_shader(xe_guest_index(uint(gl_VertexIndex)));\n";
    g += "  gl_Position = xe_host_position(xe_e_pos);\n";
    for (uint32_t i = 0; i < v.interpolator_count; ++i) {
      g += fmt::format("  xe_out{} = xe_e_interp[{}];\n", i, i);
    }
  } else {
    // Rectangle list: 6 host vertices per rectangle. Run the guest shader on
    // the 3 guest vertices, then mirror the vertex opposite the longest edge.
    g += R"(
  uint rect = uint(gl_VertexIndex) / 6u;
  uint corner = uint(gl_VertexIndex) % 6u;
  vec4 pos[3];
  vec4 interp[3][16];
  for (uint k = 0u; k < 3u; ++k) {
    uint host_index = xe_draw.vtx2.x != 0xFFFFFFFFu ? xe_vtx.d[xe_draw.vtx2.x + rect * 3u + k]
                                                    : rect * 3u + k;
    xe_shader(xe_guest_index(host_index));
    pos[k] = xe_host_position(xe_e_pos);
    for (int i = 0; i < 16; ++i) interp[k][i] = xe_e_interp[i];
  }
  vec2 p0 = pos[0].xy / pos[0].w, p1 = pos[1].xy / pos[1].w, p2 = pos[2].xy / pos[2].w;
  float e12 = dot(p2 - p1, p2 - p1), e20 = dot(p0 - p2, p0 - p2), e01 = dot(p1 - p0, p1 - p0);
  uint first = (e12 > e20 && e12 > e01) ? 0u : ((e20 > e01) ? 1u : 2u);
  uint s0 = first, s1 = (first + 1u) % 3u, s2 = (first + 2u) % 3u;
  // Strip s0 s1 s2 s3 as triangles (s0 s1 s2) and (s2 s1 s3).
  uint order[6] = uint[6](0u, 1u, 2u, 2u, 1u, 3u);
  uint which = order[corner];
  vec4 out_pos;
  vec4 out_interp[16];
  if (which == 3u) {
    out_pos = pos[s1] + pos[s2] - pos[s0];
    for (int i = 0; i < 16; ++i) out_interp[i] = interp[s1][i] + interp[s2][i] - interp[s0][i];
  } else {
    uint src = which == 0u ? s0 : (which == 1u ? s1 : s2);
    out_pos = pos[src];
    for (int i = 0; i < 16; ++i) out_interp[i] = interp[src][i];
  }
  gl_Position = out_pos;
)";
    for (uint32_t i = 0; i < v.interpolator_count; ++i) {
      g += fmt::format("  xe_out{} = out_interp[{}];\n", i, i);
    }
  }
  g += "  gl_PointSize = 1.0;\n";
  g += "}\n";
  return g;
}

std::string BuildPixelShaderGlsl(const TranslatedShader& ps, const PixelShaderVariant& v) {
  std::string g = "#version 450\n";
  g += kUniformDecls;
  g += TextureDecls(ps, 2);
  g += kCommonHelpers;
  for (uint32_t i = 0; i < v.interpolator_count; ++i) {
    bool flat = (v.flat_mask >> i) & 1;
    g += fmt::format("layout(location = {}) {}in vec4 xe_in{};\n", i, flat ? "flat " : "", i);
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (ps.colors_written & (1u << i)) {
      g += fmt::format("layout(location = {}) out vec4 xe_color{};\n", i, i);
    }
  }
  g += "vec4 xe_e_color[4];\nvec4 xe_e_depth;\n";
  g += "void main() {\n";
  g += LocalDecls(ps);
  for (uint32_t i = 0; i < v.interpolator_count && i < ps.temp_count; ++i) {
    g += fmt::format("  r[{}] = xe_in{};\n", i, i);
  }
  if (v.param_gen && v.param_gen_index < ps.temp_count) {
    // Screen position (pixel index, D3D convention), front face in the sign of
    // X, point/line flags in the signs of Y/Z (not points or lines here).
    g += fmt::format(
        "  {{ vec2 xy = gl_FragCoord.xy * xe_draw.pixel_pos.xy + xe_draw.pixel_pos.zw;\n"
        "    r[{}] = vec4(gl_FrontFacing ? xy.x : -xy.x, xy.y, 0.0, 0.0); }}\n",
        v.param_gen_index);
  }
  g += "  for (int i = 0; i < 4; ++i) xe_e_color[i] = vec4(0.0);\n";
  g += "  xe_e_depth = vec4(gl_FragCoord.z);\n";
  g += ps.body;
  // Alpha test against color 0.
  g += R"(
  if (xe_draw.alpha_test.z != 0.0) {
    float a = xe_e_color[0].a, ref = xe_draw.alpha_test.x;
    uint f = uint(xe_draw.alpha_test.y);
    bool pass = f == 7u || (f == 1u && a < ref) || (f == 2u && a == ref) || (f == 3u && a <= ref) ||
                (f == 4u && a > ref) || (f == 5u && a != ref) || (f == 6u && a >= ref);
    if (!pass) discard;
  }
)";
  for (uint32_t i = 0; i < 4; ++i) {
    if (ps.colors_written & (1u << i)) {
      g += fmt::format("  xe_color{} = xe_e_color[{}];\n", i, i);
    }
  }
  if (ps.writes_depth) {
    g += "  gl_FragDepth = clamp(xe_e_depth.x, 0.0, 1.0);\n";
  }
  g += "}\n";
  return g;
}

std::string BuildNullPixelShaderGlsl() { return "#version 450\nvoid main() {}\n"; }

}  // namespace carbon::gpu
