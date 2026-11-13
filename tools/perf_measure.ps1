# Main-thread performance measurement: starts predator.exe -Runs times with FCP_CPU_STATS=5 and FCP_FPS_STATS=5
# in the level and summarises each run: frames/s, largest p99, smallest 1 % low, game time/wall time, CPU per frame
# (process), CPU of the main and render threads, load time (developer start up to "gametime: level ... started",
# wall time of the process), audio underruns, foreign predator.exe before/after.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf_measure.ps1 -Tag before -Extra "FCP_GUEST_READABLE_VQ=1"
#       [-Level 21_dunea] [-Fps 120] [-Seconds 60] [-Runs 2] [-Config release] [-Affinity 0xFF] [-From 15]
# -Affinity sets the processor affinity of its own process (by PID) right after the start (Steam Deck
# approximation: 0xFF = 4 cores / 8 threads on CCD0). Only the process started here is touched or ended.
# FCP_GAME_ISO = the shared disc image (tools\shared_root.ps1; an FCP_GAME_ISO already set wins).
# Logs: out\logs\perf\<Tag>_<n>.log (gitignored).
param(
  [string]$Tag = "run",
  [string]$Level = "21_dunea",
  [string]$Fps = "120",
  [int]$Seconds = 60,
  [int]$Runs = 2,
  [string]$Config = "release",
  [string]$Extra = "",
  [string]$Affinity = "",
  [double]$From = 15,
  [string[]]$ParseOnly = @()
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$gameIso = Get-FcpGameIso
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$exe = Join-Path $bin "predator.exe"
$outDir = Join-Path $root "out\logs\perf"
New-Item -ItemType Directory -Force $outDir | Out-Null
$inv = [Globalization.CultureInfo]::InvariantCulture

function Foreign() {
  # foreign game processes (also the other Far Cry Instincts ports: FarCryInstincts*.exe), not our own folder
  @(Get-Process predator, FarCryInstincts* -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or -not $_.Path.StartsWith($bin) }).Count
}
function D([string]$s) { [double]::Parse($s, $inv) }
function Avg($a) { if ($a.Count) { ($a | Measure-Object -Average).Average } else { 0 } }

function RunAll() {
for ($n = 1; $n -le $Runs; $n++) {
  $vars = @{
    "FCP_GAME_ISO" = $gameIso
    "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$Seconds"
    "REX_UI_WINDOW_WIDTH" = "640"; "REX_UI_WINDOW_HEIGHT" = "360"
    "FCP_CPU_STATS" = "5"; "FCP_FPS_STATS" = "5"; "FCP_FPS" = $Fps
    "FCP_START_LEVEL" = $Level; "FCP_INPUT_SCRIPT_CLOCK" = "level"; "FCP_INPUT_SCRIPT" = "10-50:lup"
  }
  foreach ($kv in $Extra.Split(";")) {
    $i = $kv.IndexOf("=")
    if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
  }
  foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
  $before = Foreign
  $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
  if ($Affinity) {
    try { $p.ProcessorAffinity = [IntPtr][Convert]::ToInt64($Affinity, 16) } catch { Write-Output "perf: affinity not set: $_" }
  }
  if (-not $p.WaitForExit(($Seconds + 45) * 1000)) {
    Write-Output "perf: PID $($p.Id) hangs, ending it (this process only)"
    Stop-Process -Id $p.Id -Force
    $p.WaitForExit(10000) | Out-Null
  }
  $after = Foreign
  foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
  $log = Join-Path $outDir ("{0}_{1}.log" -f $Tag, $n)
  Copy-Item (Join-Path $bin "predator.log") $log -Force
  Summarize $log ("{0} #{1}" -f $Tag, $n) $before $after
}
}

function Summarize([string]$log, [string]$label, $before, $after) {
  $lines = Get-Content $log
  $fps = @(); $p99 = @(); $low = @(); $gw = @(); $main = @(); $render = @(); $take = $false
  $load = 0; $cpf = 0; $under = "-"
  foreach ($l in $lines) {
    if ($l -match 'gametime: level .* started at frame \d+, wall ([\d.]+) s') { if (-not $load) { $load = D $matches[1] } }
    if ($l -match 'cpu: window [\d.]+ s \| process [\d.]+% .*\| level (\S+) ([\d.]+) s') {
      $take = $matches[1] -ne "-" -and (D $matches[2]) -ge $From
    } elseif ($take -and $l -match 'cpu:\s+([\d.]+)% \(user.*\) (.+) \[\d+\]$') {
      $pct = D $matches[1]; $who = $matches[2]
      if ($who -match 'guest 826444F0') { $main += $pct }
      if ($who -match 'guest 82314A18') { $render += $pct }
    }
    if ($l -match 'framestat: target \S+ \| \d+ frames in [\d.]+ s = ([\d.]+) fps \| ms mean [\d.]+ p50 [\d.]+ p99 ([\d.]+) max [\d.]+ \| 1% low ([\d.]+) fps \| game/wall ([\d.]+) \| level (\S+) ([\d.]+) s') {
      if ($matches[5] -ne "-" -and (D $matches[6]) -ge $From) {
        $fps += D $matches[1]; $p99 += D $matches[2]; $low += D $matches[3]; $gw += $matches[4]
      }
    }
    if ($l -match 'cpu total level: .* \| ([\d.]+) ms CPU per frame') { $cpf = D $matches[1] }
    if ($l -match 'fcp audio: .*\| total underruns=(\d+)') { $under = $matches[1] }
  }
  $f = Avg $fps
  $pm = if ($p99.Count) { ($p99 | Measure-Object -Maximum).Maximum } else { 0 }
  $lm = if ($low.Count) { ($low | Measure-Object -Minimum).Minimum } else { 0 }
  $mm = Avg $main; $rm = Avg $render
  $mainMs = if ($f -gt 0) { 10.0 * $mm / $f } else { 0 }
  Write-Output ("{0}: {1:N2} fps ({2} windows), p99 max {3:N1} ms, 1% low min {4:N1} fps, game/wall {5} | CPU/frame {6:N2} ms | main {7:N1}% ({8:N2} ms/frame), render {9:N1}% | load {10:N2} s | underruns {11} | foreign {12}/{13}" -f `
      $label, $f, $fps.Count, $pm, $lm, (($gw | Select-Object -Unique) -join ","), $cpf, $mm, $mainMs, $rm, $load, $under, $before, $after)
}

if ($ParseOnly.Count) { foreach ($l in $ParseOnly) { Summarize $l ([IO.Path]::GetFileNameWithoutExtension($l)) '-' '-' } } else { RunAll }
