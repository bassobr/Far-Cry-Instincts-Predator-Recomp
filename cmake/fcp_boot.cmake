# fcp_boot.cmake: boot layer of predator.exe.
#  - src/fcp/*: asynchronous log, import trace, watchdog, logging D3D hooks;
#  - fcp_import_thunks.cpp, generated after codegen from its import list (tools/boot/gen_import_thunks.py);
#  - fcp_version.h with the git commit (cmake/fcp_version.cmake, every build, rewritten only on change).
# Included from the root CMakeLists.txt after add_executable(predator).

set(FCP_BOOT_GEN_DIR "${CMAKE_BINARY_DIR}/fcp_gen")
file(MAKE_DIRECTORY "${FCP_BOOT_GEN_DIR}")

add_custom_target(
  predator_version
  COMMAND ${CMAKE_COMMAND} -DFCP_SOURCE_DIR=${CMAKE_SOURCE_DIR} -DFCP_OUT=${FCP_BOOT_GEN_DIR}/fcp_version.h
          -DFCP_CONFIG=$<CONFIG> -P "${CMAKE_SOURCE_DIR}/cmake/fcp_version.cmake"
  BYPRODUCTS "${FCP_BOOT_GEN_DIR}/fcp_version.h"
  COMMENT "fcp_version.h"
  VERBATIM
)

# Wrappers for every __imp__ export of rexkernel (not only the game's imports, so the executable depends on
# nothing of the game), minus the names of game functions in the title configuration.
get_filename_component(_fcp_llvm_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(FCP_IMPORT_THUNKS "${FCP_BOOT_GEN_DIR}/fcp_import_thunks.cpp")
add_custom_command(
  OUTPUT "${FCP_IMPORT_THUNKS}"
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/boot/gen_import_thunks.py" "${_fcp_llvm_bin_dir}/llvm-nm.exe"
          $<TARGET_LINKER_FILE:rexkernel> "${CMAKE_SOURCE_DIR}/rexglue/src" "${FCP_IMPORT_THUNKS}"
          "${FCP_TITLE_CONFIG}" "${FCP_D3D_NAMES}"
  COMMAND ${CMAKE_COMMAND} -E touch "${FCP_IMPORT_THUNKS}"
  DEPENDS rexkernel "${CMAKE_SOURCE_DIR}/tools/boot/gen_import_thunks.py" "${FCP_TITLE_CONFIG}"
          "${CMAKE_BINARY_DIR}/d3d_names.stamp"
  COMMENT "Generating import wrappers for the import trace"
  VERBATIM
)

set(FCP_ADDRESSES_H "${FCP_BOOT_GEN_DIR}/fcp_addresses.h")
add_custom_command(
  OUTPUT "${FCP_ADDRESSES_H}"
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/boot/gen_addresses.py"
          "${CMAKE_SOURCE_DIR}/config/predator/runtime_addresses.toml" "${FCP_ADDRESSES_H}"
  COMMAND ${CMAKE_COMMAND} -E touch "${FCP_ADDRESSES_H}"
  DEPENDS "${CMAKE_SOURCE_DIR}/config/predator/runtime_addresses.toml"
          "${CMAKE_SOURCE_DIR}/tools/boot/gen_addresses.py"
  COMMENT "Generating fcp_addresses.h"
  VERBATIM
)

target_sources(
  predator
  PRIVATE src/fcp/env.cpp
          src/fcp/log.cpp
          src/fcp/guest.cpp
          src/fcp/cpu_check.cpp
          src/fcp/imports.cpp
          src/fcp/watchdog.cpp
          src/fcp/d3d_hooks.cpp
          src/fcp/engine_hooks.cpp
          src/fcp/devstart.cpp
          src/fcp/relaunch.cpp
          src/fcp/gametime.cpp
          src/fcp/framerate.cpp
          src/fcp/thread_profile.cpp
          src/fcp/cpu_stats.cpp
          src/fcp/shader_compile_cache.cpp
          src/fcp/gpu_wait.cpp
          src/fcp/input_script.cpp
          src/fcp/state_trace.cpp
          src/fcp/frame_coupling.cpp
          src/fcp/camera_fov.cpp
          src/fcp/detail_distance.cpp
          src/fcp/shadow_distance.cpp
          src/fcp/file_patch.cpp
          src/fcp/fp_stats.cpp
          src/fcp/level_test.cpp
          src/fcp/user_paths.cpp
          src/fcp/save_guard.cpp
          src/fcp/user_settings.cpp
          src/fcp/display.cpp
          src/fcp/device_preset.cpp
          src/fcp/toml_edit.cpp
          src/fcp/settings_menu.cpp
          src/fcp/button_prompts.cpp
          src/fcp/keyboard_text.cpp
          "${FCP_IMPORT_THUNKS}"
          "${FCP_ADDRESSES_H}"
)
target_include_directories(predator PRIVATE "${FCP_BOOT_GEN_DIR}")
# The ISA check itself must run on any x86-64 CPU (src/fcp/cpu_check.cpp).
if (FCP_MARCH AND NOT FCP_MARCH STREQUAL "x86-64-v2")
  target_compile_definitions(predator PRIVATE FCP_MARCH_V3)
  set_source_files_properties(src/fcp/cpu_check.cpp PROPERTIES COMPILE_OPTIONS "-march=x86-64-v2")
endif ()
target_link_libraries(predator PRIVATE dbghelp winmm shell32 ole32 gdi32)
# Plain CRT file and string functions in the boot layer.
target_compile_definitions(predator PRIVATE _CRT_SECURE_NO_WARNINGS)
add_dependencies(predator predator_version)
# Development builds are portable: user folder = the EXE folder (saves in bin\user, logs and caches next to the
# EXE, as before). A distribution leaves the marker out (user folder %LOCALAPPDATA%\...), see
# src/fcp/user_paths.h.
add_custom_command(
  TARGET predator
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/config/predator/portable.txt"
          "$<TARGET_FILE_DIR:predator>/portable.txt"
  VERBATIM
)

include("${CMAKE_SOURCE_DIR}/cmake/fcp_audio_input.cmake")

# Test of the save guard without the game (tests/save_guard): start check, repair, backups, the wait at the end.
#   cmake --build --preset release --target fcp_save_guard_test
#   out\build\release\bin\fcp_save_guard_test.exe <empty work folder>
add_executable(
  fcp_save_guard_test EXCLUDE_FROM_ALL "${CMAKE_SOURCE_DIR}/tests/save_guard/save_guard_test.cpp"
                      "${CMAKE_SOURCE_DIR}/src/fcp/save_guard.cpp" "${CMAKE_SOURCE_DIR}/src/fcp/env.cpp"
                      "${CMAKE_SOURCE_DIR}/src/fcp/hash.cpp"
)
target_include_directories(fcp_save_guard_test PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(fcp_save_guard_test PRIVATE rexbase)  # headers of fcp/devstart.h (rex/ppc)
target_compile_definitions(fcp_save_guard_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(fcp_save_guard_test PRIVATE bcrypt)
set_target_properties(fcp_save_guard_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:predator>")
