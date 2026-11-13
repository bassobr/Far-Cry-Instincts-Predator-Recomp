# fcp_semtest.cmake: semantic tests of the PPC -> C++ codegen.
#
# Two parts:
#  1. The PPC instruction tests that ship with the SDK (rexglue/tests/ppc, 189 assembly files from Xenia
#     and ReXGlue, plus the hardware vector tests in rexglue/tests/unit/ppc). They need a powerpc-none-elf
#     binutils with VMX128 (FCP_PPC_BINUTILS_DIR, a separate GPL program that only assembles the tests)
#     and Catch2 (FCP_CATCH2_DIR). Targets: ppc_tests, fcp_sdk_ppc_unit.
#  2. predator_semtest: our own generator (tests/semantics) builds one small guest function per encoding
#     form the game uses, recompiles it with our codegen and the title configuration, runs it with edge
#     and random values and compares against an independent reference, the SDK hardware vectors and,
#     when FCP_XENONRECOMP_DIR is set, against XenonRecomp. Runs after every change of the codegen
#     (rexcodegen), fails the build on a failure that is not listed in tests/semantics/known.toml.

set(FCP_PPC_BINUTILS_DIR
    "${CMAKE_SOURCE_DIR}/third_party/ppc-binutils"
    CACHE PATH "powerpc-none-elf binutils with VMX128 (upstream rexglue tools/binutils), for the SDK PPC tests"
)
if (EXISTS "${FCP_DEPS_DIR}/catch2/CMakeLists.txt")
  set(_fcp_catch2_default "${FCP_DEPS_DIR}/catch2")
else ()
  set(_fcp_catch2_default "${CMAKE_SOURCE_DIR}/third_party/catch2")
endif ()
set(FCP_CATCH2_DIR
    "${_fcp_catch2_default}"
    CACHE PATH "Catch2 v3.12.0 (88abf9bf) for the SDK PPC tests"
)
option(FCP_SEMTEST "Build and run predator_semtest as part of the default build" ON)
if (EXISTS "${CMAKE_SOURCE_DIR}/third_party/XenonRecomp/out/build/XenonRecomp/XenonRecomp.exe")
  set(_fcp_xr_default "${CMAKE_SOURCE_DIR}/third_party/XenonRecomp")
else ()
  set(_fcp_xr_default "")
endif ()
set(FCP_XENONRECOMP_DIR
    "${_fcp_xr_default}"
    CACHE PATH "Patched XenonRecomp build tree (XenonRecomp.exe, XenonUtils) for the differential test; empty = off"
)

# --- 1. SDK PPC tests ----------------------------------------------------------------------------------
set(_fcp_sdk_tests OFF)
if (EXISTS "${FCP_PPC_BINUTILS_DIR}/powerpc-none-elf-as.exe" AND EXISTS "${FCP_CATCH2_DIR}/CMakeLists.txt")
  set(_fcp_sdk_tests ON)
else ()
  message(STATUS "fcp: SDK PPC tests off (binutils in FCP_PPC_BINUTILS_DIR or Catch2 in FCP_CATCH2_DIR missing)")
endif ()

