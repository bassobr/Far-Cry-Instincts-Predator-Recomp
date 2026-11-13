@echo off
rem Builds the renderer's shader packs from the user's disc image (derived game data: only under out\, never in
rem the repository). predator.exe then finds them under bin\shaders\predator.<spirv|dxil>.fcpack and no longer
rem translates the 1,958 disc shaders at run time (only the ~86 run-time shaders that the disk cache holds).
rem   cmd /c tools\renderer_pack.cmd [release|relwithdebinfo|debug] [spirv|dxil|both]
rem Inputs: FCP_GAME_ISO (image), FCP_TOOLCHAIN_DIR (Python); defaults under FCP_SHARED_ROOT (tools\shared_root.cmd).
rem Intermediate: out\pack\ (containers from shaders.fat/common.fat). An existing pack with a matching stamp is not
rem rebuilt (fcshaders checks the stamp on load; the renderer logs an outdated pack and then translates itself).
setlocal
set "WT=%~dp0.."
call "%~dp0shared_root.cmd"
set "MAIN=%FCP_SHARED_ROOT%"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%MAIN%\toolchain"
if not defined FCP_GAME_ISO set "FCP_GAME_ISO=%MAIN%\assets\Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso"
set "PY=%FCP_TOOLCHAIN_DIR%\venv\Scripts\python.exe"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=relwithdebinfo"
set "TARGETS=%~2"
if "%TARGETS%"=="" set "TARGETS=both"
set "BIN=%WT%\out\build\%CFG%\bin"
set "P=%WT%\out\pack"
if not exist "%BIN%\fcshaders.exe" (
  echo renderer_pack: %BIN%\fcshaders.exe missing ^(build first^)
  exit /b 1
)
if not exist "%P%\disc\shaders.fat" (
  echo [%time%] shaders/common from the image
  "%PY%" -I "%WT%\tools\xiso.py" "%FCP_GAME_ISO%" --out "%P%\disc" --only "shaders.*" --only "common.*" || exit /b 1
)
if not exist "%P%\shaders\fxo" (
  echo [%time%] unpacking containers
  "%PY%" -I "%WT%\tools\shaders\fxo.py" extract "%P%\disc" "%P%\shaders" || exit /b 1
)
if not exist "%BIN%\shaders" mkdir "%BIN%\shaders"
if /i not "%TARGETS%"=="dxil" (
  echo [%time%] Pack SPIR-V
  "%BIN%\fcshaders.exe" pack "%P%\shaders\fxo" --dxc "%BIN%" --out "%BIN%\shaders\predator.spirv.fcpack" --target spirv || exit /b 1
)
if /i not "%TARGETS%"=="spirv" (
  echo [%time%] Pack DXIL
  "%BIN%\fcshaders.exe" pack "%P%\shaders\fxo" --dxc "%BIN%" --out "%BIN%\shaders\predator.dxil.fcpack" --target dxil || exit /b 1
)
echo [%time%] renderer_pack: done
exit /b 0
