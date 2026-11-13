# fcp_dist.cmake: target `dist` (cmake --build --preset release --target dist): the release folder and zip
# without anything of the game (tools/dist.ps1). Needs no disc image: it takes
# predator.exe, the SDK and renderer DLLs, the tools and the compiler folder, never the game code.
set(FCP_VERSION_FILE "${CMAKE_SOURCE_DIR}/VERSION")
get_filename_component(_fcp_dist_llvm "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_fcp_dist_llvm "${_fcp_dist_llvm}" DIRECTORY)
find_program(FCP_POWERSHELL NAMES powershell.exe pwsh.exe REQUIRED)
add_custom_target(
  dist
  COMMAND "${FCP_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/dist.ps1" -Build
          "${CMAKE_BINARY_DIR}" -Llvm "${_fcp_dist_llvm}" -Deps "${FCP_DEPS_DIR}" -Zip
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Release folder and zip (tools/dist.ps1)"
  VERBATIM
)
add_dependencies(dist predator)
if (TARGET fcshaders_tool)
  add_dependencies(dist fcshaders_tool)
endif ()
if (TARGET predator_code_libs)
  add_dependencies(dist predator_code_libs)
endif ()
# The release ships the codegen tool (compiler\bin\rexglue.exe). In a fresh build folder nothing else had built it
# before `dist` (rexglue.exe was missing from a release built in a fresh folder).
if (TARGET rexglue)
  add_dependencies(dist rexglue)
endif ()
