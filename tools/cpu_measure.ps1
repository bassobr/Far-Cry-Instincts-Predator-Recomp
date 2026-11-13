# CPU measurement: starts predator.exe N times via tools\boot\run.ps1 with
# FCP_CPU_STATS and FCP_FPS_STATS, notes foreign predator.exe before and after each run and summarizes:
# process CPU (sum of all cores, 100 % = one core), CPU per thread, frames/s, largest p99.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\cpu_measure.ps1 -Tag menu30 -Seconds 60
#       [-Level 21_dunea] [-Extra "FCP_FPS=120"] [-Runs 2] [-Config release] [-From 40]
# Menu: windows from -From seconds of wall time (default 40). Level: windows from 15 s of level time.
# Logs: out\logs\cpu\<Tag>_<n>.log (gitignored).
param(
  [string]$Tag = "run",
  [int]$Seconds = 60,
  [string]$Level = "",
  [string]$Extra = "",
  [int]$Runs = 2,
  [string]$Config = "release",
  [double]$From = 40
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$outDir = Join-Path $root "out\logs\cpu"
New-Item -ItemType Directory -Force $outDir | Out-Null
$inv = [Globalization.CultureInfo]::InvariantCulture

function Foreign() {
  @(Get-Process predator -ErrorAction SilentlyContinue | Where-Object { $_.Path -and -not $_.Path.StartsWith($bin) }).Count
}

for ($n = 1; $n -le $Runs; $n++) {
  $vars = "FCP_CPU_STATS=5;FCP_FPS_STATS=5"
  if ($Level) { $vars += ";FCP_START_LEVEL=$Level;FCP_INPUT_SCRIPT_CLOCK=level;FCP_INPUT_SCRIPT=10-50:lup" }
  if ($Extra) { $vars += ";$Extra" }
  $before = Foreign
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run.ps1") -Config $Config `
    -Seconds $Seconds -Env $vars | Out-Null
  $after = Foreign
  $log = Join-Path $outDir ("{0}_{1}.log" -f $Tag, $n)
  Copy-Item (Join-Path $bin "predator.log") $log -Force
  $lines = Get-Content $log
  $proc = @(); $threads = @{}; $fps = @(); $p99 = @(); $gw = @(); $take = $false
  foreach ($l in $lines) {
    if ($l -match '^\s*([\d.]+)\s+\d+ cpu: window [\d.]+ s \| process ([\d.]+)% .*\| level (\S+) ([\d.]+) s') {
      $wall = [double]::Parse($matches[1], $inv); $lv = $matches[3]; $ls = [double]::Parse($matches[4], $inv)
      $take = if ($Level) { $lv -ne "-" -and $ls -ge 15 } else { $wall -ge $From }
      if ($take) { $proc += [double]::Parse($matches[2], $inv) }
    } elseif ($take -and $l -match 'cpu:\s+([\d.]+)% \(user.*\) (.+) \[\d+\]$') {
      $name = $matches[2] -replace '^XThread[0-9A-F]+ \([0-9A-F]+\) ', '' -replace '\s+$', ''
      if (-not $threads.ContainsKey($name)) { $threads[$name] = @() }
      $threads[$name] += [double]::Parse($matches[1], $inv)
    }
    if ($l -match 'framestat: target \S+ \| \d+ frames in [\d.]+ s = ([\d.]+) fps \| ms mean [\d.]+ p50 [\d.]+ p99 ([\d.]+) .*game/wall ([\d.]+) \| level (\S+) ([\d.]+) s') {
      $wall = [double]::Parse(($l.Trim() -split '\s+')[0], $inv)
      $ok = if ($Level) { $matches[4] -ne "-" -and [double]::Parse($matches[5], $inv) -ge 15 } else { $wall -ge $From }
      if ($ok) { $fps += [double]::Parse($matches[1], $inv); $p99 += [double]::Parse($matches[2], $inv); $gw += $matches[3] }
    }
  }
  $avg = if ($proc.Count) { ($proc | Measure-Object -Average).Average } else { 0 }
  $f = if ($fps.Count) { ($fps | Measure-Object -Average).Average } else { 0 }
  $pm = if ($p99.Count) { ($p99 | Measure-Object -Maximum).Maximum } else { 0 }
  $bindLine = $lines | Select-String -Pattern "NetDll_bind -> r3=(\w+)" | Select-Object -First 1
  $bind = if (-not $bindLine) { "-" } elseif ($bindLine.Matches[0].Groups[1].Value -eq "00000000") { "ok" } else { "failed" }
  $audio = ($lines | Select-String -Pattern "fcp audio: .*\| total" | Select-Object -Last 1)
  Write-Output ("{0} #{1}: process {2:N1}% ({3} windows) | {4:N2} fps, p99 max {5:N1} ms | game/wall {6} | foreign predator before/after {7}/{8} | UDP-1000 bind {9}" -f `
      $Tag, $n, $avg, $proc.Count, $f, $pm, (($gw | Select-Object -Unique) -join ","), $before, $after, $bind)
  $top = $threads.GetEnumerator() | ForEach-Object { [pscustomobject]@{ Name = $_.Key; Avg = (($_.Value | Measure-Object -Sum).Sum / [Math]::Max(1, $proc.Count)) } } |
    Sort-Object Avg -Descending | Select-Object -First 8
  foreach ($t in $top) { Write-Output ("    {0,6:N1}%  {1}" -f $t.Avg, $t.Name) }
  if ($audio) { Write-Output ("    audio: " + $audio.Line.Trim()) }
}
