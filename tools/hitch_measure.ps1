# Hitch measurement (frames over a threshold) with a cold or warm user folder, Steam Deck approximation by default.
# Starts predator.exe -Runs times in the level (developer start) with FCP_TRACE_HITCHES=<Threshold>, FCP_FPS_STATS=5,
# FCP_CPU_STATS=5 and its own FCP_USER_ROOT (shader cache, pipeline list, Vulkan driver cache = the player's caches),
# then summarises each run:
#   - main-thread hitches in the level ("hitch: frame ..."): count, per minute, max, how many in the first 10 s
#   - renderer slow frames ("renderer: slow frame ..."), each assigned to its largest share:
#     pipe (pipeline creation in the draw path), tex (texture conversion/upload), buf (vertex/index buffer upload),
#     draw (rest of the draw path), gpu (waiting for the GPU / frame lead), out (outside the renderer: game code on
#     the render thread, waiting for the main thread)
#   - frames/s, largest p99, smallest 1 % low (5-s windows from -From level seconds), render thread ms per frame
#   - foreign game processes before/after (predator.exe, FarCryInstincts*.exe of other folders)
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\hitch_measure.ps1 -Tag cold -Fresh
#       [-Level 21_dunea] [-Fps 60] [-Seconds 60] [-Runs 2] [-Config release] [-Affinity FF] [-Threshold 25]
#       [-UserRoot out\hitch\root] [-Extra "A=1;B=2"] [-From 10] [-Bin <bin folder of another build>] [-ParseOnly <log>,...]
# -Fresh deletes the user folder before EVERY run (cold: first visit as for a new player); without it the folder is
# kept (warm: second start). Only the process started here is touched or ended (by PID).
# Logs: out\logs\hitch\<Tag>_<n>.log (gitignored).
param(
  [string]$Tag = "run",
  [string]$Level = "21_dunea",
  [string]$Fps = "60",
  [int]$Seconds = 60,
  [int]$Runs = 2,
  [string]$Config = "release",
  [string]$Bin = "",
  [string]$Extra = "",
  [string]$Affinity = "FF",
  [int]$Threshold = 25,
  [string]$UserRoot = "",
  [switch]$Fresh,
  [double]$From = 10,
  [string[]]$ParseOnly = @()
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = if ($Bin) { if ([IO.Path]::IsPathRooted($Bin)) { $Bin } else { Join-Path $root $Bin } } else { Join-Path $root "out\build\$Config\bin" }
$exe = Join-Path $bin "predator.exe"
$outDir = Join-Path $root "out\logs\hitch"
New-Item -ItemType Directory -Force $outDir | Out-Null
if (-not $UserRoot) { $UserRoot = Join-Path $root "out\hitch\root_$Tag" }
elseif (-not [IO.Path]::IsPathRooted($UserRoot)) { $UserRoot = Join-Path $root $UserRoot }
$inv = [Globalization.CultureInfo]::InvariantCulture

function Foreign() {
  @(Get-Process predator, FarCryInstincts* -ErrorAction SilentlyContinue | Where-Object { -not $_.Path -or -not $_.Path.StartsWith($bin) }).Count
}
function D([string]$s) { [double]::Parse($s, $inv) }
function Avg($a) { if ($a.Count) { ($a | Measure-Object -Average).Average } else { 0 } }

function Summarize([string]$log, [string]$label, $before, $after) {
  $lines = Get-Content $log
  $fps = @(); $p99 = @(); $low = @(); $render = @(); $take = $false
  $hitches = 0; $hitchMax = 0.0; $early = 0; $levelEnd = 0.0; $levelStart = $false
  $cat = [ordered]@{ pipe = 0; tex = 0; buf = 0; draw = 0; gpu = 0; out = 0 }
  $slowMs = [ordered]@{ pipe = 0.0; tex = 0.0; buf = 0.0; draw = 0.0; gpu = 0.0; out = 0.0 }
  $slow = 0; $pipesNew = 0; $inLevel = $false
  # A main-thread hitch is assigned to the renderer slow frame logged within 40 ms before it (the render thread ends
  # its frame with the swap; the main thread notices the long frame at its next frame start); none: "game".
  $lastSlow = $null; $lastSlowT = -1.0; $game = 0
  foreach ($l in $lines) {
    $t = -1.0
    if ($l -match '^\s+([\d.]+)\s') { $t = D $matches[1] }
    if ($l -match 'gametime: level .* started at frame') { $inLevel = $true }
    if ($l -match 'renderer: slow frame \d+: ([\d.]+) ms, draws ([\d.]+) ms \(of which pipelines (\d+) / ([\d.]+) ms, textures \d+ / ([\d.]+) ms, buffers ([\d.]+) ms / \d+ KB\), GPU wait ([\d.]+) ms') {
      $wall = D $matches[1]; $dr = D $matches[2]; $pp = D $matches[4]; $tx = D $matches[5]; $bf = D $matches[6]; $gw = D $matches[7]
      # With FCP_RENDER_PIPELINE_ASYNC the pipeline wait lies at the end of the command list (Flush), outside the draws.
      $lastSlow = [ordered]@{ pipe = $pp; tex = $tx; buf = $bf; draw = [Math]::Max(0, $dr - $pp - $tx - $bf); gpu = $gw; out = [Math]::Max(0, $wall - $dr - $gw - [Math]::Max(0, $pp - $dr)); n = [int]$matches[3] }
      $lastSlowT = $t
    }
    if ($l -match 'hitch: frame \d+ took ([\d.]+) ms .*level ([\d.]+) s\)') {
      $lv = D $matches[2]; $ms = D $matches[1]
      if ($inLevel) {
        $hitches++; $hitchMax = [Math]::Max($hitchMax, $ms)
        if ($lv -lt 10) { $early++ }
        if ($lastSlow -and $t - $lastSlowT -le 0.040) {
          $parts = $lastSlow
          $best = ($parts.GetEnumerator() | Where-Object { $_.Key -ne "n" } | Sort-Object Value -Descending | Select-Object -First 1).Key
          $cat[$best]++; $slow++; $pipesNew += $parts.n
          foreach ($k in @($slowMs.Keys)) { $slowMs[$k] += $parts[$k] }
          $lastSlow = $null
        } else { $game++ }
      }
    }
    if ($l -match 'cpu: window [\d.]+ s \| process [\d.]+% .*\| level (\S+) ([\d.]+) s') {
      $take = $matches[1] -ne "-" -and (D $matches[2]) -ge $From
    } elseif ($take -and $l -match 'cpu:\s+([\d.]+)% \(user.*\) (.+) \[\d+\]$') {
      $pct = D $matches[1]
      if ($matches[2] -match 'guest 82314A18') { $render += $pct }
    }
    if ($l -match 'framestat: target \S+ \| \d+ frames in [\d.]+ s = ([\d.]+) fps \| ms mean [\d.]+ p50 [\d.]+ p99 ([\d.]+) max [\d.]+ \| 1% low ([\d.]+) fps \| game/wall ([\d.]+) \| level (\S+) ([\d.]+) s') {
      $lv = D $matches[6]
      if ($matches[5] -ne "-") { $levelEnd = [Math]::Max($levelEnd, $lv) }
      if ($matches[5] -ne "-" -and $lv -ge $From) { $fps += D $matches[1]; $p99 += D $matches[2]; $low += D $matches[3] }
    }
  }
  $f = Avg $fps
  $pm = if ($p99.Count) { ($p99 | Measure-Object -Maximum).Maximum } else { 0 }
  $lm = if ($low.Count) { ($low | Measure-Object -Minimum).Minimum } else { 0 }
  $rm = Avg $render
  $renderMs = if ($f -gt 0) { 10.0 * $rm / $f } else { 0 }
  $perMin = if ($levelEnd -gt 0) { $hitches * 60.0 / $levelEnd } else { 0 }
  $cats = ($cat.GetEnumerator() | ForEach-Object { "{0} {1}" -f $_.Key, $_.Value }) -join ", "
  $ms = ($slowMs.GetEnumerator() | ForEach-Object { "{0} {1:N0}" -f $_.Key, $_.Value }) -join ", "
  Write-Output ("{0}: hitches >{1} ms {2} ({3:N1}/min over {4:N0} s, {5} in the first 10 s, max {6:N1} ms) | in renderer frames {7}: {8}, game {18} | ms: {9} | new pipelines in them {10} | {11:N2} fps, p99 max {12:N1} ms, 1% low min {13:N1} | render {14:N1}% ({15:N2} ms/frame) | foreign {16}/{17}" -f `
      $label, $Threshold, $hitches, $perMin, $levelEnd, $early, $hitchMax, $slow, $cats, $ms, $pipesNew, $f, $pm, $lm, $rm, $renderMs, $before, $after, $game)
}

function RunAll() {
  $gameIso = Get-FcpGameIso
  for ($n = 1; $n -le $Runs; $n++) {
    if ($Fresh -and (Test-Path $UserRoot)) { Remove-Item -Recurse -Force $UserRoot }
    New-Item -ItemType Directory -Force $UserRoot | Out-Null
    $vars = @{
      "FCP_GAME_ISO" = $gameIso; "FCP_USER_ROOT" = $UserRoot
      "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$Seconds"
      "REX_UI_WINDOW_WIDTH" = "640"; "REX_UI_WINDOW_HEIGHT" = "360"
      "FCP_CPU_STATS" = "5"; "FCP_FPS_STATS" = "5"; "FCP_FPS" = $Fps; "FCP_TRACE_HITCHES" = "$Threshold"
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
      try { $p.ProcessorAffinity = [IntPtr][Convert]::ToInt64($Affinity, 16) } catch { Write-Output "hitch: affinity not set: $_" }
    }
    if (-not $p.WaitForExit(($Seconds + 60) * 1000)) {
      Write-Output "hitch: PID $($p.Id) hangs, ending it (this process only)"
      Stop-Process -Id $p.Id -Force
      $p.WaitForExit(10000) | Out-Null
    }
    $after = Foreign
    foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
    $src = Join-Path $UserRoot "predator.log"
    if (-not (Test-Path $src)) { $src = Join-Path $bin "predator.log" }
    $log = Join-Path $outDir ("{0}_{1}.log" -f $Tag, $n)
    Copy-Item $src $log -Force
    Summarize $log ("{0} #{1}" -f $Tag, $n) $before $after
  }
}

if ($ParseOnly.Count) { foreach ($l in $ParseOnly) { Summarize $l ([IO.Path]::GetFileNameWithoutExtension($l)) '-' '-' } } else { RunAll }
