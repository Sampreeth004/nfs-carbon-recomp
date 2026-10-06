// Carbon native renderer: GLSL -> SPIR-V.

#include "shader_compiler.h"

#include <fstream>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>

#include "gpu_common.h"

namespace carbon::gpu {

namespace {

// Bump when the generated GLSL conventions change, to invalidate old caches.
constexpr uint32_t kCacheVersion = 1;

TBuiltInResource MakeResources() {
  TBuiltInResource r = {};
  r.maxLights = 32;
  r.maxClipPlanes = 6;
  r.maxTextureUnits = 32;
  r.maxTextureCoords = 32;
  r.maxVertexAttribs = 64;
  r.maxVertexUniformComponents = 4096;
  r.maxVaryingFloats = 64;
  r.maxVertexTextureImageUnits = 32;
  r.maxCombinedTextureImageUnits = 80;
  r.maxTextureImageUnits = 32;
  r.maxFragmentUniformComponents = 4096;
  r.maxDrawBuffers = 32;
  r.maxVertexUniformVectors = 128;
  r.maxVaryingVectors = 8;
  r.maxFragmentUniformVectors = 16;
  r.maxVertexOutputVectors = 16;
  r.maxFragmentInputVectors = 15;
  r.minProgramTexelOffset = -8;
  r.maxProgramTexelOffset = 7;
  r.maxClipDistances = 8;
  r.maxComputeWorkGroupCountX = 65535;
  r.maxComputeWorkGroupCountY = 65535;
  r.maxComputeWorkGroupCountZ = 65535;
  r.maxComputeWorkGroupSizeX = 1024;
  r.maxComputeWorkGroupSizeY = 1024;
  r.maxComputeWorkGroupSizeZ = 64;
  r.maxComputeUniformComponents = 1024;
  r.maxComputeTextureImageUnits = 16;
  r.maxComputeImageUniforms = 8;
  r.maxComputeAtomicCounters = 8;
  r.maxComputeAtomicCounterBuffers = 1;
  r.maxVaryingComponents = 60;
  r.maxVertexOutputComponents = 64;
  r.maxGeometryInputComponents = 64;
  r.maxGeometryOutputComponents = 128;
  r.maxFragmentInputComponents = 128;
  r.maxImageUnits = 8;
  r.maxCombinedImageUnitsAndFragmentOutputs = 8;
  r.maxCombinedShaderOutputResources = 8;
  r.maxImageSamples = 0;
  r.maxVertexImageUniforms = 0;
  r.maxTessControlImageUniforms = 0;
  r.maxTessEvaluationImageUniforms = 0;
  r.maxGeometryImageUniforms = 0;
  r.maxFragmentImageUniforms = 8;
  r.maxCombinedImageUniforms = 8;
  r.maxGeometryTextureImageUnits = 16;
  r.maxGeometryOutputVertices = 256;
  r.maxGeometryTotalOutputComponents = 1024;
  r.maxGeometryUniformComponents = 1024;
  r.maxGeometryVaryingComponents = 64;
  r.maxTessControlInputComponents = 128;
  r.maxTessControlOutputComponents = 128;
  r.maxTessControlTextureImageUnits = 16;
  r.maxTessControlUniformComponents = 1024;
  r.maxTessControlTotalOutputComponents = 4096;
  r.maxTessEvaluationInputComponents = 128;
  r.maxTessEvaluationOutputComponents = 128;
  r.maxTessEvaluationTextureImageUnits = 16;
  r.maxTessEvaluationUniformComponents = 1024;
  r.maxTessPatchComponents = 120;
  r.maxPatchVertices = 32;
  r.maxTessGenLevel = 64;
  r.maxViewports = 16;
  r.maxVertexAtomicCounters = 0;
  r.maxTessControlAtomicCounters = 0;
  r.maxTessEvaluationAtomicCounters = 0;
  r.maxGeometryAtomicCounters = 0;
  r.maxFragmentAtomicCounters = 8;
  r.maxCombinedAtomicCounters = 8;
  r.maxAtomicCounterBindings = 1;
  r.maxVertexAtomicCounterBuffers = 0;
  r.maxTessControlAtomicCounterBuffers = 0;
  r.maxTessEvaluationAtomicCounterBuffers = 0;
  r.maxGeometryAtomicCounterBuffers = 0;
  r.maxFragmentAtomicCounterBuffers = 1;
  r.maxCombinedAtomicCounterBuffers = 1;
  r.maxAtomicCounterBufferSize = 16384;
  r.maxTransformFeedbackBuffers = 4;
  r.maxTransformFeedbackInterleavedComponents = 64;
  r.maxCullDistances = 8;
  r.maxCombinedClipAndCullDistances = 8;
  r.maxSamples = 4;
  r.maxMeshOutputVerticesNV = 256;
  r.maxMeshOutputPrimitivesNV = 512;
  r.maxMeshWorkGroupSizeX_NV = 32;
  r.maxMeshWorkGroupSizeY_NV = 1;
  r.maxMeshWorkGroupSizeZ_NV = 1;
  r.maxTaskWorkGroupSizeX_NV = 32;
  r.maxTaskWorkGroupSizeY_NV = 1;
  r.maxTaskWorkGroupSizeZ_NV = 1;
  r.maxMeshViewCountNV = 4;
  r.limits.nonInductiveForLoops = true;
  r.limits.whileLoops = true;
  r.limits.doWhileLoops = true;
  r.limits.generalUniformIndexing = true;
  r.limits.generalAttributeMatrixVectorIndexing = true;
  r.limits.generalVaryingIndexing = true;
  r.limits.generalSamplerIndexing = true;
  r.limits.generalVariableIndexing = true;
  r.limits.generalConstantMatrixVectorIndexing = true;
  return r;
}

}  // namespace

ShaderCompiler::ShaderCompiler() { glslang::InitializeProcess(); }

ShaderCompiler::~ShaderCompiler() { glslang::FinalizeProcess(); }

void ShaderCompiler::SetCacheDirectory(const std::filesystem::path& dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_dir_ = dir;
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);
}

bool ShaderCompiler::Compile(const std::string& glsl, bool vertex, std::vector<uint32_t>& spirv,
                             std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t hash = HashBytes(glsl.data(), glsl.size(), kCacheVersion);
  std::filesystem::path cache_file;
  if (!cache_dir_.empty()) {
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(hash));
    cache_file = cache_dir_ / name;
    std::ifstream in(cache_file, std::ios::binary | std::ios::ate);
    if (in) {
      auto size = in.tellg();
      if (size > 0 && size % 4 == 0) {
        spirv.resize(size_t(size) / 4);
        in.seekg(0);
        in.read(reinterpret_cast<char*>(spirv.data()), size);
        if (in && spirv[0] == 0x07230203u) {
          return true;
        }
      }
    }
  }

  static const TBuiltInResource kResources = MakeResources();
  EShLanguage stage = vertex ? EShLangVertex : EShLangFragment;
  const char* src = glsl.c_str();
  glslang::TShader shader(stage);
  shader.setStrings(&src, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
  EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kResources, 450, false, messages)) {
    error = shader.getInfoLog();
    return false;
  }
  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error = program.getInfoLog();
    return false;
  }
  glslang::SpvOptions options = {};
  // The SDK's glslang is linked without SPIRV-Tools; drivers optimize anyway.
  options.disableOptimizer = true;
  options.optimizeSize = false;
  spirv.clear();
  glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
  if (spirv.empty()) {
    error = "empty SPIR-V";
    return false;
  }
  if (!cache_file.empty()) {
    std::ofstream out(cache_file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
  }
  return true;
}

}  // namespace carbon::gpu
