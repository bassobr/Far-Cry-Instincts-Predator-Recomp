@echo off
rem Builds the pipeline measurement programs (pso_d3d12, pso_vulkan) with clang-cl into work\build\renderer-probe.
rem   cmd /c tools\renderer-probe\build.cmd
rem Prerequisite: bash tools/renderer-probe/get_third_party.sh (Vulkan-Headers, volk under third_party\).
rem Toolchain: FC_TOOLCHAIN, else FCP_TOOLCHAIN_DIR, else <FCP_SHARED_ROOT>\toolchain (tools\shared_root.cmd).
setlocal
set "ROOT=%~dp0..\.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
call "%~dp0..\shared_root.cmd"
if "%FC_TOOLCHAIN%"=="" if defined FCP_TOOLCHAIN_DIR set "FC_TOOLCHAIN=%FCP_TOOLCHAIN_DIR%"
if "%FC_TOOLCHAIN%"=="" set "FC_TOOLCHAIN=%FCP_SHARED_ROOT%\toolchain"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%FC_TOOLCHAIN%\llvm\bin;%PATH%"
set "B=%ROOT%\work\build\renderer-probe"
set "S=%ROOT%\tools\renderer-probe"
set "T=%ROOT%\third_party"
if not exist "%B%" mkdir "%B%"
clang-cl /nologo /std:c++20 /O2 /EHsc /MD /W3 "%S%\pso_d3d12.cpp" /Fo"%B%\pso_d3d12.obj" /Fe"%B%\pso_d3d12.exe" /link d3d12.lib dxgi.lib dxguid.lib || exit /b 1
clang-cl /nologo /O2 /MD /c /DVK_NO_PROTOTYPES /I"%T%\Vulkan-Headers\include" "%T%\volk\volk.c" /Fo"%B%\volk.obj" || exit /b 1
clang-cl /nologo /std:c++20 /O2 /EHsc /MD /W3 /I"%T%\Vulkan-Headers\include" /I"%T%\volk" /c "%S%\pso_vulkan.cpp" /Fo"%B%\pso_vulkan.obj" || exit /b 1
lld-link /nologo "%B%\pso_vulkan.obj" "%B%\volk.obj" /out:"%B%\pso_vulkan.exe" || exit /b 1
echo done: %B%
