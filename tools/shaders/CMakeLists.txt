# fcshaders: Predator shaders (Xbox 360 microcode, container 0x102A0E00/01) to HLSL, DXIL and SPIR-V.
#
# Two ways in:
#   - standalone (tools/shaders/build.cmd): cmake -S tools/shaders -B work/build/fcshaders
#   - as a subdirectory of the renderer (src/renderer/CMakeLists.txt): add_subdirectory(tools/shaders)
# Targets:
#   fcshaders        library: container -> reflection + DXIL/SPIR-V (fcshaders/library.h), pack format and
#                    disk cache (fcshaders/pack.h); DXC is loaded at runtime
#   fcshaders_tool   command line fcshaders.exe (run, pack, translate, compile)
#
# Inputs (pinned, fetched by tools/get_renderer_deps.ps1 into third_party/renderer-deps, gitignored):
#   FCSHADERS_FMT_INCLUDE     fmt/include (header-only)
#   FCSHADERS_XXHASH_INCLUDE  xxHash (xxh3.h)
#   FCSHADERS_DXC_INCLUDE     dxc/inc (dxcapi.h)
cmake_minimum_required(VERSION 3.20)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(fcshaders LANGUAGES CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif ()

if (NOT DEFINED FCP_RENDERER_DEPS_DIR)
  if (DEFINED ENV{FCP_RENDERER_DEPS_DIR} AND NOT "$ENV{FCP_RENDERER_DEPS_DIR}" STREQUAL "")
    set(FCP_RENDERER_DEPS_DIR "$ENV{FCP_RENDERER_DEPS_DIR}")
  else ()
    set(FCP_RENDERER_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/renderer-deps")
  endif ()
endif ()
file(TO_CMAKE_PATH "${FCP_RENDERER_DEPS_DIR}" FCP_RENDERER_DEPS_DIR)
set(FCSHADERS_FMT_INCLUDE "${FCP_RENDERER_DEPS_DIR}/fmt/include" CACHE PATH "fmt include folder")
set(FCSHADERS_XXHASH_INCLUDE "${FCP_RENDERER_DEPS_DIR}/xxHash" CACHE PATH "xxHash folder (xxh3.h)")
set(FCSHADERS_DXC_INCLUDE "${FCP_RENDERER_DEPS_DIR}/dxc/inc" CACHE PATH "DXC include folder (dxcapi.h)")
foreach (f "${FCSHADERS_FMT_INCLUDE}/fmt/core.h" "${FCSHADERS_XXHASH_INCLUDE}/xxh3.h"
         "${FCSHADERS_DXC_INCLUDE}/dxcapi.h"
)
  if (NOT EXISTS "${f}")
    message(FATAL_ERROR "missing: ${f} (powershell -NoProfile -ExecutionPolicy Bypass -File tools\\get_renderer_deps.ps1)")
  endif ()
endforeach ()

add_library(fcshaders_deps INTERFACE)
target_include_directories(fcshaders_deps SYSTEM INTERFACE "${FCSHADERS_FMT_INCLUDE}" "${FCSHADERS_XXHASH_INCLUDE}")
target_compile_definitions(fcshaders_deps INTERFACE FMT_HEADER_ONLY=1 NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if (MSVC)
  target_compile_options(fcshaders_deps INTERFACE /utf-8 /bigobj /EHsc)
endif ()

# Translation core: XenosRecomp (MIT), in the revision by King Kong Recompiled (MIT).
set(XR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xenosrecomp")
add_library(xenosrecomp_core STATIC "${XR_DIR}/shader_recompiler.cpp")
target_include_directories(xenosrecomp_core PUBLIC "${XR_DIR}")
target_link_libraries(xenosrecomp_core PUBLIC fcshaders_deps)
target_precompile_headers(xenosrecomp_core PRIVATE "${XR_DIR}/pch.h")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(xenosrecomp_core PRIVATE -Wno-switch -Wno-unused-variable)
endif ()

# Prelude (hlsl/fc_common.hlsli) embedded as a byte array.
set(FC_PRELUDE "${CMAKE_CURRENT_SOURCE_DIR}/hlsl/fc_common.hlsli")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FC_PRELUDE}")
file(READ "${FC_PRELUDE}" FC_PRELUDE_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FC_PRELUDE_BYTES "${FC_PRELUDE_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp.new"
     "// Generated from hlsl/fc_common.hlsli.\nnamespace fcshaders {\nextern const char kPreludeText[] = {${FC_PRELUDE_BYTES}0x00};\n}\n"
)
configure_file("${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp.new" "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp" COPYONLY)

add_library(
  fcshaders STATIC
  src/container.cpp
  src/translator.cpp
  src/compiler.cpp
  src/library.cpp
  src/pack.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/prelude.cpp"
)
target_include_directories(fcshaders PUBLIC include PRIVATE "${FCSHADERS_DXC_INCLUDE}")
target_link_libraries(fcshaders PUBLIC fcshaders_deps PRIVATE xenosrecomp_core)

add_executable(fcshaders_tool tool/main.cpp)
set_target_properties(fcshaders_tool PROPERTIES OUTPUT_NAME fcshaders)
target_link_libraries(fcshaders_tool PRIVATE fcshaders)
