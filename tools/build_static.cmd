@echo off
rem Builds the comparison build with the game code inside predator.exe (-DFCP_GAME_CODE_DLL=OFF, the fallback) in
rem out\build\static, for old/new performance comparisons against the game code DLL. Needs generated\default
rem (run build.cmd once). Same toolchain and flags as the release preset.
rem   cmd /c tools\build_static.cmd
setlocal
set "ROOT=%~dp0.."
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%ROOT%\toolchain"
if not defined FCP_VCVARS set "FCP_VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%FCP_VCVARS%" >nul || exit /b 1
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
cd /d "%ROOT%" || exit /b 1
set "B=%ROOT%\out\build\static"
if not exist "%B%\build.ninja" (
  cmake -S . -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
    "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON -DFCP_GAME_CODE_DLL=OFF ^
    -DFCP_SEMTEST=OFF || exit /b 1
)
cmake --build "%B%" --target predator || exit /b 1
rem The renderer loads DXC from the EXE folder; the target predator alone does not put it there (without it the
rem renderer does not start and the hooks only call the originals: performance numbers would then be worthless).
if not defined FCP_RENDERER_DEPS_DIR set "FCP_RENDERER_DEPS_DIR=%ROOT%\third_party\renderer-deps"
copy /y "%FCP_RENDERER_DEPS_DIR%\dxc\bin\x64\dxcompiler.dll" "%B%\bin\" >nul || exit /b 1
echo build_static: done %B%\bin\predator.exe
exit /b 0
