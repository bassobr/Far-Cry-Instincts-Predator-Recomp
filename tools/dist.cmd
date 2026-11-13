@echo off
rem Builds predator.exe and the release folder including the zip (target dist, tools\dist.ps1) in one step:
rem   cmd /c tools\dist.cmd [release]
rem Like build.cmd with the pinned toolchain. Needs no disc image: the target dist depends only on predator, fcshaders
rem and the import libraries, never on the codegen or predator_code.dll (the configure step only warns that
rem FCP_GAME_ISO is missing). Same environment variables as build.cmd (FCP_TOOLCHAIN_DIR, FCP_VCVARS, FCP_DEPS_DIR,
rem FCP_RENDERER_DEPS_DIR, FCP_LZMA_DIR, FCP_JOBS).
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%ROOT%\toolchain"
if not exist "%FCP_TOOLCHAIN_DIR%\llvm\bin\clang++.exe" (
  echo dist.cmd: clang missing in "%FCP_TOOLCHAIN_DIR%\llvm\bin" ^(tools\get_llvm.ps1 or FCP_TOOLCHAIN_DIR^)
  exit /b 1
)
if not defined FCP_VCVARS set "FCP_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%FCP_VCVARS%" (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat`) do set "FCP_VCVARS=%%I"
)
if not exist "%FCP_VCVARS%" (
  echo dist.cmd: vcvars64.bat not found ^(set FCP_VCVARS^)
  exit /b 1
)
call "%FCP_VCVARS%" >nul || exit /b 1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
set "JOBS="
if defined FCP_JOBS set "JOBS=--parallel %FCP_JOBS%"
cd /d "%ROOT%" || exit /b 1
rem LZMA decoder (part of predator.exe: shader pack from the image on the first start), fetched once, SHA-256 checked.
if not defined FCP_LZMA_DIR if not exist "%ROOT%\third_party\lzma-sdk\LzmaDec.c" (
  echo [%time%] fetching the LZMA SDK ^(tools\get_lzma.ps1^)
  powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\get_lzma.ps1" || exit /b 1
)
if not exist "out\build\%CFG%\build.ninja" (
  cmake --preset %CFG% || exit /b 1
)
cmake --build --preset %CFG% --target dist %JOBS% || exit /b 1
exit /b 0
