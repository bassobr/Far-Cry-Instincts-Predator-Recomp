# fcp_d3d_names.cmake: config/predator/d3d_symbols.toml -> generated/d3d_names.toml ([function_names]).
#
# The D3D names become symbols of the generated code (D3DDevice_Swap instead of sub_<address>), so the
# hooks in src/ name functions and never carry addresses. The manifest includes the generated file
# (config/predator/predator_manifest.toml.in); codegen reruns when it changes.

set(FCP_D3D_SYMBOLS "${CMAKE_SOURCE_DIR}/config/predator/d3d_symbols.toml")
set(FCP_D3D_NAMES "${FCP_GENERATED_DIR}/d3d_names.toml")
# Values from [library] that are no addresses (device size), for src/fcp/d3d_hooks.cpp.
set(FCP_BOOT_GEN_DIR "${CMAKE_BINARY_DIR}/fcp_gen")
set(FCP_D3D_CONFIG_H "${FCP_BOOT_GEN_DIR}/fcp_d3d_config.h")

# Also generate at configure time. sources.cmake (a codegen output) is a configure dependency, so when an
# existing build folder is upgraded, ninja runs codegen with the *old* build graph before it reconfigures;
# that graph does not know the build-time rule below, and the manifest would include a missing file.
file(MAKE_DIRECTORY "${FCP_BOOT_GEN_DIR}")
execute_process(
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/boot/gen_d3d_names.py" "${FCP_D3D_SYMBOLS}"
          "${FCP_D3D_NAMES}" "${FCP_D3D_CONFIG_H}"
  RESULT_VARIABLE _fcp_d3d_names_result
  OUTPUT_QUIET
)
if (NOT _fcp_d3d_names_result EQUAL 0)
  message(FATAL_ERROR "gen_d3d_names.py failed (${_fcp_d3d_names_result}) for ${FCP_D3D_SYMBOLS}")
endif ()
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${FCP_D3D_SYMBOLS}" "${CMAKE_SOURCE_DIR}/tools/boot/gen_d3d_names.py"
)

add_custom_command(
  OUTPUT "${CMAKE_BINARY_DIR}/d3d_names.stamp"
  BYPRODUCTS "${FCP_D3D_NAMES}" "${FCP_D3D_CONFIG_H}"
  COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tools/boot/gen_d3d_names.py" "${FCP_D3D_SYMBOLS}"
          "${FCP_D3D_NAMES}" "${FCP_D3D_CONFIG_H}"
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/d3d_names.stamp"
  DEPENDS "${FCP_D3D_SYMBOLS}" "${CMAKE_SOURCE_DIR}/tools/boot/gen_d3d_names.py"
  COMMENT "Generating [function_names] from d3d_symbols.toml"
  VERBATIM
)
list(APPEND FCP_CODEGEN_EXTRA_DEPENDS "${CMAKE_BINARY_DIR}/d3d_names.stamp")
