@echo off
rem Micro benchmark of the [fp_rules] helpers (NaN fidelity rules of the codegen). Builds
rem tests\semantics\tool\fp_rules_bench.cpp with the flags of the recompiled code and runs it.
rem   cmd /c tools\fp_rules_bench.cmd [loops]
setlocal
rem Toolchain and SDK dependencies: FCP_TOOLCHAIN_DIR / FCP_DEPS_DIR, default under FCP_SHARED_ROOT (tools\shared_root.cmd).
call "%~dp0shared_root.cmd"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%FCP_SHARED_ROOT%\toolchain"
if not defined FCP_DEPS_DIR set "FCP_DEPS_DIR=%FCP_SHARED_ROOT%\third_party\rexglue-deps"
set "WT=%~dp0.."
set "OUT=%WT%\out\bench"
if not exist "%OUT%" mkdir "%OUT%"
"%FCP_TOOLCHAIN_DIR%\llvm\bin\clang++.exe" -std=c++23 -O3 -msse4.1 -ffp-model=strict -fno-strict-aliasing ^
  -I"%WT%\rexglue\include" -I"%FCP_DEPS_DIR%\simde" -DNDEBUG ^
  "%WT%\tests\semantics\tool\fp_rules_bench.cpp" -o "%OUT%\fp_rules_bench.exe" || exit /b 1
"%OUT%\fp_rules_bench.exe" %1
