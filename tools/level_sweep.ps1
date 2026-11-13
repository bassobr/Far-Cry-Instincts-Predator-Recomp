# Level sweep: starts each level via FCP_START_LEVEL one after another (only ever
# one own predator.exe), with a simple key script (walk, look around, shoot), frame dumps, frame rate
# statistics and command trace, and evaluates the logs.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\level_sweep.ps1 [-Config release] [-Seconds 90]
#       [-Levels 00_training,21_dunea] [-DumpAt 750,1350,1950,2550] [-Env "FCP_FPS=60;..."] [-OutDir <path>]
#       [-NoDumps]
# Without -Levels: all 19 story levels of both campaigns (table in default.xex, 0x8205A3FC/0x8205C4CC).
# Output (gitignored): out\sweep\<time>\ with sweep.csv, sweep.md, per level <level>.log, commands/state CSV and
# dumps <level>_<frame>.png (for viewing; delete them individually afterwards). Exit code 0 = all levels loaded without crash.
param(
  [string]$Config = "release",
  [int]$Seconds = 90,
  [string[]]$Levels = @(),
  [string]$DumpAt = "750,1350,1950,2550",
  [string[]]$Env = @(),
  [string]$OutDir = "",
  [string]$KeyScript = "3-12:w,13-15:mx=600,16-18:mouse1,20-30:w,31:my=-120,33-38:mouse1+w,40-50:w+d,52-54:mx=-900,55-65:w,66-70:mouse1,72:my=120,73-85:w+a",
  [switch]$NoDumps
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$exe = Join-Path $bin "predator.exe"
if (-not (Test-Path $exe)) { Write-Output "level_sweep: missing $exe"; exit 2 }
if ($Levels.Count -eq 1 -and $Levels[0].Contains(",")) { $Levels = $Levels[0].Split(",") }
if ($Levels.Count -eq 0) {
  $Levels = @("00_training", "01_beach", "02_native", "03_river", "04_rain_forest", "05_mines", "06_cliffs",
    "07_sub-aquatic_facility", "10_dead_marshes", "11_wasteland", "13_black_forest",
    "21_dunea", "22_duneb", "23_dunec", "24_shantyday", "26_shantynight", "28_jungle", "29_forest", "30_mountain")
}
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ($OutDir -eq "") { $OutDir = Join-Path $root "out\sweep\$stamp" }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$logDir = Join-Path $bin "logs"
New-Item -ItemType Directory -Force $logDir | Out-Null

function Num($s) { return [double]::Parse($s, [Globalization.CultureInfo]::InvariantCulture) }
function Fmt($v) { if ($null -eq $v) { return "-" }; return ([double]$v).ToString("0.0", [Globalization.CultureInfo]::InvariantCulture) }

$rows = New-Object System.Collections.Generic.List[object]
foreach ($level in $Levels) {
  Write-Output "level_sweep: $level ..."
  Get-ChildItem $logDir -Filter "*.csv" -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
  $vars = @(
    "FCP_START_LEVEL=$level", "FCP_FPS_STATS=10", "FCP_TRACE_COMMANDS=1", "FCP_TRACE_STATE=30",
    "FCP_TRACE_HITCHES=250", "FCP_INPUT_SCRIPT_CLOCK=level", "FCP_KEY_SCRIPT=$KeyScript"
  )
  if (-not $NoDumps) { $vars += @("FCP_DUMP_FRAMES=sweep_%d.png", "FCP_DUMP_FRAME_AT=$DumpAt") }
  $vars += $Env
  $start = Get-Date
  $run = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run.ps1") `
    -Config $Config -Seconds $Seconds -Env ($vars -join ";")
  $exitCode = "?"
  foreach ($l in $run) { if ($l -match "exit code (\S+)") { $exitCode = $Matches[1] } }
  $killed = [bool]($run | Select-String "ending it")
  $log = Join-Path $bin "predator.log"
  $dst = Join-Path $OutDir "$level.log"
  Copy-Item -LiteralPath $log -Destination $dst -Force
  foreach ($csv in @("commands.csv", "state.csv")) {
    $p = Join-Path $logDir $csv
    if (Test-Path $p) { Move-Item -LiteralPath $p -Destination (Join-Path $OutDir "$level.$csv") -Force }
  }
  $dumps = @()
  foreach ($png in @(Get-ChildItem $logDir -Filter "sweep_*.png" -ErrorAction SilentlyContinue)) {
    $n = $png.Name -replace "^sweep_", "$level`_"
    Move-Item -LiteralPath $png.FullName -Destination (Join-Path $OutDir $n) -Force
    $dumps += $n
  }
  $text = Get-Content $dst

  # Loading: dispatch of StartLevel until level_loaded (command trace, wall time)
  $loadS = $null; $loaded = $false
  $cmdCsv = Join-Path $OutDir "$level.commands.csv"
  if (Test-Path $cmdCsv) {
    $c = Import-Csv $cmdCsv
    $disp = $c | Where-Object { $_.event -eq "dispatch" -and $_.command -like "StartLevel*" } | Select-Object -First 1
    $ld = $c | Where-Object { $_.event -eq "level_loaded" } | Select-Object -First 1
    if ($ld) { $loaded = $true; if ($disp) { $loadS = (Num $ld.wall_s) - (Num $disp.wall_s) } }
    $otherCmds = @($c | Where-Object { $_.event -eq "push" -and $_.command -notlike "StartLevel*" } | ForEach-Object { $_.command } | Select-Object -Unique)
  } else { $otherCmds = @() }
  if (-not $loaded -and ($text | Select-String "gametime: level '$([regex]::Escape($level))' started")) { $loaded = $true }

  # Frame rate: framestat lines in the level (from the second one; the first contains the loading)
  $fs = @($text | Select-String "framestat: target (\d+) \| \d+ frames in [\d.]+ s = ([\d.]+) fps \| ms mean [\d.]+ p50 [\d.]+ p99 ([\d.]+) max ([\d.]+) \| 1% low ([\d.]+) fps \| game/wall ([\d.]+) \| level (\S+)")
  $lv = @($fs | Where-Object { $_.Matches[0].Groups[7].Value -ne "-" })
  if ($lv.Count -gt 1) { $lv = $lv[1..($lv.Count - 1)] }
  $fpsVals = @($lv | ForEach-Object { Num $_.Matches[0].Groups[2].Value })
  $p99Vals = @($lv | ForEach-Object { Num $_.Matches[0].Groups[3].Value })
  $lowVals = @($lv | ForEach-Object { Num $_.Matches[0].Groups[5].Value })
  $gwVals = @($lv | ForEach-Object { Num $_.Matches[0].Groups[6].Value })
  $fpsMean = if ($fpsVals.Count) { ($fpsVals | Measure-Object -Average).Average } else { $null }
  $fpsMin = if ($fpsVals.Count) { ($fpsVals | Measure-Object -Minimum).Minimum } else { $null }
  $p99Max = if ($p99Vals.Count) { ($p99Vals | Measure-Object -Maximum).Maximum } else { $null }
  $lowMin = if ($lowVals.Count) { ($lowVals | Measure-Object -Minimum).Minimum } else { $null }
  $gwMin = if ($gwVals.Count) { ($gwVals | Measure-Object -Minimum).Minimum } else { $null }

  # Findings in the log
  $pat = [ordered]@{
    "fatal"     = "FATAL|crash report|unhandled title (read|write)|ACCESS_VIOLATION|EXCEPTION_|thread dump: watchdog";
    "badcall"   = "bad call|unresolved (indirect|function)|no function at";
    "shader"    = "renderer: Shader .* failed|unsupported interpolator|fcshaders.*error";
    "nvrhi"     = "\[nvrhi (error|warn)\]|VUID-|Validation Error|D3D12 ERROR";
    "format"    = "(?i)unknown format|unsupported format|not supported";
    "gpuerr"    = " E \[gpu\]";
    "nomount"   = "no mount covers";
    "hitch"     = "^\s*[\d.]+\s+\d+ hitch:";
  }
  $counts = [ordered]@{}
  $firsts = [ordered]@{}
  foreach ($k in $pat.Keys) {
    $h = @($text | Select-String -CaseSensitive -Pattern $pat[$k])
    $counts[$k] = $h.Count
    if ($h.Count -gt 0) { $t = $h[0].Line.Trim(); if ($t.Length -gt 180) { $t = $t.Substring(0, 180) }; $firsts[$k] = $t }
  }
  # skipped draws (renderer statistics "(UP n, skipped n)")
  $skipped = 0
  foreach ($m in ($text | Select-String "skipped (\d+)\)")) { $skipped = [Math]::Max($skipped, [int]$m.Matches[0].Groups[1].Value) }
  # Audio: last underrun count
  $underruns = "-"
  $ur = $text | Select-String "fcp audio: .* underruns=(\d+)" | Select-Object -Last 1
  if ($ur) { $underruns = $ur.Matches[0].Groups[1].Value }
  # Player moved? (state trace)
  $moved = "-"
  $stCsv = Join-Path $OutDir "$level.state.csv"
  if (Test-Path $stCsv) {
    $st = @(Import-Csv $stCsv)
    if ($st.Count -gt 1) {
      $a = $st[0]; $b = $st[$st.Count - 1]
      $d = [Math]::Sqrt([Math]::Pow((Num $b.x) - (Num $a.x), 2) + [Math]::Pow((Num $b.y) - (Num $a.y), 2))
      $moved = "{0:N0} m, hp {1}" -f $d, $b.health
    }
  }
  $levelTime = $null
  $lt = $text | Select-String "clock: .* level \S+ ([\d.]+) s" | Select-Object -Last 1
  if ($lt) { $levelTime = Num $lt.Matches[0].Groups[1].Value }
  $crash = ($exitCode -ne "0") -or ($counts["fatal"] -gt 0) -or $killed
  $row = [pscustomobject]@{
    level = $level; loaded = $loaded; load_s = (Fmt $loadS); level_time_s = (Fmt $levelTime); exit = $exitCode;
    crash = $crash; fps_mean = (Fmt $fpsMean); fps_min10s = (Fmt $fpsMin); p99_ms_max = (Fmt $p99Max);
    low1_min = (Fmt $lowMin); game_wall_min = $(if ($null -ne $gwMin) { $gwMin.ToString("0.000", [Globalization.CultureInfo]::InvariantCulture) } else { "-" });
    hitches = $counts["hitch"]; fatal = $counts["fatal"]; badcall = $counts["badcall"]; shader = $counts["shader"];
    nvrhi = $counts["nvrhi"]; format = $counts["format"]; gpu_err = $counts["gpuerr"]; no_mount = $counts["nomount"];
    skipped_draws_max = $skipped; underruns = $underruns; moved = $moved;
    other_cmds = ($otherCmds -join " | "); dumps = ($dumps -join " ");
    first_findings = (($firsts.Keys | Where-Object { $_ -ne "hitch" -and $_ -ne "nomount" } | ForEach-Object { "$_=" + $firsts[$_] }) -join " || ")
  }
  $rows.Add($row)
  Write-Output ("level_sweep: {0}: loaded {1} in {2} s, exit {3}, fps {4} (min {5}), p99 {6} ms, findings fatal {7} shader {8} nvrhi {9} gpu {10}, dumps {11}" -f
    $level, $loaded, $row.load_s, $exitCode, $row.fps_mean, $row.fps_min10s, $row.p99_ms_max, $counts["fatal"], $counts["shader"], $counts["nvrhi"], $counts["gpuerr"], $dumps.Count)
  $rows | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $OutDir "sweep.csv")
}

$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Level sweep $stamp ($Config, $Seconds s per level)")
$md.Add("")
$md.Add("| Level | loaded | load s | Exit | fps | min 10 s | p99 ms | 1% low | game/wall | Hitches | Shader | NVRHI | GPU-E | draws skipped | underruns | movement | other commands |")
$md.Add("|---|---|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|")
foreach ($r in $rows) {
  $md.Add(("| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16} |" -f
    $r.level, $(if ($r.loaded) { "yes" } else { "NO" }), $r.load_s, $(if ($r.crash) { "**$($r.exit)**" } else { $r.exit }),
    $r.fps_mean, $r.fps_min10s, $r.p99_ms_max, $r.low1_min, $r.game_wall_min, $r.hitches, $r.shader, $r.nvrhi, $r.gpu_err,
    $r.skipped_draws_max, $r.underruns, $r.moved, $r.other_cmds))
}
$md | Set-Content -Encoding utf8 (Join-Path $OutDir "sweep.md")
$md | ForEach-Object { Write-Output $_ }
Write-Output "level_sweep: result $OutDir"
$bad = @($rows | Where-Object { -not $_.loaded -or $_.crash }).Count
if ($bad -eq 0) { exit 0 } else { exit 1 }
