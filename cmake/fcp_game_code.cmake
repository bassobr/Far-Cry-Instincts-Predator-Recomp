# fcp_game_code.cmake: the recompiled game code as predator_code.dll (built on the player's first start).
#
# predator.exe carries no game code. It
#  - exports its hooks (REX_TITLE_HOOK_EXPORTS, rexglue/UPSTREAM.md change 141) and the kernel/XAM import wrappers
#    (fcp_import_thunks.cpp), which the DLL links from the executable's import library;
#  - imports the originals __imp__<name> of hooked functions from the DLL through a delay-load import library made
#    from the names of the title configuration (tools/boot/gen_game_code_def.py), resolved by fcp::game_code;
#  - carries FCP_GAME_CODE_ABI (cmake/fcp_game_code_abi.cmake), which the DLL must report.
# The DLL is built by predator.exe itself (`--fcp-build-game-code`, src/fcp/game_code_build.cpp), with the same
# compile and link as on the player's first start, from generated/default into bin/game_code.
# Included from the root CMakeLists.txt after fcp_boot.cmake (needs the predator target and FCP_BOOT_GEN_DIR).

get_filename_component(_fcp_llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(FCP_LLD_LINK "${_fcp_llvm_bin}/lld-link.exe")
if (NOT EXISTS "${FCP_LLD_LINK}")
  message(FATAL_ERROR "lld-link.exe not found next to ${CMAKE_CXX_COMPILER}")
endif ()

# Mid-asm hooks would have to ship as sources for the first-start codegen; there are none (checked here).
file(GLOB_RECURSE _fcp_host_sources "${CMAKE_SOURCE_DIR}/src/*.cpp" "${CMAKE_SOURCE_DIR}/src/*.h")
foreach (_f IN LISTS _fcp_host_sources)
  file(STRINGS "${_f}" _midasm REGEX "REX_MIDASM_(HOOK|AT|BRANCH|RETURN_IF)")
  if (_midasm)
    message(FATAL_ERROR "${_f} defines a mid-asm hook; the first-start build of predator_code.dll does not take "
                        "hook sources (cmake/fcp_game_code.cmake)."
    )
  endif ()
endforeach ()

# --- ABI of the game code ------------------------------------------------------------------------------
set(FCP_GAME_CODE_ABI_H "${FCP_BOOT_GEN_DIR}/fcp_game_code_abi.h")
set(_fcp_pgo_hash "none")
if (FCP_PGO STREQUAL "use" AND EXISTS "${FCP_PGO_PROFILE}")
  file(SHA256 "${FCP_PGO_PROFILE}" _fcp_pgo_hash)
endif ()
add_custom_target(
  predator_game_code_abi
  COMMAND
    ${CMAKE_COMMAND} -DFCP_SOURCE_DIR=${CMAKE_SOURCE_DIR} -DFCP_OUT=${FCP_GAME_CODE_ABI_H}
    "-DFCP_EXTRA=march=${FCP_MARCH}|clang=${CMAKE_CXX_COMPILER_VERSION}|pgo=${_fcp_pgo_hash}"
    -DFCP_SDK_VERSION=${FCP_REXGLUE_SDK_VERSION} -P "${CMAKE_SOURCE_DIR}/cmake/fcp_game_code_abi.cmake"
  BYPRODUCTS "${FCP_GAME_CODE_ABI_H}"
  COMMENT "fcp_game_code_abi.h"
  VERBATIM
)
add_dependencies(predator predator_game_code_abi)

if (FCP_MARCH)
  set(_fcp_code_march "${FCP_MARCH}")
else ()
  set(_fcp_code_march "x86-64-v2")
endif ()
target_compile_definitions(
  predator PRIVATE REX_TITLE_HOOK_EXPORTS FCP_GAME_CODE_MARCH="${_fcp_code_march}"
)
# FCP_PGO=use: the first start compiles with the same profile, shipped as compiler\pgo\predator.profdata
# (tools\dist.ps1); predator.exe only takes a file with this SHA-256 (src/fcp/game_code.cpp).
if (FCP_PGO STREQUAL "use" AND EXISTS "${FCP_PGO_PROFILE}")
  target_compile_definitions(predator PRIVATE FCP_GAME_CODE_PGO_SHA256="${_fcp_pgo_hash}")
endif ()
# ThinLTO of predator_code.dll (development build and first start; FCP_GAME_CODE_LTO=0/1 overrides at run time).
if (FCP_GAME_CODE_LTO)
  target_compile_definitions(predator PRIVATE FCP_GAME_CODE_LTO_DEFAULT=1)
else ()
  target_compile_definitions(predator PRIVATE FCP_GAME_CODE_LTO_DEFAULT=0)
endif ()
set(FCP_LZMA_DIR
    "${CMAKE_SOURCE_DIR}/third_party/lzma-sdk"
    CACHE PATH "LZMA SDK decoder (tools/get_lzma.ps1), for the shader pack of the first start"
)
if (NOT EXISTS "${FCP_LZMA_DIR}/LzmaDec.c")
  message(FATAL_ERROR "LZMA decoder missing in ${FCP_LZMA_DIR}. Run "
                      "powershell -NoProfile -ExecutionPolicy Bypass -File tools\\get_lzma.ps1"
  )
endif ()
target_sources(
  predator
  PRIVATE src/fcp/game_code.cpp
          src/fcp/game_code_build.cpp
          src/fcp/first_start.cpp
          src/fcp/progress_window.cpp
          src/fcp/build_process.cpp
          src/fcp/hash.cpp
          "${FCP_LZMA_DIR}/LzmaDec.c"
)
target_include_directories(predator PRIVATE "${FCP_LZMA_DIR}")
target_link_libraries(predator PRIVATE bcrypt delayimp)

if (NOT FCP_GAME_CODE_DLL)
  # The game code linked into predator.exe as before (fcp_setup_title_target); no DLL, no first-start build.
  target_compile_definitions(predator PRIVATE FCP_STATIC_GAME_CODE=1)
  return()
endif ()
if (FCP_PGO STREQUAL "generate")
  message(FATAL_ERROR "FCP_PGO=generate needs the profile runtime in the game code; with predator_code.dll it "
                      "is not wired up. Configure with -DFCP_GAME_CODE_DLL=OFF for the training build."
  )
endif ()

# --- predator.exe: delay-load imports of the originals ------------------------------------------------
set(_fcp_delay_def "${FCP_BOOT_GEN_DIR}/predator_code_delay.def")
set(_fcp_delay_lib "${FCP_BOOT_GEN_DIR}/predator_code_delay.lib")
add_custom_command(
  OUTPUT "${_fcp_delay_lib}"
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/boot/gen_game_code_def.py" "${_fcp_delay_def}"
          "${FCP_TITLE_CONFIG}" "${FCP_D3D_NAMES}"
  COMMAND "${FCP_LLD_LINK}" /lib /nologo /machine:x64 "/def:${_fcp_delay_def}" "/out:${_fcp_delay_lib}"
  DEPENDS "${FCP_TITLE_CONFIG}" "${CMAKE_BINARY_DIR}/d3d_names.stamp"
          "${CMAKE_SOURCE_DIR}/tools/boot/gen_game_code_def.py"
  COMMENT "Delay-load import library of predator_code.dll"
  VERBATIM
)
add_custom_target(predator_code_delay DEPENDS "${_fcp_delay_lib}")
add_dependencies(predator predator_code_delay)
target_link_libraries(predator PRIVATE "${_fcp_delay_lib}")
target_link_options(predator PRIVATE "LINKER:/delayload:predator_code.dll")

# --- import libraries of the C runtime DLLs for the game code (name lists only) -------------------------
set(FCP_GAME_CODE_LIB_DIR "${CMAKE_BINARY_DIR}/game_code_libs")
file(GLOB _fcp_crt_defs "${CMAKE_SOURCE_DIR}/src/fcp/game_code/*.def")
set(_fcp_crt_libs "")
foreach (_def IN LISTS _fcp_crt_defs)
  get_filename_component(_name "${_def}" NAME_WE)
  set(_lib "${FCP_GAME_CODE_LIB_DIR}/${_name}.lib")
  add_custom_command(
    OUTPUT "${_lib}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${FCP_GAME_CODE_LIB_DIR}"
    COMMAND "${FCP_LLD_LINK}" /lib /nologo /machine:x64 "/def:${_def}" "/out:${_lib}"
    DEPENDS "${_def}"
    COMMENT "Import library ${_name}.lib for the game code"
    VERBATIM
  )
  list(APPEND _fcp_crt_libs "${_lib}")
endforeach ()
add_custom_target(predator_code_libs DEPENDS ${_fcp_crt_libs})

# --- predator_code.dll ------------------------------------------------------------------------------------
set(FCP_GAME_CODE_DLL_PATH "${REX_OUTPUT_DIR}/game_code/predator_code.dll")
set(_fcp_code_work "${CMAKE_BINARY_DIR}/game_code_work")
file(READ "${FCP_DISC_CONFIG}" _fcp_disc)
string(REGEX MATCH "sha256 *= *\"([0-9A-Fa-f]+)\"" _ "${_fcp_disc}")
string(TOUPPER "${CMAKE_MATCH_1}" _fcp_xex_sha256)
set(_fcp_settings "gen=${FCP_GEN_DIR}\ndll=${FCP_GAME_CODE_DLL_PATH}\nwork=${_fcp_code_work}\n")
string(APPEND _fcp_settings "log=${_fcp_code_work}/build.log\norigin=build\nimage_sha256=${_fcp_xex_sha256}\n")
string(APPEND _fcp_settings "clang=${CMAKE_CXX_COMPILER}\nlld_link=${FCP_LLD_LINK}\n")
string(APPEND _fcp_settings "include=${CMAKE_SOURCE_DIR}/rexglue/include\ninclude=${FCP_DEPS_DIR}/simde\n")
string(APPEND _fcp_settings "include=${FCP_DEPS_DIR}/simde/simde\ninclude=${CMAKE_SOURCE_DIR}/src\n")
string(APPEND _fcp_settings "isystem=${CMAKE_SOURCE_DIR}/rexglue/include/rex/guest_std\n")
string(APPEND _fcp_settings "lib=$<TARGET_LINKER_FILE:rexppc>\nlib=$<TARGET_LINKER_FILE:rexbase>\n")
foreach (_lib IN LISTS _fcp_crt_libs)
  string(APPEND _fcp_settings "lib=${_lib}\n")
endforeach ()
string(APPEND _fcp_settings "entry=${CMAKE_SOURCE_DIR}/src/fcp/game_code/entry.cpp\nruntime=${CMAKE_SOURCE_DIR}/src/fcp/game_code/runtime.cpp\n")
if (FCP_PGO STREQUAL "use")
  string(APPEND _fcp_settings "pgo_profile=${FCP_PGO_PROFILE}\n")
endif ()
if (FCP_GAME_CODE_LTO)
  string(APPEND _fcp_settings "lto=thin\n")
endif ()
set(_fcp_settings_file "${CMAKE_BINARY_DIR}/game_code_build.txt")
file(GENERATE OUTPUT "${_fcp_settings_file}" CONTENT "${_fcp_settings}")
if (FCP_COVERAGE_CHECK)
  set(_fcp_code_gate "${FCP_COVERAGE_DIR}/coverage.stamp")
else ()
  set(_fcp_code_gate "${FCP_GEN_DIR}/codegen.build.stamp")
endif ()
add_custom_command(
  OUTPUT "${FCP_GAME_CODE_DLL_PATH}"
  COMMAND $<TARGET_FILE:predator> --fcp-build-game-code "${_fcp_settings_file}"
  DEPENDS predator "${_fcp_settings_file}" "${_fcp_code_gate}" ${_fcp_crt_libs}
          "${CMAKE_SOURCE_DIR}/src/fcp/game_code/entry.cpp" "${CMAKE_SOURCE_DIR}/src/fcp/game_code/runtime.cpp"
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  COMMENT "Building predator_code.dll (the recompiled game code) with predator.exe --fcp-build-game-code"
  VERBATIM
)
add_custom_target(predator_code ALL DEPENDS "${FCP_GAME_CODE_DLL_PATH}")
add_dependencies(predator_code predator_coverage predator_code_libs)
