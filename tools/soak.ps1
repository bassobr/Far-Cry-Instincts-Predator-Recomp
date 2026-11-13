# Soak test: about 50 minutes of play over both campaigns, as several process chains in one user folder, with
# per-process sampling of memory, handles and threads every -SampleSeconds and an evaluation of all logs.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\soak.ps1 [-Config release] [-Dwell 160]
#       [-LongSeconds 960] [-SampleSeconds 10] [-Quick] [-Only 1,2,3,4] [-Analyze <out\soak\<time>>]
# Segments (one user folder out\soak\<time>\user for all, FCP_USER_ROOT; frame rate from predator.toml):
#   1 instincts: menu -> "Create New Game" (FCP_KEYBOARD_TEXT) -> 00_training -> 01_beach -> 02_native -> 03_river
#     (teleport into the EndMapTrigger after -Dwell s of level time, real level change by relaunch).
#     Gen 0 at 30 fps; the F10 menu sets fps 120 in every generation, so gen 1.. run at 120.
#   2 long: new process, menu -> "Resume Game" (loads the save of segment 1), -LongSeconds in that one level at 120,
#     F10 menu, Alt+Enter twice, pause.
#   3 evolution: development start 21_dunea -> 22_duneb -> 23_dunec -> 24_shantyday, gen 0 at 60, gen 1.. at 30.
#   4 menu: 21_dunea, F10 menu stress, Alt+Enter, "ReturnToMenu", main menu, clean exit.
# Gameplay in every generation through FCP_KEY_SCRIPT (wall clock): walk, look, fire, jump, strafe, use (vehicles),
# reload, pause/unpause. Changes the game only by the teleports and test commands (like level_transitions.ps1).
# Output (gitignored): out\soak\<time>\ with seg<n>\g<gen>.log, CSVs, samples.csv, soak.md.
# Exit code 0 = no findings, 1 = findings, 2 = not evaluable.
param(
  [string]$Config = "release",
  [int]$Dwell = 160,
  [int]$LongSeconds = 960,
  [int]$SampleSeconds = 10,
  [switch]$Quick,
  [int[]]$Only = @(1, 2, 3, 4),
  [string]$Analyze = ""
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$exe = Join-Path $bin "predator.exe"
$inv = [Globalization.CultureInfo]::InvariantCulture
if ($Quick) { $Dwell = 40; $LongSeconds = 150 }

# EndMapTrigger positions (from tools\level_transitions.ps1, disc mission XML).
$trig = @{
  "00_training" = "424.038,199.272,34.11"; "01_beach" = "160.554,105.442,20.04"; "02_native" = "413.353,96.2241,37.81"
  "03_river" = "988.213,827.204,35.23"; "21_dunea" = "846.201,459.571,116.45"; "22_duneb" = "18.1841,780.646,61.50"
  "23_dunec" = "951.25,662.25,36.80"; "24_shantyday" = "458.318,199.278,179.09"
}
function Teleports([string[]]$levels, [double]$at) {
  $list = foreach ($l in $levels) { "$l@$($at.ToString($inv)):$($trig[$l])"; "$l@$(($at + 15).ToString($inv)):$($trig[$l])" }
  return ($list -join "|")
}
# Gameplay cycles of 20 s from $from to $to (wall seconds since the title launch). A cycle that holds an F10 menu
# opening ($menuAt, open..close = 6 s) only presses Escape 2 s after the close: the closed F10 menu leaves the game in
# its own pause menu, as the console guide does (measured). Every 6th other cycle is a pause test: Escape (pause menu),
# 4 s later Escape (resume).
function Gameplay([int]$from, [int]$to, [int[]]$menuAt = @()) {
  $e = New-Object System.Collections.Generic.List[string]
  $k = 0
  for ($t = $from; $t + 20 -le $to; $t += 20) {
    $m = @($menuAt | Where-Object { $_ -ge $t - 8 -and $_ -lt $t + 12 })
    if ($m.Count) { $e.Add("$($m[0] + 8):escape"); continue }
    if ($k % 6 -eq 5) { $e.Add("$($t + 2):escape"); $e.Add("$($t + 6):escape"); $e.Add("$($t + 8)-$($t + 12):w"); $k++; continue }
    $e.Add("$t-$($t + 4):w"); $e.Add("$($t + 4)-$($t + 6):mx=900"); $e.Add("$($t + 6)-$($t + 7.5):mouse1")
    $e.Add("$($t + 8):space"); $e.Add("$($t + 9)-$($t + 11):a"); $e.Add("$($t + 11)-$($t + 13):mx=-700")
    $e.Add("$($t + 13)-$($t + 13.5):my=120"); $e.Add("$($t + 14):e"); $e.Add("$($t + 15)-$($t + 16.5):w+mouse1")
    $e.Add("$($t + 17)-$($t + 18):s"); $e.Add("$($t + 18.5):r")
    $k++
  }
  return ($e -join ",")
}
function Menu-Script([int[]]$at, [string]$extra) {
  $m = foreach ($t in $at) { "$($t):open"; "$($t + 2):tab=Graphics"; "$($t + 4):tab=Display"; "$($t + 6):close" }
  $s = ($m -join ",")
  if ($extra) { $s += ",$extra" }
  return $s
}
function Set-TomlFps([string]$user, [int]$fps) {
  $f = Join-Path $user "predator.toml"
  if (-not (Test-Path $f)) { Set-Content -Encoding ascii $f "[display]`nfps = $fps"; return "new file" }
  $t = [IO.File]::ReadAllText($f)
  if ($t -match "(?m)^fps\s*=\s*(\d+)") {
    $was = $Matches[1]
    $t = [regex]::Replace($t, "(?m)^fps\s*=\s*\d+", "fps = $fps")
    [IO.File]::WriteAllText($f, $t)
    return "was $was"
  }
  return "no fps line"
}

# ---- one chain with sampling ----
function Invoke-Segment([int]$seg, [string]$name, [hashtable]$vars, [int]$seconds, [int]$hard, [string]$out, [string]$user, $csv) {
  $base = @{
    "FCP_GAME_ISO" = (Get-FcpGameIso); "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$seconds"
    "REX_UI_WINDOW_WIDTH" = "640"; "REX_UI_WINDOW_HEIGHT" = "360"; "FCP_USER_ROOT" = $user
    "FCP_FPS_STATS" = "10"; "FCP_TRACE_HITCHES" = "250"; "FCP_TRACE_COMMANDS" = "1"; "FCP_TEST_ALT_ENTER_DIRECT" = "1"
  }
  foreach ($k in $vars.Keys) { $base[$k] = $vars[$k] }
  foreach ($k in $base.Keys) { [Environment]::SetEnvironmentVariable($k, $base[$k], "Process") }
  $segDir = Join-Path $out "seg$seg"
  New-Item -ItemType Directory -Force $segDir | Out-Null
  ($base.GetEnumerator() | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" }) | Set-Content -Encoding utf8 (Join-Path $segDir "env.txt")
  Get-ChildItem $user -Filter "predator*.log" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
  Get-ChildItem (Join-Path $user "logs") -Filter "*.csv" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue

  $start = Get-Date
  $p0 = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru
  $null = $p0.Handle
  $script:OwnProcs.Add($p0)
  $chain = @{}
  $chain[$p0.Id] = @{ Gen = 0; Proc = $p0; Start = $start; End = $null }
  Write-Host ("soak: seg {0} ({1}) gen 0 PID {2} started {3}" -f $seg, $name, $p0.Id, $start.ToString("HH:mm:ss"))
  $nextSample = $start
  while ($true) {
    Start-Sleep -Milliseconds 500
    $ids = @($chain.Keys)
    $filter = ($ids | ForEach-Object { "ParentProcessId=$_" }) -join " OR "
    foreach ($c in @(Get-CimInstance Win32_Process -Filter "($filter) AND Name='predator.exe'" | Where-Object { $_.ExecutablePath -eq $exe })) {
      if (-not $chain.ContainsKey([int]$c.ProcessId)) {
        $gen = $chain[[int]$c.ParentProcessId].Gen + 1
        try { $proc = Get-Process -Id $c.ProcessId; $null = $proc.Handle; $script:OwnProcs.Add($proc) } catch { $proc = $null }
        $chain[[int]$c.ProcessId] = @{ Gen = $gen; Proc = $proc; Start = Get-Date; End = $null }
        Write-Host ("soak: seg {0} gen {1} PID {2} after {3:N0} s" -f $seg, $gen, $c.ProcessId, ((Get-Date) - $start).TotalSeconds)
      }
    }
    $now = Get-Date
    if ($now -ge $nextSample) {
      $nextSample = $nextSample.AddSeconds($SampleSeconds)
      $foreign = @(Get-CimInstance Win32_Process -Filter "Name='predator.exe'" | Where-Object { -not $chain.ContainsKey([int]$_.ProcessId) }).Count
      foreach ($id in @($chain.Keys)) {
        $e = $chain[$id]
        if (-not $e.Proc -or $e.Proc.HasExited) { continue }
        try {
          $e.Proc.Refresh()
          $csv.Add([string]::Format($inv, "{0},{1},{2},{3},{4:F1},{5:F1},{6:F1},{7},{8},{9:F1},{10}", $now.ToString("HH:mm:ss"), $seg,
            $e.Gen, $id, ($now - $e.Start).TotalSeconds, $e.Proc.PrivateMemorySize64 / 1MB, $e.Proc.WorkingSet64 / 1MB,
            $e.Proc.HandleCount, $e.Proc.Threads.Count, $e.Proc.TotalProcessorTime.TotalSeconds, $foreign))
        } catch { }
      }
    }
    foreach ($id in @($chain.Keys)) { $e = $chain[$id]; if ($e.Proc -and $e.Proc.HasExited -and -not $e.End) { $e.End = Get-Date } }
    $alive = @($chain.Values | Where-Object { $_.Proc -and -not $_.Proc.HasExited })
    if ($alive.Count -eq 0) { break }
    if (($now - $start).TotalSeconds -gt $hard) {
      foreach ($e in $alive) { Write-Host "soak: PID $($e.Proc.Id) still running after $hard s, ending it"; Stop-Process -Id $e.Proc.Id -Force }
      Start-Sleep -Seconds 2
    }
  }
  $res = New-Object System.Collections.Generic.List[object]
  foreach ($id in ($chain.Keys | Sort-Object { $chain[$_].Gen })) {
    $e = $chain[$id]
    $code = if ($e.Proc) { $e.Proc.ExitCode } else { "?" }
    $wall = if ($e.End) { [int]($e.End - $e.Start).TotalSeconds } else { -1 }
    $res.Add([pscustomobject]@{ Seg = $seg; Gen = $e.Gen; Pid = $id; Exit = $code; Start = $e.Start.ToString("HH:mm:ss"); WallS = $wall })
    Write-Host ("soak: seg {0} gen {1} PID {2} exit code {3}" -f $seg, $e.Gen, $id, $code)
  }
  # logs of this segment
  $g0 = Join-Path $user "predator.log"
  if (Test-Path $g0) { Copy-Item -LiteralPath $g0 -Destination (Join-Path $segDir "g0.log") -Force }
  foreach ($g in Get-ChildItem $user -Filter "predator.gen*.log") { if ($g.Name -match "gen(\d+)") { Copy-Item -LiteralPath $g.FullName -Destination (Join-Path $segDir "g$($Matches[1]).log") -Force } }
  foreach ($c in Get-ChildItem (Join-Path $user "logs") -Filter "*.csv" -ErrorAction SilentlyContinue) { Move-Item -LiteralPath $c.FullName -Destination (Join-Path $segDir $c.Name) -Force }
  foreach ($p in Get-ChildItem (Join-Path $user "logs") -Filter "soak_*.png" -ErrorAction SilentlyContinue) { Move-Item -LiteralPath $p.FullName -Destination (Join-Path $segDir $p.Name) -Force }
  $res | Export-Csv -NoTypeInformation -Encoding utf8 (Join-Path $segDir "exits.csv")
  foreach ($k in $base.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
  return $res
}

# ---- run ----
$script:OwnProcs = New-Object System.Collections.Generic.List[object]
if ($Analyze -eq "") {
 try {
  if (-not (Test-Path $exe)) { Write-Output "soak: missing $exe"; exit 2 }
  $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
  $out = Join-Path $root "out\soak\$stamp"
  $user = Join-Path $out "user"
  New-Item -ItemType Directory -Force $user | Out-Null
  $csv = New-Object System.Collections.Generic.List[string]
  $csv.Add("time,seg,gen,pid,proc_s,private_mb,ws_mb,handles,threads,cpu_s,foreign")
  $t0 = Get-Date
  $menuCreate = "34:start,38:start,42:a,46:down,48:a,53:a,58:a,63:a"
  $menuResume = "34:start,38:start,42:a,46:down,47.5:down,49:a,53:a"
  $D = $Dwell
  if ($Only -contains 1) {
    $lv = @("00_training", "01_beach", "02_native", "03_river")
    Write-Output "soak: seg 1 fps file: $(Set-TomlFps $user 30)"
    $sec = 75 + $D + 90
    $null = Invoke-Segment 1 "instincts" @{
      "FCP_INPUT_SCRIPT" = $menuCreate; "FCP_KEYBOARD_TEXT" = "SoakSave{n}"
      "FCP_TEST_TELEPORT" = (Teleports $lv $D); "FCP_KEY_SCRIPT" = (Gameplay 80 ($sec - 5) @(110)); "FCP_KEY_SCRIPT_GENS" = "all"
      "FCP_TEST_MENU" = (Menu-Script @(110) "120:set=display.fps=120"); "FCP_TEST_MENU_GENS" = "all"
      "FCP_TEST_ALT_ENTER" = "150,165"; "FCP_TEST_ALT_ENTER_GENS" = "0,2"
      "FCP_DUMP_FRAMES" = "soak_g%g_%d.png"; "FCP_DUMP_FRAME_AT" = "3000"
    } $sec ($sec * 4 + 300) $out $user $csv
  }
  if ($Only -contains 2) {
    Write-Output "soak: seg 2 fps file: $(Set-TomlFps $user 120)"
    $sec = 60 + $LongSeconds
    $menuAt = @(); for ($t = 200; $t -lt $sec - 60; $t += 240) { $menuAt += $t }
    $alt = @(); for ($t = 320; $t -lt $sec - 60; $t += 300) { $alt += $t; $alt += ($t + 12) }
    $null = Invoke-Segment 2 "long" @{
      "FCP_INPUT_SCRIPT" = $menuResume; "FCP_KEY_SCRIPT" = (Gameplay 80 ($sec - 5) $menuAt)
      "FCP_TEST_MENU" = (Menu-Script $menuAt ""); "FCP_TEST_ALT_ENTER" = ($alt -join ",")
      "FCP_DUMP_FRAMES" = "soak_g%g_%d.png"; "FCP_DUMP_FRAME_AT" = "100000"
    } $sec ($sec * 2 + 120) $out $user $csv
  }
  if ($Only -contains 3) {
    $lv = @("21_dunea", "22_duneb", "23_dunec", "24_shantyday")
    Write-Output "soak: seg 3 fps file: $(Set-TomlFps $user 60)"
    $sec = 30 + $D + 90
    $null = Invoke-Segment 3 "evolution" @{
      "FCP_START_LEVEL" = "21_dunea"; "FCP_TEST_TELEPORT" = (Teleports $lv $D)
      "FCP_KEY_SCRIPT" = (Gameplay 30 ($sec - 5) @(60)); "FCP_KEY_SCRIPT_GENS" = "all"
      "FCP_TEST_MENU" = (Menu-Script @(60) "70:set=display.fps=30"); "FCP_TEST_MENU_GENS" = "all"
      "FCP_TEST_ALT_ENTER" = "100,112"; "FCP_TEST_ALT_ENTER_GENS" = "1,3"
    } $sec ($sec * 4 + 300) $out $user $csv
  }
  if ($Only -contains 4) {
    Write-Output "soak: seg 4 fps file: $(Set-TomlFps $user 120)"
    $m = @(); for ($t = 40; $t -le 120; $t += 20) { $m += $t }
    $null = Invoke-Segment 4 "menu" @{
      "FCP_START_LEVEL" = "21_dunea"; "FCP_KEY_SCRIPT" = (Gameplay 25 140 $m)
      "FCP_TEST_MENU" = (Menu-Script $m ""); "FCP_TEST_ALT_ENTER" = "145,155"
      "FCP_TEST_COMMAND" = "165:ReturnToMenu"
    } 240 700 $out $user $csv
  }
  $csv | Set-Content -Encoding utf8 (Join-Path $out "samples.csv")
  Write-Output ("soak: all segments done after {0:N1} min" -f ((Get-Date) - $t0).TotalMinutes)
  $Analyze = $out
 } finally {
  # on an error or Ctrl+C: end only the processes this script started (process objects, not names)
  foreach ($p in $script:OwnProcs) { if (-not $p.HasExited) { Write-Output "soak: ending own PID $($p.Id)"; Stop-Process -Id $p.Id -Force } }
 }
}

# ---- evaluation ----
$out = (Resolve-Path $Analyze).Path
$md = New-Object System.Collections.Generic.List[string]
$findings = 0
$md.Add("# Soak $(Split-Path $out -Leaf)")
$md.Add("")
$samples = @(Import-Csv (Join-Path $out "samples.csv"))
$patterns = "FATAL|crash report|unhandled title (read|write)|ACCESS_VIOLATION|EXCEPTION_|thread dump: watchdog|shutdown did not finish|bad call|unresolved (indirect|function)|no function at|relaunch: ERROR|VUID-|D3D12 ERROR"
$md.Add("| Seg | Gen | PID | Exit | Wall s | Level(s) | Target fps | fps median | p99 max ms | max ms | 1% low min | hitches >250 | game/wall | audio underruns | actions | Private MB first/max/last | WS MB max | Handles first/max/last | Threads max | Findings |")
$md.Add("|---:|---:|---:|---|---:|---|---|---:|---:|---:|---:|---:|---|---:|---|---|---:|---|---:|---|")
$fsAll = New-Object System.Collections.Generic.List[object]
foreach ($segDir in Get-ChildItem $out -Directory -Filter "seg*" | Sort-Object Name) {
  $seg = [int]$segDir.Name.Substring(3)
  $exits = @{}
  if (Test-Path (Join-Path $segDir.FullName "exits.csv")) { foreach ($x in Import-Csv (Join-Path $segDir.FullName "exits.csv")) { $exits[[int]$x.Gen] = $x } }
  foreach ($log in Get-ChildItem $segDir.FullName -Filter "g*.log" | Sort-Object { [int]$_.BaseName.Substring(1) }) {
    $g = [int]$log.BaseName.Substring(1)
    $text = Get-Content $log.FullName
    $levels = (@($text | Select-String "gametime: level '([^']+)' started" | ForEach-Object { $_.Matches[0].Groups[1].Value }) | Select-Object -Unique) -join ","
    $fs = @($text | Select-String "framestat: target (\S+) \| (\d+) frames in ([\d.]+) s = ([\d.]+) fps \| ms mean ([\d.]+) p50 ([\d.]+) p99 ([\d.]+) max ([\d.]+) \| 1% low ([\d.]+) fps \| game/wall ([\d.]+) \| level (\S+)" | ForEach-Object {
      $m = $_.Matches[0].Groups
      [pscustomobject]@{ Seg = $seg; Gen = $g; T = [double]($_.Line.Trim().Split(" ")[0]); Target = $m[1].Value; Fps = [double]$m[4].Value; P99 = [double]$m[7].Value; Max = [double]$m[8].Value; Low = [double]$m[9].Value; GW = [double]$m[10].Value; Level = $m[11].Value }
    })
    foreach ($f in $fs) { $fsAll.Add($f) }
    $inl = @($fs | Where-Object { $_.Level -ne "-" })
    $lt = @($text | Select-String "framestat: .*\| level \S+ ([\d.]+) s" | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
    if ($lt.Count) { $levels += " ({0:F0} s level time)" -f ($lt | Measure-Object -Maximum).Maximum }
    $targets = (@($fs | ForEach-Object { $_.Target }) | Select-Object -Unique) -join ","
    $med = if ($inl.Count) { ($inl | Sort-Object Fps)[[int][Math]::Floor($inl.Count / 2)].Fps } else { 0 }
    $p99 = if ($inl.Count) { ($inl | Measure-Object P99 -Maximum).Maximum } else { 0 }
    $mx = if ($inl.Count) { ($inl | Measure-Object Max -Maximum).Maximum } else { 0 }
    $low = if ($inl.Count) { ($inl | Measure-Object Low -Minimum).Minimum } else { 0 }
    $gw = if ($inl.Count) { "{0:N3}..{1:N3}" -f ($inl | Measure-Object GW -Minimum).Minimum, ($inl | Measure-Object GW -Maximum).Maximum } else { "-" }
    $hit = @($text | Select-String "hitch:|frame \d+ took").Count
    $au = @($text | Select-String "fcp audio: .*total underruns=(\d+)" | Select-Object -Last 1)
    $aud = if ($au) { $au.Matches[0].Groups[1].Value } else { "-" }
    $bad = @($text | Select-String -CaseSensitive -Pattern $patterns)
    $cc = @($text | Select-String "import (first xam!|call )XamContentCreate .* flags=([0-9A-F]+)" | ForEach-Object { $_.Matches[0].Groups[2].Value })
    $act = "alt+enter {0}, F10 {1}, pause {2}, save w/r {3}/{4}, keys {5}" -f @($text | Select-String "settings: Alt\+Enter: ").Count,
      @($text | Select-String "settings menu: open").Count, @($text | Select-String 'r4="Switch pause').Count,
      @($cc | Where-Object { $_ -eq "00000002" }).Count, @($cc | Where-Object { $_ -eq "00000003" }).Count, @($text | Select-String "key script: ").Count
    $x = $exits[$g]
    $code = if ($x) { $x.Exit } else { "?" }
    $wall = if ($x) { $x.WallS } else { "-" }
    $s = @($samples | Where-Object { [int]$_.seg -eq $seg -and [int]$_.gen -eq $g })
    $pmv = @($s | ForEach-Object { [double]$_.private_mb }); $wmv = @($s | ForEach-Object { [double]$_.ws_mb })
    $hv = @($s | ForEach-Object { [int]$_.handles }); $tv = @($s | ForEach-Object { [int]$_.threads })
    $pm = if ($s.Count) { "{0:F0}/{1:F0}/{2:F0}" -f $pmv[0], ($pmv | Measure-Object -Maximum).Maximum, $pmv[-1] } else { "-" }
    $wm = if ($s.Count) { "{0:F0}" -f ($wmv | Measure-Object -Maximum).Maximum } else { "-" }
    $hm = if ($s.Count) { "{0}/{1}/{2}" -f $hv[0], ($hv | Measure-Object -Maximum).Maximum, $hv[-1] } else { "-" }
    $tm = if ($s.Count) { ($tv | Measure-Object -Maximum).Maximum } else { "-" }
    $fnd = "-"
    if ($bad.Count -gt 0) { $findings += $bad.Count; $fnd = (($bad[0].Line.Trim()) -replace '\|', '/'); $fnd = "$($bad.Count): " + $fnd.Substring(0, [Math]::Min(100, $fnd.Length)) }
    if ("$code" -ne "0") { $findings++; if ($fnd -eq "-") { $fnd = "exit $code" } }
    $md.Add("| $seg | $g | $(if ($x) { $x.Pid } else { '-' }) | $code | $wall | $levels | $targets | $("{0:N1}" -f $med) | $("{0:N1}" -f $p99) | $("{0:N1}" -f $mx) | $("{0:N1}" -f $low) | $hit | $gw | $aud | $act | $pm | $wm | $hm | $tm | $fnd |")
  }
}
# Memory growth: linear fit of private bytes per process over the samples after the first 120 s.
$md.Add("")
$md.Add("## Memory trend per process (samples after 120 s process time)")
$md.Add("")
$md.Add("| Seg | Gen | samples | span s | Private MB start..end | slope MB/min | WS slope MB/min | handles slope /min |")
$md.Add("|---:|---:|---:|---:|---|---:|---:|---:|")
function Slope($xs, $ys) {
  $n = $xs.Count; if ($n -lt 3) { return 0 }
  $mx = ($xs | Measure-Object -Average).Average; $my = ($ys | Measure-Object -Average).Average
  $num = 0.0; $den = 0.0
  for ($i = 0; $i -lt $n; $i++) { $num += ($xs[$i] - $mx) * ($ys[$i] - $my); $den += ($xs[$i] - $mx) * ($xs[$i] - $mx) }
  if ($den -eq 0) { return 0 } else { return $num / $den }
}
foreach ($grp in ($samples | Group-Object seg, gen)) {
  $s = @($grp.Group | Where-Object { [double]$_.proc_s -ge 120 })
  if ($s.Count -lt 3) { continue }
  $xs = @($s | ForEach-Object { [double]$_.proc_s / 60 })
  $sp = Slope $xs @($s | ForEach-Object { [double]$_.private_mb })
  $sw = Slope $xs @($s | ForEach-Object { [double]$_.ws_mb })
  $sh = Slope $xs @($s | ForEach-Object { [double]$_.handles })
  $md.Add(("| {0} | {1} | {2} | {3:N0} | {4:N0}..{5:N0} | {6:N2} | {7:N2} | {8:N2} |" -f $s[0].seg, $s[0].gen, $s.Count,
    ([double]$s[-1].proc_s - [double]$s[0].proc_s), [double]$s[0].private_mb, [double]$s[-1].private_mb, $sp, $sw, $sh))
}
# Frame time per quarter of the longest in-level stretch (growing stutter?)
$md.Add("")
$md.Add("## Frame time per window over time (in level), per process: quarters")
$md.Add("")
$md.Add("| Seg | Gen | quarter | windows | fps mean | p99 mean ms | p99 max ms | max max ms | 1% low min |")
$md.Add("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
foreach ($grp in ($fsAll | Where-Object { $_.Level -ne "-" } | Group-Object Seg, Gen)) {
  $w = @($grp.Group)
  if ($w.Count -lt 8) { continue }
  for ($q = 0; $q -lt 4; $q++) {
    $part = @($w[([int]($q * $w.Count / 4))..([int](($q + 1) * $w.Count / 4) - 1)])
    $md.Add(("| {0} | {1} | {2} | {3} | {4:N1} | {5:N2} | {6:N1} | {7:N1} | {8:N1} |" -f $w[0].Seg, $w[0].Gen, ($q + 1), $part.Count,
      ($part | Measure-Object Fps -Average).Average, ($part | Measure-Object P99 -Average).Average, ($part | Measure-Object P99 -Maximum).Maximum,
      ($part | Measure-Object Max -Maximum).Maximum, ($part | Measure-Object Low -Minimum).Minimum))
  }
}
$foreignMax = if ($samples.Count) { (@($samples | ForEach-Object { [int]$_.foreign }) | Measure-Object -Maximum).Maximum } else { 0 }
$md.Add("")
$md.Add("Foreign predator.exe seen while sampling (max): $foreignMax. Findings: $findings -> $(if ($findings -eq 0) { 'PASSED' } else { 'FAILED' })")
$md | Set-Content -Encoding utf8 (Join-Path $out "soak.md")
$md | ForEach-Object { Write-Output $_ }
Write-Output "soak: report $(Join-Path $out 'soak.md')"
if ($findings -eq 0) { exit 0 } else { exit 1 }
