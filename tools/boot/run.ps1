# Starts predator.exe for a short test run and ends only the process it started itself (by PID) if that one
# exceeds the hard time limit.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\boot\run.ps1 [-Config relwithdebinfo]
#       [-Seconds 30] [-HardSeconds 75] [-Env "FCP_TRACE_IMPORTS=1;FCP_TRACE_D3D=100,1"] [-Args ...]
# Default switches for test runs: FCP_NO_DIALOGS=1, FCP_MUTE=1, FCP_EXIT_AFTER=<Seconds>, window 640x360,
# FCP_GAME_ISO = the shared disc image (tools\shared_root.ps1; an FCP_GAME_ISO already set wins).
# Log: out\build\<cfg>\bin\predator.log
param(
  [string]$Config = "relwithdebinfo",
  [int]$Seconds = 30,
  [int]$HardSeconds = 0,
  [string[]]$Env = @(),
  [string[]]$Args = @()
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\..\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$exe = Join-Path $bin "predator.exe"
if (-not (Test-Path $exe)) { throw "missing: $exe" }
if ($HardSeconds -le 0) { $HardSeconds = $Seconds + 45 }

$vars = @{
  "FCP_GAME_ISO" = (Get-FcpGameIso)
  "FCP_NO_DIALOGS" = "1"
  "FCP_MUTE" = "1"
  "FCP_EXIT_AFTER" = "$Seconds"
  "REX_UI_WINDOW_WIDTH" = "640"
  "REX_UI_WINDOW_HEIGHT" = "360"
}
foreach ($kv in ($Env -join ";").Split(";")) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }

$start = Get-Date
$p = if ($Args.Count -gt 0) {
  Start-Process -FilePath $exe -WorkingDirectory $bin -ArgumentList $Args -PassThru
} else {
  Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
}
Write-Output "run: PID $($p.Id) started $($start.ToString('HH:mm:ss')), limit $Seconds s, hard $HardSeconds s"
$exited = $p.WaitForExit($HardSeconds * 1000)
if (-not $exited) {
  Write-Output "run: PID $($p.Id) still running after $HardSeconds s, ending it (this process only)"
  Stop-Process -Id $p.Id -Force
  $p.WaitForExit(10000) | Out-Null
}
$dur = ((Get-Date) - $start).TotalSeconds
Write-Output ("run: done after {0:N1} s, exit code {1}" -f $dur, $p.ExitCode)
