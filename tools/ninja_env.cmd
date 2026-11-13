@echo off
rem Starts ninja with the same environment as build.cmd (VS Build Tools 2022 + toolchain\llvm first in PATH).
rem   cmd /c tools\ninja_env.cmd -C out\build\release -j 32 predator
rem FCP_TOOLCHAIN_DIR and FCP_VCVARS as in build.cmd.
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%ROOT%\toolchain"
rem Without the pinned toolchain a reconfigure would find a different clang and discard the cache.
if not exist "%FCP_TOOLCHAIN_DIR%\llvm\bin\clang++.exe" (
  echo ninja_env.cmd: clang missing in "%FCP_TOOLCHAIN_DIR%\llvm\bin" ^(set FCP_TOOLCHAIN_DIR^)
  exit /b 1
)
if not defined FCP_VCVARS set "FCP_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%FCP_VCVARS%" >nul || exit /b 1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
cd /d "%ROOT%" || exit /b 1
ninja %*
exit /b %ERRORLEVEL%
