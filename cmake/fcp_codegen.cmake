# fcp_codegen.cmake: disc image -> default.xex -> rexglue codegen -> coverage check -> recompiled code.
#
# Same wiring as rexglue_project() of the SDK (cmake/rexglue_project.cmake), with three differences:
#  - the manifest is generated from config/predator/predator_manifest.toml.in into FCP_GENERATED_DIR,
#    because its paths come from the build and codegen rewrites [project].sdk_version in place;
#  - codegen also reruns when rexglue.exe or the title configuration change (we patch the codegen);
#  - tools/coverage_check.py runs after codegen and gates the compile of the generated code.
#
# Steps (targets): predator_game_files -> predator_codegen -> predator_coverage -> predator_recompiled.
# The list of generated sources is only known after the first codegen run. build.cmd therefore builds
# predator_coverage first, configures again if the build folder has no predator_recompiled target yet
# (reads generated/default/sources.cmake) and builds predator last. A plain "cmake --build" on a fresh
# tree stops at the link with a message; configure and build again (or use build.cmd).

set(FCP_SOURCE_DIR "${CMAKE_SOURCE_DIR}")
# Codegen writes the numeric SDK version into [project].sdk_version; the template carries the same value,
# so codegen does not touch the manifest (which would rerun it on every build).
string(REGEX REPLACE "-.*$" "" FCP_REXGLUE_SDK_VERSION "${REXGLUE_VERSION_OVERRIDE}")
set(FCP_MANIFEST "${FCP_GENERATED_DIR}/predator_manifest.toml")
set(FCP_GEN_DIR "${FCP_GENERATED_DIR}/default")
set(FCP_TITLE_CONFIG "${CMAKE_SOURCE_DIR}/config/predator/default.toml")
set(FCP_COVERAGE_CONFIG "${CMAKE_SOURCE_DIR}/config/predator/coverage.toml")
set(FCP_DISC_CONFIG "${CMAKE_SOURCE_DIR}/config/predator/disc.toml")

file(MAKE_DIRECTORY "${FCP_GENERATED_DIR}" "${FCP_GAME_DIR}")
# LF: codegen rewrites the sdk_version line with LF endings and compares byte for byte (rexglue change 4).
configure_file(
  "${CMAKE_SOURCE_DIR}/config/predator/predator_manifest.toml.in" "${FCP_MANIFEST}" @ONLY
  NEWLINE_STYLE LF
)

# --- 1. Files from the disc image ---------------------------------------------------------------------
set(_fcp_game_outputs "${FCP_GAME_DIR}/default.xex" "${FCP_GAME_DIR}/default.bin"
                      "${FCP_GAME_DIR}/default.json"
)
set(_fcp_game_depends
    "${CMAKE_SOURCE_DIR}/tools/prepare_xex.py" "${CMAKE_SOURCE_DIR}/tools/xiso.py"
    "${CMAKE_SOURCE_DIR}/tools/xexunpack.py" "${CMAKE_SOURCE_DIR}/tools/xexinfo.py" "${FCP_DISC_CONFIG}"
)
if (EXISTS "${FCP_GAME_ISO}")
  list(APPEND _fcp_game_depends "${FCP_GAME_ISO}")
else ()
  message(WARNING "FCP_GAME_ISO does not exist: ${FCP_GAME_ISO}\nThe SDK builds, the codegen step "
                  "fails until FCP_GAME_ISO points to the disc image."
  )
endif ()
# FCP_GAME_DIR is shared by all build folders. prepare_xex.py only rewrites a file whose content changed,
# so the files are byproducts (restat) and the per-folder stamp is the output.
add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/game_files.stamp"
  BYPRODUCTS ${_fcp_game_outputs}
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/prepare_xex.py" --iso "${FCP_GAME_ISO}" --disc
          "${FCP_DISC_CONFIG}" --out "${FCP_GAME_DIR}" --stamp "${CMAKE_BINARY_DIR}/game_files.stamp"
  DEPENDS ${_fcp_game_depends}
  COMMENT "Extracting default.xex from the disc image"
  VERBATIM
)
add_custom_target(predator_game_files DEPENDS "${CMAKE_BINARY_DIR}/game_files.stamp")

# --- 2. Codegen ---------------------------------------------------------------------------------------
set_property(GLOBAL PROPERTY REXGLUE_PROJECT_NAME predator)
rexglue_recomp_options(REXGLUE_RECOMP_OPTIONS REXGLUE_RECOMP_OPTIMIZED)

set(GENERATED_SOURCES "")
set(GENERATED_PREFIX "")
if (EXISTS "${FCP_GEN_DIR}/sources.cmake")
  include("${FCP_GEN_DIR}/sources.cmake")
endif ()
set(FCP_GENERATED_SOURCES ${GENERATED_SOURCES})
set(FCP_GENERATED_PREFIX ${GENERATED_PREFIX})

# D3D names for the codegen ([function_names], tools/boot/gen_d3d_names.py).
include("${CMAKE_SOURCE_DIR}/cmake/fcp_d3d_names.cmake")

