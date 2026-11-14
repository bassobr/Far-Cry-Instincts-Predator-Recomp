# First-start build of the game code per compiler variant on the PC, then the main thread in the game with that DLL
# (docs/phase1/deck-erststart-rumble.md). Per variant a fresh portable folder out\build\bv_<name>\bin (copy of the
# release folder), the first start under the PC measurement lock (tools/measure_lock.ps1) with the variant's
# switches, then tools\perf_measure.ps1 in the same lock window.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_variants.ps1 [-Dist out\dist\<release>]
#       [-Variants "base=;cap0=FCP_GAME_CODE_CAPTURE_USES=0;..."] [-Cpus 8] [-Runs 2] [-Affinity 0xFF] [-Fps 120]
# -Cpus limits the build to the first n logical processors (FCP_GAME_CODE_CPUS, 8 = like the Steam Deck).
# Only processes started here are waited for or ended (by PID).
param(
  [string]$Dist = "",
  [string]$Variants = "base=;cap0=FCP_GAME_CODE_CAPTURE_USES=0;lto0=FCP_GAME_CODE_LTO=0;cap10k=FCP_GAME_CODE_CAPTURE_USES=10000;old=FCP_GAME_CODE_CAPTURE_USES=0,FCP_GAME_CODE_LTO=0",
  [int]$Cpus = 8,
  [int]$Runs = 3,
  [string]$Affinity = "0xFF",
  [string]$Fps = "120",
  [string]$Level = "21_dunea"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
. "$PSScriptRoot\measure_lock.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not $Dist) { $Dist = (Get-ChildItem (Join-Path $root "out\dist") -Directory | Where-Object { $_.Name -like "*-win64" } | Select-Object -First 1).FullName }
$iso = Get-FcpGameIso
$log = Join-Path $root "out\logs\build_variants.txt"
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
foreach ($item in $Variants.Split(";")) {
  $i = $item.IndexOf("=")
  $name = $item.Substring(0, $i)
  $vars = @{}
  foreach ($kv in $item.Substring($i + 1).Split(",")) {
    $j = $kv.IndexOf("=")
    if ($j -gt 0) { $vars[$kv.Substring(0, $j)] = $kv.Substring($j + 1) }
  }
  $bin = Join-Path $root "out\build\bv_$name\bin"
  if (Test-Path $bin) { cmd /c rmdir /s /q "$bin" | Out-Null }
  New-Item -ItemType Directory -Force $bin | Out-Null
  robocopy $Dist $bin /E /NFL /NDL /NJH /NJS /NP | Out-Null
  Set-Content -Path (Join-Path $bin "portable.txt") -Value "" -Encoding ascii
  TakeLock "build variant $name"
  try {
    $vv = @{ "FCP_GAME_ISO" = $iso; "FCP_NO_DIALOGS" = "1"; "FCP_EXIT_AFTER" = "3"; "FCP_GAME_CODE_CPUS" = "$Cpus";
              "FCP_GAME_CODE_CAPTURE_USES" = $null; "FCP_GAME_CODE_LTO" = $null; "FCP_MUTE" = "1" }
    foreach ($k in $vars.Keys) { $vv[$k] = $vars[$k] }
    foreach ($k in $vv.Keys) { [Environment]::SetEnvironmentVariable($k, $vv[$k], "Process") }
    $p = Start-Process -FilePath (Join-Path $bin "predator.exe") -WorkingDirectory $bin -PassThru
    if (-not $p.WaitForExit(1800 * 1000)) { Write-Output "bv: PID $($p.Id) hangs, ending it"; Stop-Process -Id $p.Id -Force }
    foreach ($k in @("FCP_GAME_CODE_CAPTURE_USES", "FCP_GAME_CODE_LTO", "FCP_EXIT_AFTER", "FCP_GAME_CODE_CPUS")) {
      [Environment]::SetEnvironmentVariable($k, $null, "Process")
    }
    $line = Select-String (Join-Path $bin "predator.log") -Pattern "first-start build in" | Select-Object -First 1
    "$(Get-Date -Format T) $name ($($item.Substring($i + 1))): $($line.Line)" | Tee-Object -FilePath $log -Append
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "perf_measure.ps1") -Tag "bv_$name" `
        -Config "bv_$name" -Level $Level -Fps $Fps -Runs $Runs -Affinity $Affinity | Tee-Object -FilePath $log -Append
  } finally {
    DropLock
  }
}

