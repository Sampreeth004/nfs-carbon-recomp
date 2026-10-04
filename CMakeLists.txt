# nfscarbon - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nfscarbon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NFSCARBON_SOURCES
    src/main.cpp
)

if(WIN32)
    add_executable(nfscarbon WIN32 ${NFSCARBON_SOURCES})
else()
    add_executable(nfscarbon ${NFSCARBON_SOURCES})
endif()

rexglue_setup_target(nfscarbon GPU_PLUGINS xenos)
