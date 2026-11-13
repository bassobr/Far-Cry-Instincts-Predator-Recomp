@echo off
rem Builds predator.exe and the release folder including the zip (target dist, tools\dist.ps1) in one step:
rem   cmd /c tools\dist.cmd [release]
rem Like build.cmd with the pinned toolchain; needs no disc image if the build folder is already configured
rem (target dist depends only on predator, fcshaders and the import libraries, never on predator_code.dll).
setlocal
set "ROOT=%~dp0.."
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%ROOT%\toolchain"
if not defined FCP_VCVARS set "FCP_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%FCP_VCVARS%" >nul || exit /b 1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
cd /d "%ROOT%" || exit /b 1
if not exist "out\build\%CFG%\build.ninja" (
  cmake --preset %CFG% || exit /b 1
)
cmake --build --preset %CFG% --target dist || exit /b 1
exit /b 0
