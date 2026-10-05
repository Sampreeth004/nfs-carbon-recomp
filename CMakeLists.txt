# nfscarbon - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nfscarbon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Cross-compiling (Android): codegen runs at build time, so it must execute on
# the host. Point the imported rexglue target at a native build.
if(ANDROID AND REXGLUE_HOST_REXGLUE_TOOL)
    set_target_properties(rex::rexglue PROPERTIES
        IMPORTED_LOCATION "${REXGLUE_HOST_REXGLUE_TOOL}"
        IMPORTED_LOCATION_RELEASE "${REXGLUE_HOST_REXGLUE_TOOL}"
    )
endif()

# Sources
set(NFSCARBON_SOURCES
    src/main.cpp
    src/dispatch_tolerance.cpp
)

if(WIN32)
    list(APPEND NFSCARBON_SOURCES src/crash_diag.cpp)
endif()

if(ANDROID)
    # JNI bridge for the on-screen touch gamepad overlay.
    list(APPEND NFSCARBON_SOURCES src/touch_bridge.cpp)
    # CPU core pinning watchdog for the command processor and main guest thread.
    list(APPEND NFSCARBON_SOURCES src/thread_affinity.cpp)
    # Futex sleep instead of a busy spin while D3D waits for ring space.
    list(APPEND NFSCARBON_SOURCES src/ring_wait_hook.cpp)
    # Retail render mode 2 (one tile, no MSAA) for the scene: removes the tiling/MSAA redraws.
    list(APPEND NFSCARBON_SOURCES src/render_mode_hook.cpp)
endif()

if(WIN32)
    add_executable(nfscarbon WIN32 ${NFSCARBON_SOURCES})
    target_link_libraries(nfscarbon PRIVATE dbghelp)
elseif(ANDROID)
    # SDLActivity loads the game as a shared library.
    add_library(nfscarbon SHARED ${NFSCARBON_SOURCES})
else()
    add_executable(nfscarbon ${NFSCARBON_SOURCES})
endif()

if(WIN32)
    target_link_options(nfscarbon PRIVATE "LINKER:/force:multiple")
endif()

rexglue_setup_target(nfscarbon GPU_PLUGINS xenos)

if(ANDROID)
    # The NDK defaults to per-function sections; with ~72k recompiled functions
    # that overflows ELF's 16-bit section count and breaks strip/dlopen. Merge
    # them back into one .text/.data per object.
    target_compile_options(nfscarbon PRIVATE -fno-function-sections -fno-data-sections)
    # Bind guest-to-guest calls inside the library directly. Otherwise every
    # call to an exported sub_XXXXXXXX goes through the PLT (~19.8k slots).
    # Hooks still win: the static linker picks strong definitions first.
    target_link_options(nfscarbon PRIVATE "LINKER:-Bsymbolic-functions")
    if(TARGET nfscarbon_recomp)
        target_compile_options(nfscarbon_recomp PRIVATE
            -fno-function-sections -fno-data-sections)
    endif()
endif()
