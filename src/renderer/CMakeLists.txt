# predator_renderer: the native renderer (D3D high-level emulation with pre-translated shaders).
#
# Two ways in:
#   - from the project: cmake/fcp_renderer.cmake (one line in the root CMakeLists.txt), built with build.cmd;
#   - standalone: cmake -S src/renderer -B out/build/renderer-<cfg> (tools/renderer_tests.cmd), without SDK and codegen.
#
# Targets:
#   predator_renderer_resources  King Kong's resource building blocks (kk/resources: tiling, formats, endian, conversion)
#   predator_renderer            interfaces (include/renderer), host devices, ShaderLibrary, caches, render thread
#   predator_renderer_tests      own tests + King Kong's backend tests (no game, no GPU except the "gpu" cases)
#   predator_renderer_res_tests  King Kong's resource tests
#   renderer_disc_tests          tests with real data from the disc (skipped without work\disc)
#   renderer_probe               window, triangle, quad with a translated shader pair and a texture from the game data
#
# Dependencies (pinned, tools/get_renderer_deps.ps1 -> FCP_RENDERER_DEPS_DIR): NVRHI in King Kong's version and
# DirectX-Headers (both in the King Kong clone), Vulkan-Headers, fmt, xxHash, DXC; toml++ from the SDK dependencies
# (FCP_DEPS_DIR, tools/get_deps.ps1).
cmake_minimum_required(VERSION 3.25)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(predator_renderer LANGUAGES C CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  set(FCP_RENDERER_STANDALONE ON)
else ()
  set(FCP_RENDERER_STANDALONE OFF)
endif ()

get_filename_component(FCP_REPO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

function (fcp_renderer_path name fallback doc)
  if (NOT DEFINED CACHE{${name}})
    if (DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
      set(value "$ENV{${name}}")
    else ()
      set(value "${fallback}")
    endif ()
    file(TO_CMAKE_PATH "${value}" value)
    set(${name} "${value}" CACHE PATH "${doc}")
  endif ()
endfunction ()
fcp_renderer_path(FCP_RENDERER_DEPS_DIR "${FCP_REPO_DIR}/third_party/renderer-deps" "tools/get_renderer_deps.ps1")
fcp_renderer_path(FCP_DEPS_DIR "${FCP_REPO_DIR}/third_party/rexglue-deps" "tools/get_deps.ps1 (toml++)")
# Shared root (same rule as tools/shared_root.cmd): FCP_SHARED_ROOT if set, else the main checkout of a Git
# worktree (parent of the common Git dir), else this repository. The unpacked disc is expected in <root>/work/disc.
if (DEFINED ENV{FCP_SHARED_ROOT} AND NOT "$ENV{FCP_SHARED_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{FCP_SHARED_ROOT}" _fcp_shared_root)
else ()
  set(_fcp_shared_root "${FCP_REPO_DIR}")
  find_package(Git QUIET)
  if (GIT_EXECUTABLE)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" -C "${FCP_REPO_DIR}" rev-parse --path-format=absolute --git-common-dir
      OUTPUT_VARIABLE _fcp_git_common
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _fcp_git_rc
      ERROR_QUIET
    )
    if (_fcp_git_rc EQUAL 0 AND NOT "${_fcp_git_common}" STREQUAL "")
      get_filename_component(_fcp_shared_root "${_fcp_git_common}/.." ABSOLUTE)
    endif ()
  endif ()
endif ()
fcp_renderer_path(FCP_DISC_DIR "${_fcp_shared_root}/work/disc"
                  "Unpacked disc for renderer_disc_tests (read only; the tests are skipped if it is missing)")
if (NOT EXISTS "${FCP_DISC_DIR}/common.fat")
  message(STATUS "predator_renderer: no unpacked disc in FCP_DISC_DIR (${FCP_DISC_DIR}); renderer_disc_tests "
                 "will skip. Set FCP_DISC_DIR or FCP_SHARED_ROOT to enable them.")
endif ()

set(_kk "${FCP_RENDERER_DEPS_DIR}/king-kong-recompiled/native-renderer")
set(_nvrhi "${_kk}/thirdparty/nvrhi")
foreach (f "${_nvrhi}/CMakeLists.txt" "${FCP_RENDERER_DEPS_DIR}/Vulkan-Headers/include/vulkan/vulkan.hpp"
         "${FCP_DEPS_DIR}/tomlplusplus/include/toml++/toml.hpp"
)
  if (NOT EXISTS "${f}")
    message(FATAL_ERROR "missing: ${f}\n(powershell -NoProfile -ExecutionPolicy Bypass -File tools\\get_renderer_deps.ps1, "
                        "or tools\\get_deps.ps1 for toml++)"
    )
  endif ()
endforeach ()

# --- NVRHI (King Kong's patched version, MIT), Vulkan and D3D12, nothing is downloaded -----------------------
if (NOT TARGET Vulkan::Headers)
  add_library(fcp_vulkan_headers INTERFACE)
  target_include_directories(fcp_vulkan_headers SYSTEM INTERFACE "${FCP_RENDERER_DEPS_DIR}/Vulkan-Headers/include")
  add_library(Vulkan::Headers ALIAS fcp_vulkan_headers)
endif ()
if (NOT TARGET Microsoft::DirectX-Headers)
  add_library(fcp_directx_headers INTERFACE)
  target_include_directories(fcp_directx_headers SYSTEM INTERFACE "${_kk}/thirdparty/DirectX-Headers/include")
  add_library(Microsoft::DirectX-Headers ALIAS fcp_directx_headers)
  add_library(fcp_directx_guids STATIC "${_kk}/thirdparty/DirectX-Headers/src/dxguids.cpp")
  target_link_libraries(fcp_directx_guids PUBLIC fcp_directx_headers dxguid)
  add_library(Microsoft::DirectX-Guids ALIAS fcp_directx_guids)
endif ()
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_RTXMU OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_AFTERMATH OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_NVAPI OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_DIRECTX_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory("${_nvrhi}" "${CMAKE_CURRENT_BINARY_DIR}/nvrhi" EXCLUDE_FROM_ALL)
foreach (t nvrhi nvrhi_vk nvrhi_d3d12)
  if (TARGET ${t} AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${t} PRIVATE -w)  # third-party code: its warnings are not ours
  endif ()
endforeach ()

# --- fcshaders (tools/shaders) as a library -------------------------------------------------------------------
if (NOT TARGET fcshaders)
  add_subdirectory("${FCP_REPO_DIR}/tools/shaders" "${CMAKE_CURRENT_BINARY_DIR}/fcshaders")
endif ()

set(_warn)
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(_warn -Wall -Wno-missing-field-initializers -Wno-unused-function -Wno-sign-compare -Wno-unused-lambda-capture)
endif ()

# --- King Kong's resource building blocks -------------------------------------------------------------------
add_library(
  predator_renderer_resources STATIC
  kk/resources/xenos.cpp
  kk/resources/tiling.cpp
  kk/resources/guest_texture.cpp
  kk/resources/endian.cpp
  kk/resources/texture_convert.cpp
  kk/resources/texture_workers.cpp
  kk/resources/buffers.cpp
  kk/resources/texture_cache.cpp
  kk/resources/render_targets.cpp
  kk/resources/resolve.cpp
  kk/resources/image_io.cpp
)
target_include_directories(predator_renderer_resources PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(predator_renderer_resources SYSTEM PRIVATE "${FCP_RENDERER_DEPS_DIR}/xxHash")
target_compile_definitions(predator_renderer_resources PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(predator_renderer_resources PRIVATE ${_warn})
target_link_libraries(predator_renderer_resources PUBLIC windowscodecs ole32)

# --- The renderer ------------------------------------------------------------------------------------------
add_library(
  predator_renderer STATIC
  core/guest_device.cpp
  core/nr_api.cpp
  core/shader_library.cpp
  core/diagnostics.cpp
  core/translated_layout.cpp
  core/resource_cache.cpp
  core/target_cache.cpp
  core/builtin_shaders.cpp
  core/overlay_pass.cpp
  core/smaa_pass.cpp
  core/game_renderer.cpp
  core/host_vulkan.cpp
  core/host_d3d12.cpp
  kk/backend/pipeline_cache.cpp
  kk/backend/render_thread.cpp
  kk/backend/vk_pipeline_cache.cpp
  kk/backend/vk_pipeline_library.cpp
  kk/backend/vulkan_dispatch.cpp
  kk/backend/file_lock.cpp
)
target_include_directories(predator_renderer PUBLIC include "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(predator_renderer SYSTEM PRIVATE "${FCP_DEPS_DIR}/tomlplusplus/include")
target_compile_definitions(predator_renderer PUBLIC VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(predator_renderer PRIVATE ${_warn})
target_link_libraries(
  predator_renderer PUBLIC predator_renderer_resources fcshaders nvrhi_vk nvrhi_d3d12 nvrhi Vulkan::Headers
                           Microsoft::DirectX-Headers d3d12 dxgi
)

# At run time DXC sits next to the programs (NCSA licence; dxil.dll is not needed).
set(FCP_DXC_DLL "${FCP_RENDERER_DEPS_DIR}/dxc/bin/x64/dxcompiler.dll")
function (fcp_renderer_runtime target)
  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${FCP_DXC_DLL}" "$<TARGET_FILE_DIR:${target}>"
    VERBATIM
  )
  target_compile_options(${target} PRIVATE ${_warn})
  target_compile_definitions(${target} PRIVATE FCP_REPO_DIR="${FCP_REPO_DIR}" FCP_DISC_DIR="${FCP_DISC_DIR}")
endfunction ()

# --- Tests -------------------------------------------------------------------------------------------------
add_executable(
  predator_renderer_tests
  tests/test_main.cpp
  tests/interface_tests.cpp
  tests/shader_pack_tests.cpp
  tests/gpu_tests.cpp
  kk/backend/tests/pipeline_cache_tests.cpp
  kk/backend/tests/pipeline_demand_tests.cpp
  kk/backend/tests/background_file_writer_tests.cpp
  kk/backend/tests/vulkan_requirements_tests.cpp
  kk/backend/tests/d3d12_requirements_tests.cpp
  kk/backend/tests/scheduling_tests.cpp
)
target_link_libraries(predator_renderer_tests PRIVATE predator_renderer)
fcp_renderer_runtime(predator_renderer_tests)

add_executable(
  predator_renderer_res_tests
  kk/resources/tests/test_main.cpp
  kk/resources/tests/test_tiling.cpp
  kk/resources/tests/test_roundtrip.cpp
  kk/resources/tests/test_tail_mips.cpp
  kk/resources/tests/test_convert.cpp
  kk/resources/tests/test_convert_workers.cpp
  kk/resources/tests/test_buffers.cpp
  kk/resources/tests/test_cache.cpp
  kk/resources/tests/test_render_targets.cpp
  kk/resources/tests/test_resolve.cpp
  kk/resources/tests/test_nvrhi_format.cpp
  "${_nvrhi}/src/common/format-info.cpp"
)
target_include_directories(predator_renderer_res_tests PRIVATE "${_nvrhi}/include")
target_link_libraries(predator_renderer_res_tests PRIVATE predator_renderer_resources)
target_compile_options(predator_renderer_res_tests PRIVATE ${_warn} -Wno-unused-variable)

add_executable(renderer_disc_tests tests/disc_tests.cpp tests/test_main.cpp tests/disc_util.cpp)
target_link_libraries(renderer_disc_tests PRIVATE predator_renderer)
fcp_renderer_runtime(renderer_disc_tests)

# Console program (prints the measurements) with its own window.
add_executable(renderer_probe probe/renderer_probe.cpp tests/disc_util.cpp)
target_link_libraries(renderer_probe PRIVATE predator_renderer)
fcp_renderer_runtime(renderer_probe)