if (_fcp_sdk_tests)
  enable_testing()
  if (NOT TARGET Catch2::Catch2WithMain)
    add_subdirectory("${FCP_CATCH2_DIR}" "${CMAKE_BINARY_DIR}/third_party/catch2" EXCLUDE_FROM_ALL)
  endif ()
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/rexglue/cmake" "${FCP_CATCH2_DIR}/extras")
  set(REXGLUE_PPC_BINUTILS_DIR
      "${FCP_PPC_BINUTILS_DIR}"
      CACHE PATH "" FORCE
  )
  # The SDK's own pipeline: assemble, link, nm, `rexglue recompile-tests`, Catch2 executable ppc_tests.
  add_subdirectory("${CMAKE_SOURCE_DIR}/rexglue/tests/ppc" "${CMAKE_BINARY_DIR}/sdk_tests/ppc" EXCLUDE_FROM_ALL)

  # The instruction related unit tests of the SDK (hardware vectors for the estimates, multiply add,
  # dot products, D3D packs, saturation, flush mode). The full unit_tests target needs the Xenos host
  # drivers (Vulkan SDK), these sources do not.
  set(_fcp_unit "${CMAKE_SOURCE_DIR}/rexglue/tests/unit")
  add_executable(
    fcp_sdk_ppc_unit EXCLUDE_FROM_ALL
    ${_fcp_unit}/ppc/dot_product_test.cpp
    ${_fcp_unit}/ppc/estimate_test.cpp
    ${_fcp_unit}/ppc/fpscr_flush_test.cpp
    ${_fcp_unit}/ppc/vscr_register_test.cpp
    ${_fcp_unit}/ppc/multiply_add_test.cpp
    ${_fcp_unit}/ppc/pack_d3d_test.cpp
    ${_fcp_unit}/ppc/saturation_test.cpp
    ${_fcp_unit}/ppc/vector_tables_test.cpp
    ${_fcp_unit}/ppc/vector_unaligned_write_test.cpp
  )
  target_include_directories(
    fcp_sdk_ppc_unit PRIVATE "${CMAKE_SOURCE_DIR}/rexglue/src" "${CMAKE_SOURCE_DIR}/rexglue/tests"
                             "${CMAKE_SOURCE_DIR}/rexglue/thirdparty" "${_fcp_unit}"
  )
  target_link_libraries(fcp_sdk_ppc_unit PRIVATE rexppc rexbase simde Catch2::Catch2WithMain)
  target_compile_definitions(fcp_sdk_ppc_unit PRIVATE REXGLUE_BUILD_CONFIG="$<CONFIG>")
  set_target_properties(fcp_sdk_ppc_unit PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${REX_OUTPUT_DIR}")
  rexglue_link_host_allocator(fcp_sdk_ppc_unit)

  add_custom_target(
    fcp_sdk_tests
    COMMAND $<TARGET_FILE:ppc_tests> --reporter compact
    COMMAND $<TARGET_FILE:fcp_sdk_ppc_unit> --reporter compact
    # self check of the reference interpreter of predator_semtest against the SDK vectors
    COMMAND "${FCP_PYTHON}" -I "${CMAKE_SOURCE_DIR}/tests/semantics/py/check_ref_sdk.py"
            "${CMAKE_SOURCE_DIR}/rexglue/tests/ppc/asm" "${CMAKE_BINARY_DIR}/tests/ppc/bin"
    DEPENDS ppc_tests fcp_sdk_ppc_unit
    WORKING_DIRECTORY "${REX_OUTPUT_DIR}"
    COMMENT "Running the SDK PPC instruction tests"
    VERBATIM
  )
endif ()

# --- 2. predator_semtest ------------------------------------------------------------------------------
# fcp_semtool: decoder view of the codegen and codegen of the synthetic test functions with the title
# configuration (tests/semantics/tool/semtool.cpp).
add_executable(fcp_semtool "${CMAKE_SOURCE_DIR}/tests/semantics/tool/semtool.cpp")
target_include_directories(
  fcp_semtool PRIVATE "${CMAKE_SOURCE_DIR}/rexglue/src" "${CMAKE_SOURCE_DIR}/rexglue/thirdparty/disasm"
)
target_link_libraries(fcp_semtool PRIVATE rexcodegen tomlplusplus disasm)
rexglue_link_host_allocator(fcp_semtool)

# _fcp_sem_runner(<runner> <gen_dir> <generated sources>): the runner of one variant. With FCP_GAME_CODE_DLL the
# test functions are compiled exactly like predator_code.dll (freestanding headers and flags of the game code,
# predator.exe --fcp-build-game-code, kind=semtest) into <runner>_code.dll, which the hosted runner loads; so the
# semantic test checks the code path of the game. Without it as before (hosted, linked into the runner).
function (_fcp_sem_runner name gen_dir)
  set(sources ${ARGN})
  if (FCP_GAME_CODE_DLL)
    get_filename_component(llvm_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(dll "${REX_OUTPUT_DIR}/${name}_code.dll")
    set(settings "kind=semtest\ngen=${gen_dir}\ndll=${dll}\nwork=${_st}/${name}_work\norigin=build\n")
    string(APPEND settings "clang=${CMAKE_CXX_COMPILER}\nlld_link=${llvm_bin}/lld-link.exe\n")
    string(APPEND settings "include=${CMAKE_SOURCE_DIR}/rexglue/include\ninclude=${FCP_DEPS_DIR}/simde\n")
    string(APPEND settings "include=${FCP_DEPS_DIR}/simde/simde\ninclude=${CMAKE_SOURCE_DIR}/src\ninclude=${_sr}\n")
    string(APPEND settings "isystem=${CMAKE_SOURCE_DIR}/rexglue/include/rex/guest_std\n")
    string(APPEND settings "lib=$<TARGET_LINKER_FILE:rexppc>\nlib=$<TARGET_LINKER_FILE:rexbase>\n")
    file(GLOB defs "${CMAKE_SOURCE_DIR}/src/fcp/game_code/*.def")
    set(libs "")
    foreach (def IN LISTS defs)
      get_filename_component(lib_name "${def}" NAME_WE)
      string(APPEND settings "lib=${CMAKE_BINARY_DIR}/game_code_libs/${lib_name}.lib\n")
      list(APPEND libs "${CMAKE_BINARY_DIR}/game_code_libs/${lib_name}.lib")
    endforeach ()
    string(APPEND settings "runtime=${CMAKE_SOURCE_DIR}/src/fcp/game_code/runtime.cpp\ndata_export=fcp_sem_funcs\n")
    file(GENERATE OUTPUT "${_st}/${name}_build.txt" CONTENT "${settings}")
    add_custom_command(
      OUTPUT "${dll}"
      COMMAND $<TARGET_FILE:predator> --fcp-build-game-code "${_st}/${name}_build.txt"
      DEPENDS predator "${_st}/${name}_build.txt" ${sources} ${libs}
              "${CMAKE_SOURCE_DIR}/src/fcp/game_code/runtime.cpp"
      COMMENT "semtest: compiling ${name}_code.dll like predator_code.dll"
      VERBATIM
    )
    add_custom_target(${name}_code DEPENDS "${dll}")
    add_dependencies(${name}_code predator_code_libs)
    add_executable(${name} "${_sr}/sem_runner.cpp")
    add_dependencies(${name} ${name}_code)
    target_compile_definitions(${name} PRIVATE FCP_SEM_DLL="${name}_code.dll")
  else ()
    add_executable(${name} ${sources} "${_sr}/sem_runner.cpp")
  endif ()
  target_include_directories(${name} PRIVATE "${gen_dir}" "${_sr}" "${FCP_GEN_DIR}")
  target_compile_definitions(${name} PRIVATE FCP_SEM_REX _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(${name} PRIVATE rex::sdk)
  rexglue_apply_target_settings(${name})
  rexglue_apply_recomp_settings(${name} "${FCP_GEN_DIR}/predator_pch.h")
  target_compile_options(${name} PRIVATE -Wno-unused-label -Wno-unused-variable)
  add_dependencies(${name} predator_codegen)
endfunction ()

if (FCP_SEMTEST)
  set(_st "${CMAKE_BINARY_DIR}/semtest")
  set(_sp "${CMAKE_SOURCE_DIR}/tests/semantics/py")
  set(_sr "${CMAKE_SOURCE_DIR}/tests/semantics/runner")
  set(_shards 8)
  file(MAKE_DIRECTORY "${_st}/gen" "${_st}/rex" "${_st}/xr_in" "${_st}/xr")
  set(_py_sources
      "${_sp}/gen.py" "${_sp}/ppcref.py" "${_sp}/ppcref_more.py" "${_sp}/xenia_lookup.py"
      "${_sp}/compare.py" "${_sp}/run.py" "${_sp}/xr_table.py" "${CMAKE_SOURCE_DIR}/tests/semantics/forms.json"
      "${CMAKE_SOURCE_DIR}/tests/semantics/leaf_sample.toml"
  )
  file(GLOB _hw_vectors "${CMAKE_SOURCE_DIR}/rexglue/tests/unit/ppc/*_cases.h")

  # a. Test functions, inputs and expected results (deterministic; contains no game code). With the
  #    leaf sample, functions of the game are taken from out/game/default.bin into semtest/gen (local).
  set(_gen_out "${_st}/gen/sem.bin" "${_st}/gen/sem_syms.txt" "${_st}/gen/sem.elf" "${_st}/gen/cases.bin"
               "${_st}/gen/expected.bin" "${_st}/gen/meta.json"
  )
  add_custom_command(
    OUTPUT ${_gen_out}
    COMMAND "${FCP_PYTHON}" -I "${_sp}/gen.py" "${CMAKE_SOURCE_DIR}/tests/semantics/forms.json"
            "${CMAKE_SOURCE_DIR}/rexglue/tests/unit/ppc" "${_st}/gen" --leaf
            "${CMAKE_SOURCE_DIR}/tests/semantics/leaf_sample.toml" --image "${FCP_GAME_DIR}/default.bin"
    DEPENDS ${_py_sources} ${_hw_vectors} "${FCP_GAME_DIR}/default.bin"
    COMMENT "semtest: generating test functions and cases"
    VERBATIM
  )

  # b. Our codegen with the title configuration, against the PCH of the game build (same macros).
  set(_rex_out "${_st}/rex/sem_table.cpp" "${_st}/rex/sem_decls.h")
  foreach (i RANGE 1 ${_shards})
    math(EXPR n "${i} - 1")
    list(APPEND _rex_out "${_st}/rex/sem_funcs_${n}.cpp")
  endforeach ()
  add_custom_command(
    OUTPUT ${_rex_out}
    COMMAND $<TARGET_FILE:fcp_semtool> emit "${_st}/gen/sem.bin" 82000000 "${_st}/gen/sem_syms.txt"
            "${FCP_TITLE_CONFIG}" "${_st}/rex" ${_shards} "${FCP_GEN_DIR}/predator_pch.h"
    DEPENDS fcp_semtool ${_gen_out} "${FCP_TITLE_CONFIG}" "${FCP_GEN_DIR}/codegen.build.stamp"
    COMMENT "semtest: recompiling the test functions with our codegen"
    VERBATIM
  )
  _fcp_sem_runner(fcp_semtest_rex "${_st}/rex" ${_rex_out})


  # b2. The same with every [vector_math] key exact (tests/semantics/vector_exact.toml), informative only.
  file(MAKE_DIRECTORY "${_st}/rex_exact")
  string(REPLACE "${_st}/rex/" "${_st}/rex_exact/" _rexx_out "${_rex_out}")
  add_custom_command(
    OUTPUT ${_rexx_out}
    COMMAND $<TARGET_FILE:fcp_semtool> emit "${_st}/gen/sem.bin" 82000000 "${_st}/gen/sem_syms.txt"
            "${CMAKE_SOURCE_DIR}/tests/semantics/vector_exact.toml" "${_st}/rex_exact" ${_shards}
            "${FCP_GEN_DIR}/predator_pch.h"
    DEPENDS fcp_semtool ${_gen_out} "${FCP_TITLE_CONFIG}" "${CMAKE_SOURCE_DIR}/tests/semantics/vector_exact.toml"
            "${FCP_GEN_DIR}/codegen.build.stamp"
    COMMENT "semtest: recompiling the test functions with exact vector math"
    VERBATIM
  )
  _fcp_sem_runner(fcp_semtest_rex_exact "${_st}/rex_exact" ${_rexx_out})


  # b3. FCP: the same with [fp_rules] on (tests/semantics/fp_rules.toml). The
  #     run fails when this variant is worse than b. in any case.
  file(MAKE_DIRECTORY "${_st}/rex_fprules")
  string(REPLACE "${_st}/rex/" "${_st}/rex_fprules/" _rexf_out "${_rex_out}")
  add_custom_command(
    OUTPUT ${_rexf_out}
    COMMAND $<TARGET_FILE:fcp_semtool> emit "${_st}/gen/sem.bin" 82000000 "${_st}/gen/sem_syms.txt"
            "${CMAKE_SOURCE_DIR}/tests/semantics/fp_rules.toml" "${_st}/rex_fprules" ${_shards}
            "${FCP_GEN_DIR}/predator_pch.h"
    DEPENDS fcp_semtool ${_gen_out} "${FCP_TITLE_CONFIG}" "${CMAKE_SOURCE_DIR}/tests/semantics/fp_rules.toml"
            "${FCP_GEN_DIR}/codegen.build.stamp"
    COMMENT "semtest: recompiling the test functions with [fp_rules]"
    VERBATIM
  )
  _fcp_sem_runner(fcp_semtest_rex_fprules "${_st}/rex_fprules" ${_rexf_out})


  # c. XenonRecomp (optional): its test mode reads ELF objects from a folder.
  set(_runners $<TARGET_FILE:fcp_semtest_rex> $<TARGET_FILE:fcp_semtest_rex_exact>
               $<TARGET_FILE:fcp_semtest_rex_fprules>)
  set(_run_depends fcp_semtest_rex fcp_semtest_rex_exact fcp_semtest_rex_fprules)
  if (FCP_XENONRECOMP_DIR)
    set(_xr_exe "${FCP_XENONRECOMP_DIR}/out/build/XenonRecomp/XenonRecomp.exe")
    add_custom_command(
      OUTPUT "${_st}/xr/sem.cpp" "${_st}/xr/xr_table.cpp"
      COMMAND ${CMAKE_COMMAND} -E copy "${_st}/gen/sem.elf" "${_st}/xr_in/sem.o"
      COMMAND "${_xr_exe}" "${_st}/xr_in" "${_st}/xr" > "${_st}/xr/xenonrecomp.log"
      COMMAND "${FCP_PYTHON}" -I "${_sp}/xr_table.py" "${_st}/xr" sem "${_st}/xr/xr_table.cpp"
      DEPENDS "${_st}/gen/sem.elf" "${_xr_exe}" "${_sp}/xr_table.py"
      COMMENT "semtest: recompiling the test functions with XenonRecomp"
      VERBATIM
    )
    add_executable(fcp_semtest_xr "${_st}/xr/sem.cpp" "${_st}/xr/xr_table.cpp" "${_sr}/sem_runner.cpp")
    target_include_directories(
      fcp_semtest_xr PRIVATE "${_sr}" "${FCP_XENONRECOMP_DIR}/XenonUtils"
                             "${FCP_XENONRECOMP_DIR}/thirdparty/simde"
    )
    target_compile_definitions(fcp_semtest_xr PRIVATE FCP_SEM_XR PPC_CONFIG_H_INCLUDED)
    # Flags of the XenonRecomp dry run in phase 0 (the flags XenonRecomp's generated code needs)
    target_compile_options(
      fcp_semtest_xr PRIVATE -O2 -march=sandybridge -fno-strict-aliasing -ffp-model=strict
                             -fasync-exceptions -w
    )
    list(APPEND _runners $<TARGET_FILE:fcp_semtest_xr>)
    list(APPEND _run_depends fcp_semtest_xr)
  endif ()

  # d. Run and compare. Fails the build on a failure that tests/semantics/known.toml does not list.
  add_custom_command(
    OUTPUT "${_st}/semtest.stamp"
    COMMAND "${FCP_PYTHON}" -I "${_sp}/run.py" --gen "${_st}/gen" --out "${_st}" --known
            "${CMAKE_SOURCE_DIR}/tests/semantics/known.toml" --runners ${_runners} --stamp "${_st}/semtest.stamp"
    DEPENDS ${_run_depends} ${_gen_out} "${_sp}/compare.py" "${_sp}/run.py"
            "${CMAKE_SOURCE_DIR}/tests/semantics/known.toml"
    COMMENT "semtest: running"
    VERBATIM
  )
  add_custom_target(predator_semtest ALL DEPENDS "${_st}/semtest.stamp")
endif ()
