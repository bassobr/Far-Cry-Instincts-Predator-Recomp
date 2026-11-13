@echo off
rem Builds fcshaders (tools\shaders) standalone with clang-cl (LLVM from FCP_TOOLCHAIN_DIR) and Ninja into
rem work\build\fcshaders. The renderer build (src\renderer) includes fcshaders anyway.
rem   cmd /c tools\shaders\build.cmd
rem Prerequisite: powershell -NoProfile -ExecutionPolicy Bypass -File tools\get_renderer_deps.ps1
rem (fmt, xxHash, DXC under third_party\renderer-deps; other location via FCP_RENDERER_DEPS_DIR).
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
call "%~dp0..\shared_root.cmd"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%FCP_SHARED_ROOT%\toolchain"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%FCP_TOOLCHAIN_DIR%\llvm\bin;%PATH%"
set "B=%ROOT%\work\build\fcshaders"
if not exist "%B%" mkdir "%B%"
cmake -S "%ROOT%\tools\shaders" -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang-cl > "%B%\configure.log" 2>&1 || (type "%B%\configure.log" & exit /b 1)
cmake --build "%B%" > "%B%\build.log" 2>&1 || (type "%B%\build.log" & exit /b 1)
echo done: %B%\fcshaders.exe
