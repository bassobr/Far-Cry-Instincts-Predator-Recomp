# Far Cry Instincts Predator (Xbox 360) -> Windows x64, static recompilation on the ReXGlue SDK.
#
# Build from scratch: tools\get_deps.ps1, then build.cmd (the SDK copy is described in rexglue/UPSTREAM.md).
# The game code is generated at build time from the user's disc image (FCP_GAME_ISO) into generated/,
# build outputs go to out/. Neither is ever committed.

cmake_minimum_required(VERSION 3.25)
project(predator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Never read or write the CMake user package registry.
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)
set(CMAKE_EXPORT_NO_PACKAGE_REGISTRY ON)

# --- Project paths and switches (prefix FCP_) --------------------------------------------------------

# Helper: a cache path whose default comes from an environment variable, then from a fallback.
function (fcp_path_option name type fallback doc)
  if (NOT DEFINED CACHE{${name}})
    if (DEFINED ENV{${name}} AND NOT "$ENV{${name}}" STREQUAL "")
      set(value "$ENV{${name}}")
    else ()
      set(value "${fallback}")
    endif ()
    file(TO_CMAKE_PATH "${value}" value)
    set(${name}
        "${value}"
        CACHE ${type} "${doc}"
    )
  endif ()
endfunction ()

fcp_path_option(
  FCP_DEPS_DIR PATH "${CMAKE_SOURCE_DIR}/third_party/rexglue-deps"
  "Former git submodules of the ReXGlue SDK (tools/get_deps.ps1); may be a shared read-only copy"
)
fcp_path_option(
  FCP_GAME_ISO FILEPATH
  "${CMAKE_SOURCE_DIR}/assets/Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso"
  "Disc image of the user (read only)"
)
fcp_path_option(
  FCP_TOOLCHAIN_DIR PATH "${CMAKE_SOURCE_DIR}/toolchain" "Pinned toolchain (llvm, venv)"
)
fcp_path_option(
  FCP_PYTHON FILEPATH "${FCP_TOOLCHAIN_DIR}/venv/Scripts/python.exe"
  "Python with pycryptodome for the build steps"
)
set(FCP_GAME_DIR
    "${CMAKE_SOURCE_DIR}/out/game"
    CACHE PATH "Files taken from the disc image (default.xex, decrypted image); game data, never committed"
)
set(FCP_GENERATED_DIR
    "${CMAKE_SOURCE_DIR}/generated"
    CACHE PATH "Codegen output; game code, never committed"
)
option(FCP_COVERAGE_CHECK "Run tools/coverage_check.py after codegen and fail on missing entry points" ON)
# The recompiled game code as predator_code.dll, built by predator.exe (cmake/fcp_game_code.cmake), so the
# executable and a release carry no game code. OFF = linked into predator.exe
# as before (the way back; also for FCP_PGO=generate).
option(FCP_GAME_CODE_DLL "Build the recompiled code as predator_code.dll instead of linking it into predator.exe" ON)
option(FCP_XENOS_GPU "Build the Xenos GPU emulation drivers of the SDK (Vulkan, D3D12)" OFF)
option(FCP_TRACY "Build the SDK with the Tracy profiler client (TracyClient.dll)" OFF)
# Host ISA level of all code. Default x86-64-v3 (AVX2, FMA, BMI2, MOVBE;
# Zen 2 = Steam Deck, Zen 4 = ROG Ally X, Intel Haswell and newer): measured -10 % main thread, -18 % render thread
# CPU, semtest results bit for bit equal to x86-64-v2. -ffp-contract=off: the compiler fuses no a*b+c on its own,
# only explicit fused operations (fmadd, the SDK's __FMA__ paths) use FMA. Back to the original:
# -DFCP_MARCH=x86-64-v2 (or empty = the preset's -march=x86-64-v2).
set(FCP_MARCH
    "x86-64-v3"
    CACHE STRING "Host -march for all code (x86-64-v3 default, x86-64-v2 or empty = preset)"
)
if (FCP_MARCH)
  add_compile_options(-march=${FCP_MARCH} -ffp-contract=off)
endif ()
set(FCP_PGO
    ""
    CACHE STRING "PGO of the recompiled code: empty, generate or use (cmake/fcp_codegen.cmake)"
)
set(FCP_PGO_PROFILE
    "${CMAKE_SOURCE_DIR}/out/pgo/predator.profdata"
    CACHE FILEPATH "Merged profile for FCP_PGO=use"
)

if (NOT EXISTS "${FCP_DEPS_DIR}/sdl3/CMakeLists.txt")
  message(FATAL_ERROR "SDK dependencies missing in ${FCP_DEPS_DIR}. Run "
                      "powershell -NoProfile -ExecutionPolicy Bypass -File tools\\get_deps.ps1"
  )
endif ()
if (NOT EXISTS "${FCP_PYTHON}")
  message(FATAL_ERROR "Python not found: ${FCP_PYTHON} (set FCP_PYTHON or FCP_TOOLCHAIN_DIR)")
endif ()

# --- ReXGlue SDK (frozen copy in rexglue/, see rexglue/UPSTREAM.md) ----------------------------------

set(FCP_REXGLUE_COMMIT 351bc94d8080917509cce83ff23c4a8e881d7dac)
set(REXGLUE_VERSION_OVERRIDE "0.11.0-dev.g351bc94d")
set(REXGLUE_DEPS_DIR "${FCP_DEPS_DIR}")
set(REX_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")
set(REXGLUE_BUILD_TESTS OFF)
set(REXGLUE_BUILD_PPC_TESTS OFF)
set(REXGLUE_REGISTER_PACKAGE OFF)
set(REXGLUE_BUILD_XENOS_HOST_DRIVERS ${FCP_XENOS_GPU})
set(REXGLUE_ENABLE_TRACY ${FCP_TRACY})
add_subdirectory(rexglue)

# Our targets land next to the SDK binaries, so the EXE finds its DLLs and rexglue.exe its own.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")

include(cmake/fcp_codegen.cmake)
include(cmake/fcp_renderer.cmake)
include(cmake/fcp_semtest.cmake)

# --- predator.exe -------------------------------------------------------------------------------------

add_executable(predator WIN32 src/main.cpp)
fcp_setup_title_target(predator)
include(cmake/fcp_boot.cmake)
include(cmake/fcp_game_code.cmake)
include(cmake/fcp_dist.cmake)
# The guest address space sits at 0x100000000 and above; keep the host image and heaps out of the way of
# the high-entropy ASLR range (lessons from FCI). Checked after the link with tools/check_exe.py.
target_link_options(predator PRIVATE "LINKER:/HIGHENTROPYVA:NO")
add_custom_command(
  TARGET predator
  POST_BUILD
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/check_exe.py" $<TARGET_FILE:predator>
          --require-no-high-entropy --imports
  COMMENT "Checking predator.exe (HIGHENTROPYVA, imports)"
  VERBATIM
)
