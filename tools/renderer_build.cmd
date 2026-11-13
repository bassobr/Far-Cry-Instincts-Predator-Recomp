@echo off
rem Builds the whole project (build.cmd) with the shared paths of the main checkout, for worktrees without their
rem own toolchain\, assets\, SDK and renderer dependencies. Values already set are kept. Main checkout:
rem FCP_SHARED_ROOT, default from tools\shared_root.cmd.
rem   cmd /c tools\renderer_build.cmd [release|relwithdebinfo|debug] [--log]
rem With --log the output goes to out\logs\build_<cfg>.log (like tools\boot\wt_build.cmd).
setlocal
call "%~dp0shared_root.cmd"
set "MAIN=%FCP_SHARED_ROOT%"
if not defined FCP_TOOLCHAIN_DIR set "FCP_TOOLCHAIN_DIR=%MAIN%\toolchain"
if not defined FCP_DEPS_DIR set "FCP_DEPS_DIR=%MAIN%\third_party\rexglue-deps"
if not defined FCP_RENDERER_DEPS_DIR set "FCP_RENDERER_DEPS_DIR=%MAIN%\third_party\renderer-deps"
if not defined FCP_GAME_ISO set "FCP_GAME_ISO=%MAIN%\assets\Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
set "WT=%~dp0.."
if /i "%~2"=="--log" (
  if not exist "%WT%\out\logs" mkdir "%WT%\out\logs"
  call "%WT%\build.cmd" %CFG% > "%WT%\out\logs\build_%CFG%.log" 2>&1
) else (
  call "%WT%\build.cmd" %CFG%
)
set "RC=%ERRORLEVEL%"
echo renderer_build: exit %RC%
exit /b %RC%
