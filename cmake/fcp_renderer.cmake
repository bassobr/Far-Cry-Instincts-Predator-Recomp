# fcp_renderer.cmake: adds the native renderer (src/renderer) to the project build and to predator.exe.
#
# The targets predator_renderer, the tests and renderer_probe end up next to the EXE under out/build/<cfg>/bin.
# predator.exe links predator_renderer and gets src/fcp/render_bridge.cpp (presentation through IHostPresenter,
# NrApi for the D3D hooks); renderer.toml and dxcompiler.dll lie next to the EXE. Switched off at run time with
# FCP_RENDERER=0 (the hooks then only call the original). predator.exe needs the renderer to build.
# Dependencies: tools/get_renderer_deps.ps1 (FCP_RENDERER_DEPS_DIR), toml++ from FCP_DEPS_DIR.
option(FCP_RENDERER "Build the native renderer (src/renderer) and its tests" ON)
if (FCP_RENDERER)
  add_subdirectory("${CMAKE_SOURCE_DIR}/src/renderer" "${CMAKE_BINARY_DIR}/renderer")

  # This file is included before add_executable(predator): the attachment runs at the end of the root CMakeLists.
  function (fcp_renderer_attach)
    if (NOT TARGET predator)
      return()
    endif ()
    target_sources(predator PRIVATE "${CMAKE_SOURCE_DIR}/src/fcp/render_bridge.cpp")
    target_link_libraries(predator PRIVATE predator_renderer)
    target_compile_definitions(predator PRIVATE FCP_HAS_RENDERER=1)
    # FCP_DXC_DLL is set in src/renderer's directory scope only; this deferred call runs in the root scope, where it
    # was empty (`copy_if_different ""`), so predator.exe got DXC only through the renderer test targets and the
    # target `dist` failed in a fresh build folder (dxcompiler.dll missing next to predator.exe).
    set(FCP_DXC_DLL "${FCP_RENDERER_DEPS_DIR}/dxc/bin/x64/dxcompiler.dll")
    fcp_renderer_runtime(predator)
    add_custom_command(
      TARGET predator
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/config/predator/renderer.toml"
              "$<TARGET_FILE_DIR:predator>/renderer.toml"
      VERBATIM
    )
  endfunction ()
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL fcp_renderer_attach)
endif ()
