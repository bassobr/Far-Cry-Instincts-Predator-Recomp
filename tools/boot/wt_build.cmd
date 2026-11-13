@echo off
rem Builds predator.exe in a worktree against the shared, read-only copies in the main checkout.
rem   cmd /c tools\boot\wt_build.cmd [release|relwithdebinfo|debug]
rem FCP_SHARED_ROOT overrides the location of the main checkout (default: tools\shared_root.cmd, i.e. the main
rem checkout of this worktree, or this checkout itself). Log: out\logs\build_<cfg>.log
setlocal
call "%~dp0..\shared_root.cmd"
set "FCP_TOOLCHAIN_DIR=%FCP_SHARED_ROOT%\toolchain"
set "FCP_DEPS_DIR=%FCP_SHARED_ROOT%\third_party\rexglue-deps"
if not defined FCP_RENDERER_DEPS_DIR set "FCP_RENDERER_DEPS_DIR=%FCP_SHARED_ROOT%\third_party\renderer-deps"
set "FCP_GAME_ISO=%FCP_SHARED_ROOT%\assets\Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso"
set "CFG=%~1"
if "%CFG%"=="" set "CFG=release"
set "WT=%~dp0..\.."
if not exist "%WT%\out\logs" mkdir "%WT%\out\logs"
call "%WT%\build.cmd" %CFG% > "%WT%\out\logs\build_%CFG%.log" 2>&1
set "RC=%ERRORLEVEL%"
echo wt_build: exit %RC% (log: out\logs\build_%CFG%.log)
exit /b %RC%
