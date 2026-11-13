@echo off
rem Semantic test of the PPC codegen (generated code checked against reference results per instruction).
rem
rem   cmd /c tools\semtest.cmd [release|relwithdebinfo|debug] [--sdk]
rem
rem Builds and runs the target predator_semtest in the existing build folder out\build\<cfg> (run
rem build.cmd once before). Regenerates the test functions when the generator or the reference changed, and
rem recompiles them when the codegen (rexcodegen) changed. Output: number of cases,
rem failures, duration; report out\build\<cfg>\semtest\report.txt. Exit code 0 = no new failures
rem (known ones are in tests\semantics\known.toml), 1 = new failures or build errors.
rem --sdk also runs the SDK's PPC tests (ppc_tests, fcp_sdk_ppc_unit; they need
rem third_party\ppc-binutils and Catch2).
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "CFG=release"
set "SDK="
:args
if "%~1"=="" goto run
if /i "%~1"=="--sdk" (set "SDK=1") else set "CFG=%~1"
shift
goto args
:run
if not exist "%ROOT%\out\build\%CFG%\build.ninja" (
  echo semtest.cmd: out\build\%CFG% is not configured ^(run build.cmd %CFG% first^)
  exit /b 1
)
set "START=%time%"
call "%ROOT%\tools\ninja_env.cmd" -C "out\build\%CFG%" predator_semtest
set "RC=%ERRORLEVEL%"
if defined SDK (
  call "%ROOT%\tools\ninja_env.cmd" -C "out\build\%CFG%" fcp_sdk_tests
  if errorlevel 1 set "RC=1"
)
echo semtest.cmd: start %START%, end %time%, exit %RC%
exit /b %RC%