# --ignore-stamp: ninja only runs this when an input changed, and a changed codegen does not change the
# SDK version stamp. Unchanged files keep their time stamps, so nothing recompiles needlessly.
# File dependency on rexcodegen.lib (analysis, builders, templates) rather than rexglue.exe: rexglue.exe
# relinks after every configure (build time stamp in rexbase), which would rerun codegen for nothing.
# The genex in COMMAND still orders the build after rexglue.exe.
add_custom_command(
  OUTPUT "${FCP_GEN_DIR}/codegen.build.stamp" "${FCP_GEN_DIR}/sources.cmake" ${FCP_GENERATED_SOURCES}
  COMMAND $<TARGET_FILE:rex::rexglue> codegen "${FCP_MANIFEST}" --ignore-stamp
  DEPENDS rexcodegen "${FCP_MANIFEST}" "${FCP_TITLE_CONFIG}" "${FCP_GAME_DIR}/default.xex"
          ${FCP_CODEGEN_EXTRA_DEPENDS}
  DEPFILE "${FCP_GEN_DIR}/codegen.d"
  WORKING_DIRECTORY "${FCP_GENERATED_DIR}"
  COMMENT "Generating recompiled code for default.xex"
  VERBATIM
)
add_custom_target(predator_codegen DEPENDS "${FCP_GEN_DIR}/codegen.build.stamp")
add_dependencies(predator_codegen predator_game_files rexglue)
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${FCP_GEN_DIR}/sources.cmake"
)

# --- 3. Coverage check (fails the build on entry points the codegen missed) ---------------------------
set(FCP_COVERAGE_DIR "${CMAKE_BINARY_DIR}/coverage")
if (FCP_COVERAGE_CHECK)
  add_custom_command(
    OUTPUT "${FCP_COVERAGE_DIR}/coverage.stamp"
    COMMAND
      "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/coverage_check.py" "${FCP_GEN_DIR}"
      "${FCP_GAME_DIR}/default.bin" "${FCP_GAME_DIR}/default.json" --config "${FCP_COVERAGE_CONFIG}"
      --report "${FCP_COVERAGE_DIR}/coverage_report.txt" --suggest
      "${FCP_COVERAGE_DIR}/suggested_functions.toml" --stamp "${FCP_COVERAGE_DIR}/coverage.stamp"
    DEPENDS "${FCP_GEN_DIR}/codegen.build.stamp" "${FCP_GAME_DIR}/default.bin"
            "${FCP_GAME_DIR}/default.json" "${CMAKE_SOURCE_DIR}/tools/coverage_check.py"
            "${FCP_COVERAGE_CONFIG}"
    COMMENT "Checking codegen coverage of default.xex"
    VERBATIM
  )
  add_custom_target(predator_coverage DEPENDS "${FCP_COVERAGE_DIR}/coverage.stamp")
  add_dependencies(predator_coverage predator_codegen)
else ()
  add_custom_target(predator_coverage)
  add_dependencies(predator_coverage predator_codegen)
endif ()

# --- 4. Host executable ---------------------------------------------------------------------------------
# fcp_setup_title_target(<exe>): like rexglue_setup_target() of the SDK. With FCP_GAME_CODE_DLL (default) the
# executable carries no game code and does not see the generated headers; cmake/fcp_game_code.cmake builds
# predator_code.dll. Without it the recompiled code is linked into the executable as before.
function (fcp_setup_title_target target_name)
  if (FCP_GAME_CODE_DLL)
    target_include_directories(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(${target_name} PRIVATE rex::sdk)
    rexglue_configure_target(${target_name})
    return()
  endif ()
  if (FCP_GENERATED_SOURCES)
    add_library(predator_recompiled OBJECT ${FCP_GENERATED_SOURCES})
    target_include_directories(
      predator_recompiled PRIVATE "${CMAKE_SOURCE_DIR}/src" "${FCP_GEN_DIR}"
    )
    target_link_libraries(predator_recompiled PRIVATE rex::sdk)
    rexglue_apply_target_settings(predator_recompiled)
    rexglue_apply_recomp_settings(predator_recompiled "${FCP_GEN_DIR}/${FCP_GENERATED_PREFIX}_pch.h")
    add_dependencies(predator_recompiled predator_coverage)
    target_link_libraries(${target_name} PRIVATE predator_recompiled)
    # PGO of the recompiled code (main-thread performance): FCP_PGO=generate writes
    # %LLVM_PROFILE_FILE% (default: default.profraw in the working folder) at exit, FCP_PGO=use reads
    # FCP_PGO_PROFILE (llvm-profdata merge). The profile is derived from the game: out/ only, never committed.
    if (FCP_PGO STREQUAL "generate")
      target_compile_options(predator_recompiled PRIVATE -fprofile-instr-generate)
      target_link_options(${target_name} PRIVATE -fprofile-instr-generate)
    elseif (FCP_PGO STREQUAL "use")
      if (NOT EXISTS "${FCP_PGO_PROFILE}")
        message(FATAL_ERROR "FCP_PGO=use: FCP_PGO_PROFILE '${FCP_PGO_PROFILE}' not found")
      endif ()
      target_compile_options(
        predator_recompiled PRIVATE "-fprofile-instr-use=${FCP_PGO_PROFILE}" -Wno-profile-instr-unprofiled
                                    -Wno-profile-instr-out-of-date -Wno-profile-instr-missing
      )
    endif ()
  else ()
    add_custom_command(
      TARGET ${target_name}
      PRE_LINK
      COMMAND ${CMAKE_COMMAND} -E echo
              "fcp: codegen output was missing at configure time. Build again (or use build.cmd)."
      COMMAND ${CMAKE_COMMAND} -E false
      VERBATIM
    )
  endif ()
  add_dependencies(${target_name} predator_coverage)
  target_include_directories(${target_name} PRIVATE "${CMAKE_SOURCE_DIR}/src" "${FCP_GEN_DIR}")
  target_link_libraries(${target_name} PRIVATE rex::sdk)
  rexglue_configure_target(${target_name})
endfunction ()
