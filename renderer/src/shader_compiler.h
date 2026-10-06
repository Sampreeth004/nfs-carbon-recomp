// Carbon native renderer: GLSL -> SPIR-V with glslang, plus an on-disk cache
// keyed by the hash of the GLSL source (so a translated shader is compiled
// once per install, not once per boot).
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace carbon::gpu {

class ShaderCompiler {
 public:
  ShaderCompiler();
  ~ShaderCompiler();

  void SetCacheDirectory(const std::filesystem::path& dir);

  // Returns false and fills `error` if compilation fails.
  bool Compile(const std::string& glsl, bool vertex, std::vector<uint32_t>& spirv,
               std::string& error);

 private:
  std::mutex mutex_;
  std::filesystem::path cache_dir_;
};

}  // namespace carbon::gpu
