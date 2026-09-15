cmake_minimum_required(VERSION 3.20)
project(xenia-host-tools LANGUAGES C CXX)

# Standalone native build for cross-compilation: no emulator or desktop UI deps.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
get_filename_component(XENIA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV-Headers_SOURCE_DIR "${XENIA_ROOT}/third_party/SPIRV-Headers")
add_subdirectory("${XENIA_ROOT}/third_party/SPIRV-Tools" spirv-tools EXCLUDE_FROM_ALL)

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT ON CACHE BOOL "" FORCE)
set(ALLOW_EXTERNAL_SPIRV_TOOLS ON CACHE BOOL "" FORCE)
add_subdirectory("${XENIA_ROOT}/third_party/glslang" glslang EXCLUDE_FROM_ALL)

add_executable(xenia-shader-cc shader_cc.cc)
target_include_directories(xenia-shader-cc PRIVATE
  "${XENIA_ROOT}/third_party/glslang"
  "${XENIA_ROOT}/third_party/glslang/StandAlone")
target_link_libraries(xenia-shader-cc PRIVATE
  glslang SPIRV glslang-default-resource-limits SPIRV-Tools-opt SPIRV-Tools-static)
