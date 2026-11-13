@echo off
rem PGO training build: the game code linked into predator.exe (-DFCP_GAME_CODE_DLL=OFF) and instrumented
rem (-DFCP_PGO=generate) in out\build\pgo-train. predator_code.dll cannot carry the profile runtime (no CRT, no
rem entry point), so tools\pgo.ps1 trains this build and then builds the DLL with the merged profile
rem (FCP_PGO=use). The generated sources are the same (generated\default, from build.cmd), so the profile
rem matches the DLL's functions by name and structure hash.
rem   cmd /c tools\pgo_train_build.cmd
rem Needs a normal build first (generated\default). Uses the shared toolchain/dependencies like
rem tools\boot\wt_build.cmd (works in the main checkout and in worktrees).
setlocal
call "%~dp0shared_root.cmd"
set "ROOT=%~dp0.."
for %%P in ("%ROOT%") do set "ROOT=%%~fP"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%FCP_SHARED_ROOT%\toolchain"
if not defined FCP_DEPS_DIR set "FCP_DEPS_DIR=%FCP_SHARED_ROOT%\third_party\rexglue-deps"
if not defined FCP_RENDERER_DEPS_DIR set "FCP_RENDERER_DEPS_DIR=%FCP_SHARED_ROOT%\third_party\renderer-deps"
if not defined FCP_GAME_ISO set "FCP_GAME_ISO=%FCP_SHARED_ROOT%\assets\Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso"
if not defined FCP_LZMA_DIR if exist "%FCP_SHARED_ROOT%\third_party\lzma-sdk\LzmaDec.c" set "FCP_LZMA_DIR=%FCP_SHARED_ROOT%\third_party\lzma-sdk"
if not defined FCP_LZMA_DIR set "FCP_LZMA_DIR=%ROOT%\third_party\lzma-sdk"
if not defined FCP_VCVARS set "FCP_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%FCP_VCVARS%" >nul || exit /b 1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
cd /d "%ROOT%" || exit /b 1
if not exist "%ROOT%\generated\default\sources.cmake" (
  echo pgo_train_build: generated\default missing, run build.cmd first
  exit /b 1
)
set "B=%ROOT%\out\build\pgo-train"
if not exist "%B%\build.ninja" (
  cmake -S . -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DFCP_GAME_CODE_DLL=OFF ^
    -DFCP_SEMTEST=OFF -DFCP_PGO=generate "-DFCP_LZMA_DIR=%FCP_LZMA_DIR%" || exit /b 1
)
cmake --build "%B%" --target predator || exit /b 1
rem The renderer loads DXC from the EXE folder (without it the renderer does not start: worthless training).
copy /y "%FCP_RENDERER_DEPS_DIR%\dxc\bin\x64\dxcompiler.dll" "%B%\bin\" >nul || exit /b 1
echo pgo_train_build: done %B%\bin\predator.exe
exit /b 0
