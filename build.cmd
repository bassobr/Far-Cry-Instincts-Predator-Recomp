@echo off
rem Configures and builds predator.exe with the pinned toolchain (toolchain\llvm, clang 22.1.8) and the
rem VS Build Tools 2022 (only the Windows SDK and the linker libraries).
rem
rem   cmd /c build.cmd [release|relwithdebinfo|debug]      (default: release)
rem
rem Environment variables (all optional):
rem   FCP_GAME_ISO       disc image (default: assets\Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso)
rem   FCP_DEPS_DIR       SDK dependencies from tools\get_deps.ps1 (default: third_party\rexglue-deps)
rem   FCP_TOOLCHAIN_DIR  folder with llvm\ and venv\ (default: toolchain)
rem   FCP_VCVARS         path to vcvars64.bat (default: VS Build Tools 2022, else vswhere)
rem   FCP_JOBS           parallel jobs (default: Ninja's default)
rem The FCP_* paths apply when a build folder is configured for the first time; after that they are in the CMake cache.
rem In a Git worktree without its own toolchain\, third_party\ and assets\: tools\boot\wt_build.cmd.
rem
rem Steps: configure (only without build.ninja) -> predator_coverage (ISO -> default.xex -> codegen ->
rem coverage check) -> everything: predator.exe (without game code), then predator_code.dll (predator.exe
rem --fcp-build-game-code, the same path as on the first start), semantic test.
rem Output: out\build\<config>\bin\predator.exe and bin\game_code\predator_code.dll, generated code in
rem generated\default. Release folder: cmake --build --preset release --target dist (tools\dist.ps1).
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
if /i not "%CFG%"=="release" if /i not "%CFG%"=="relwithdebinfo" if /i not "%CFG%"=="debug" (
  echo build.cmd: unknown configuration "%CFG%" ^(release, relwithdebinfo, debug^)
  exit /b 2
)

if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%ROOT%\toolchain"
if not exist "%FCP_TOOLCHAIN_DIR%\llvm\bin\clang++.exe" (
  echo build.cmd: clang missing in "%FCP_TOOLCHAIN_DIR%\llvm\bin" ^(tools\get_llvm.ps1 or FCP_TOOLCHAIN_DIR^)
  exit /b 1
)

if not defined FCP_VCVARS set "FCP_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%FCP_VCVARS%" (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "FCP_VCVARS=%%I"
)
if not exist "%FCP_VCVARS%" (
  echo build.cmd: vcvars64.bat not found ^(set FCP_VCVARS^)
  exit /b 1
)
call "%FCP_VCVARS%" >nul || exit /b 1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"

set "JOBS="
if defined FCP_JOBS set "JOBS=--parallel %FCP_JOBS%"

cd /d "%ROOT%" || exit /b 1
rem LZMA decoder for the first start (shader pack from the image), fetched once and checked by SHA-256.
if not defined FCP_LZMA_DIR if not exist "%ROOT%\third_party\lzma-sdk\LzmaDec.c" (
  echo [%time%] fetching the LZMA SDK ^(tools\get_lzma.ps1^)
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\get_lzma.ps1" || exit /b 1
)
rem Configure only when needed: every CMake run renews the time stamp in rexbase, which relinks the SDK DLLs
rem and rexglue.exe and so restarts the codegen. Later changes are detected by Ninja itself.
set "BDIR=%ROOT%\out\build\%CFG%"
if not exist "%BDIR%\build.ninja" (
  echo [%time%] configure %CFG%
  cmake --preset %CFG% || exit /b 1
) else (
  rem If build.ninja is outdated (CMake files changed), configure first. Otherwise Ninja would start the codegen
  rem from the old graph before reconfiguring (sources.cmake is a configure dependency), and new build steps
  rem before the codegen would be missing. See tools\needs_configure.py.
  "%FCP_TOOLCHAIN_DIR%\venv\Scripts\python.exe" -I "%ROOT%\tools\needs_configure.py" "%BDIR%"
  if errorlevel 1 (
    echo [%time%] configure %CFG% ^(build.ninja outdated^)
    cmake --preset %CFG% || exit /b 1
  )
)
echo [%time%] codegen and coverage check
cmake --build --preset %CFG% --target predator_coverage %JOBS% || exit /b 1
rem Only the static build (-DFCP_GAME_CODE_DLL=OFF) needs the codegen's source list at configure time; the
rem default build (predator_code.dll, cmake\fcp_game_code.cmake) does not.
findstr /c:"predator_code.dll" "%BDIR%\build.ninja" >nul 2>&1
if errorlevel 1 (
  findstr /c:"predator_recompiled" "%BDIR%\build.ninja" >nul 2>&1
  if errorlevel 1 (
    echo [%time%] configure %CFG% with the codegen's source list
    cmake --preset %CFG% >nul || exit /b 1
  )
)
echo [%time%] predator and predator_code.dll
cmake --build --preset %CFG% %JOBS% || exit /b 1
echo [%time%] done: out\build\%CFG%\bin\predator.exe, bin\game_code\predator_code.dll
exit /b 0
