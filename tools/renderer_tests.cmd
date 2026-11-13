@echo off
rem Builds the native renderer on its own (without SDK and codegen) and runs all of its tests:
rem   cmd /c tools\renderer_tests.cmd [release|debug] [--no-disc]
rem 1. configure and build into out\build\renderer-<cfg> (clang, Ninja, pinned toolchain)
rem 2. predator_renderer_res_tests (King Kong's resource tests) and predator_renderer_tests (interfaces,
rem    pack/cache, pipeline cache keys, King Kong's backend tests, GPU cases without a window)
rem 3. with the disc (FCP_DISC_DIR, default: the unpacked disc in work\disc of the main checkout, read only): derive
rem    game data into work\ (shader containers, DDS from common.fat, disc packs DXIL and SPIR-V; never commit them)
rem    and run renderer_disc_tests.
rem Without the disc step 3 is skipped. Prerequisite: tools\get_renderer_deps.ps1. Main checkout: FCP_SHARED_ROOT,
rem default from tools\shared_root.cmd.
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
call "%~dp0shared_root.cmd"
set "MAIN=%FCP_SHARED_ROOT%"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%MAIN%\toolchain"
if not defined FCP_DEPS_DIR set "FCP_DEPS_DIR=%MAIN%\third_party\rexglue-deps"
if not defined FCP_RENDERER_DEPS_DIR set "FCP_RENDERER_DEPS_DIR=%MAIN%\third_party\renderer-deps"
if not defined FCP_DISC_DIR set "FCP_DISC_DIR=%MAIN%\work\disc"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
if /i "%CFG%"=="--no-disc" (set "CFG=release" & set "NODISC=1")
if /i "%~2"=="--no-disc" set "NODISC=1"
set "BT=Release"
if /i "%CFG%"=="debug" set "BT=Debug"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
set "PY=%FCP_TOOLCHAIN_DIR%\venv\Scripts\python.exe"
set "B=%ROOT%\out\build\renderer-%CFG%"
cd /d "%ROOT%" || exit /b 1
if not exist "%ROOT%\out\build" mkdir "%ROOT%\out\build"

if not exist "%B%\build.ninja" (
  echo [%time%] configure %B%
  cmake -S src\renderer -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=%BT% -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-march=x86-64-v2 > "%B%.configure.log" 2>&1 || (type "%B%.configure.log" & exit /b 1)
)
echo [%time%] build
cmake --build "%B%" || exit /b 1
set "BIN=%B%\bin"
set "FAIL=0"
echo [%time%] predator_renderer_res_tests
"%BIN%\predator_renderer_res_tests.exe" || set "FAIL=1"
echo [%time%] predator_renderer_tests
"%BIN%\predator_renderer_tests.exe" || set "FAIL=1"

if defined NODISC goto :done
if not exist "%FCP_DISC_DIR%\common.fat" (
  echo [%time%] disc missing ^(%FCP_DISC_DIR%^): renderer_disc_tests skipped
  goto :done
)
set "W=%ROOT%\work"
if not exist "%W%\shaders\disc\fxo" (
  echo [%time%] shader containers from the disc
  "%PY%" -I tools\shaders\fxo.py extract "%FCP_DISC_DIR%" "%W%\shaders\disc" || exit /b 1
)
if not exist "%W%\renderer\common\esrb_fci.xbt" (
  echo [%time%] unpacking common.fat
  "%PY%" -I tools\xfat.py extract "%FCP_DISC_DIR%\common.fat" "%W%\renderer\common" || exit /b 1
)
set "DXC=%FCP_RENDERER_DEPS_DIR%"
if "%DXC%"=="" set "DXC=%ROOT%\third_party\renderer-deps"
if not exist "%W%\shaders\predator.spv.fcpack" "%BIN%\fcshaders.exe" pack "%W%\shaders\disc\fxo" --dxc "%DXC%\dxc\bin\x64" --out "%W%\shaders\predator.spv.fcpack" --target spirv || exit /b 1
if not exist "%W%\shaders\predator.dxil.fcpack" "%BIN%\fcshaders.exe" pack "%W%\shaders\disc\fxo" --dxc "%DXC%\dxc\bin\x64" --out "%W%\shaders\predator.dxil.fcpack" --target dxil || exit /b 1
echo [%time%] renderer_disc_tests
"%BIN%\renderer_disc_tests.exe" || set "FAIL=1"

:done
if "%FAIL%"=="1" (
  echo [%time%] FAILED
  exit /b 1
)
echo [%time%] all tests passed
exit /b 0
