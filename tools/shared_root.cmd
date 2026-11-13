@echo off
rem Sets FCP_SHARED_ROOT (unless already set) to the checkout that holds the shared, read-only inputs
rem (toolchain\, third_party\, assets\, work\). In a Git worktree that is the main checkout (the parent of
rem "git rev-parse --git-common-dir"); in a normal checkout, or without Git, it is this repository itself.
rem   call "%~dp0shared_root.cmd"      (from tools\; from tools\boot\: call "%~dp0..\shared_root.cmd")
rem Called with "call" in the caller's setlocal scope, so the variable stays local to the caller.
if defined FCP_SHARED_ROOT goto :eof
set "FCP_SHARED_ROOT="
for /f "usebackq delims=" %%G in (`git -C "%~dp0.." rev-parse --path-format^=absolute --git-common-dir 2^>nul`) do (
  for %%P in ("%%G\..") do set "FCP_SHARED_ROOT=%%~fP"
)
if not defined FCP_SHARED_ROOT for %%P in ("%~dp0..") do set "FCP_SHARED_ROOT=%%~fP"
goto :eof
