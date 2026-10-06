# rexgpu-carbon: native Vulkan renderer for NFS Carbon (no Xenos emulation).
#
# Loaded at runtime through the SDK GPU plugin seam (gpu_plugin = "carbon"),
# so the same sources build as rexgpu-carbon.dll on Windows and
# librexgpu-carbon.so on Android. It links only rexruntime (provider,
# presenter, memory, kernel) plus glslang for runtime GLSL -> SPIR-V.

set(CARBON_GPU_SOURCES
    src/plugin_main.cpp
    src/gpu_system.cpp
    src/command_processor.cpp
    src/shader_translator.cpp
    src/shader_compiler.cpp
    src/renderer.cpp
    src/render_targets.cpp
    src/textures.cpp
    src/texture_decode.cpp
)

add_library(rexgpu-carbon SHARED ${CARBON_GPU_SOURCES})

set_target_properties(rexgpu-carbon PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# glslang's public headers are not part of the SDK install; take them from
# the SDK source tree (same version as the installed static libraries).
set(CARBON_GPU_SDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../reference/rexglue-sdk"
    CACHE PATH "rexglue-sdk source tree (for glslang headers)")
target_include_directories(rexgpu-carbon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CARBON_GPU_SDK_SOURCE_DIR}/thirdparty/glslang
)

target_link_libraries(rexgpu-carbon PRIVATE
    rex::runtime
    rex::renderdoc
    rex::xxhash
    rex::glslang
    rex::SPIRV
    rex::MachineIndependent
    rex::GenericCodeGen
    rex::OSDependent
    rex::OGLCompiler
)

if(WIN32)
    # Place the plugin next to nfscarbon.exe, where LoadGpuPlugin looks.
    set_target_properties(rexgpu-carbon PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_definitions(rexgpu-carbon PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    set_target_properties(rexgpu-carbon PROPERTIES OUTPUT_NAME "rexgpu-carbon")
endif()

if(ANDROID)
    target_link_libraries(rexgpu-carbon PRIVATE log)
endif()
