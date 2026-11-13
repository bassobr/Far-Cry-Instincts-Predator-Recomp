# Level transitions: plays through a campaign as a restart chain. In every level
# FCP_TEST_TELEPORT puts the player (level clock) into the level's EndMapTrigger; from there the real game path runs:
# Lua Triggers\EndMapTrigger.lua -> Game:LoadNextMap(NextMap) -> "RestartStartLevel ..." or "EndMapRebootCredits"
# -> fade out -> Game_Relaunch -> new process with "-map <next level>" or menu with credits.
# Only the path through the level (teleport) is shortened, plus for three levels the unlocking of the trigger (below).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\level_transitions.ps1 [-Campaign instincts|evolution]
#       [-From <level>] [-To <level>] [-Start devstart|menu] [-MenuInput "<FCP_INPUT_SCRIPT>"] [-TeleportAt 12]
#       [-Seconds 150] [-HardSeconds 1500] [-DumpAt "150,450"] [-Env "A=1;B=2"] [-Keep]
# -Start devstart: gen 0 via FCP_START_LEVEL=<From> (session without save game). -Start menu: gen 0 boots into the menu,
#   -MenuInput runs e.g. "Create New Game" via input script (wall clock, gen 0 only) (a save game is written).
# -To: no more teleport after this level (the chain ends there after -Seconds).
# Output (gitignored): out\transitions\<time>\ with all logs, CSV, dumps (g<gen>_<frame>.png) and transitions.md.
# Exit code 0 = every expected transition seen, no crash; 1 = findings; 2 = not evaluable.
param(
  [string]$Config = "release",
  [string]$Campaign = "instincts",
  [string]$From = "",
  [string]$To = "",
  [string]$Start = "devstart",
  [string]$MenuInput = "",
  [double]$TeleportAt = 12,
  [int]$Seconds = 150,
  [int]$HardSeconds = 1500,
  [string]$DumpAt = "",
  [string[]]$Env = @(),
  [switch]$NoEnablePatch,
  [switch]$Keep
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
if (-not (Test-Path (Join-Path $bin "predator.exe"))) { Write-Output "level_transitions: missing predator.exe ($Config)"; exit 2 }

# EndMapTrigger per level (disc image: mission XML in the level archive; name, pos, NextMap). Case as NextMap in the
# previous level (that is the level's name in the StartLevel command). Enable = ProximityTrigger that unlocks the
# disabled EndMapTrigger (event "Enable"); Patch = FCP_TEST_PATCH, in case the unlocker cannot be triggered by
# teleport (use key, or group/raft only).
$table = [ordered]@{
  "00_training"             = @{ Trig = "424.038,199.272,34.11"; Next = "01_Beach" }
  "01_beach"                = @{ Trig = "160.554,105.442,20.04"; Next = "02_Native" }
  "02_native"               = @{ Trig = "413.353,96.2241,37.81"; Next = "03_River" }
  "03_river"                = @{ Trig = "988.213,827.204,35.23"; Next = "04_Rain_Forest" }
  "04_rain_forest"          = @{ Trig = "451.21,1000.25,22.42"; Next = "05_Mines" }
  "05_mines"                = @{ Trig = "478.493,186.426,69.78"; Next = "06_Cliffs" }
  "06_cliffs"               = @{ Trig = "234.492,219.435,129.58"; Next = "07_Sub-Aquatic_Facility" }
  "07_sub-aquatic_facility" = @{ Trig = "672.726,68.2,101.18"; Next = "10_Dead_Marshes"; Enable = "647.108,131.651,105.1";
                                 Patch = 'bEnabled="0" Message="" NextMap="10_Dead_Marshes"|bEnabled="1" Message="" NextMap="10_Dead_Marshes"' }
  "10_dead_marshes"         = @{ Trig = "419.984,420.043,21.98"; Next = "11_Wasteland"; Enable = "419.742,466.473,22.48";
                                 Patch = 'bEnabled="0" Message="" NextMap="11_Wasteland"|bEnabled="1" Message="" NextMap="11_Wasteland"' }
  "11_wasteland"            = @{ Trig = "155.285,942.134,62.49"; Next = "13_Black_Forest" }
  "13_black_forest"         = @{ Trig = "379.231,147.451,154.07"; Next = "Credits"; At = 45 }
  "21_dunea"                = @{ Trig = "846.201,459.571,116.45"; Next = "22_duneb" }
  "22_duneb"                = @{ Trig = "18.1841,780.646,61.50"; Next = "23_dunec" }
  "23_dunec"                = @{ Trig = "951.25,662.25,36.80"; Next = "24_shantyday" }
  "24_shantyday"            = @{ Trig = "458.318,199.278,179.09"; Next = "26_shantynight" }
  "26_shantynight"          = @{ Trig = "201.346,891.575,145.70"; Next = "28_jungle"; Enable = "201.346,891.575,145.70" }
  "28_jungle"               = @{ Trig = "13.554,936.549,23.18"; Next = "29_forest" }
  "29_forest"               = @{ Trig = "95.4108,974.324,132.99"; Next = "30_Mountain" }
  "30_mountain"             = @{ Trig = "396.557,855.518,193.08"; Next = "Credits" }
}
$order = if ($Campaign -eq "evolution") { @("21_dunea", "22_duneb", "23_dunec", "24_shantyday", "26_shantynight", "28_jungle", "29_forest", "30_mountain") }
         else { @("00_training", "01_beach", "02_native", "03_river", "04_rain_forest", "05_mines", "06_cliffs", "07_sub-aquatic_facility", "10_dead_marshes", "11_wasteland", "13_black_forest") }
if ($From -eq "") { $From = $order[0] }
$i0 = [Array]::IndexOf($order, $From.ToLower()); if ($i0 -lt 0) { Write-Output "level_transitions: $From not in $Campaign"; exit 2 }
$i1 = if ($To -eq "") { $order.Count - 1 } else { [Array]::IndexOf($order, $To.ToLower()) }
if ($i1 -lt $i0) { Write-Output "level_transitions: -To $To lies before -From"; exit 2 }
$levels = $order[$i0..$i1]

$inv = [Globalization.CultureInfo]::InvariantCulture
$tp = New-Object System.Collections.Generic.List[string]
$patches = New-Object System.Collections.Generic.List[string]
foreach ($l in $levels) {
  $e = $table[$l]
  $T = if ($e.At) { [double]$e.At } else { $TeleportAt }   # 13: only after the helicopter approach
  if ($e.Enable) {
    $tp.Add("$l@$(($T - 4).ToString($inv)):$($e.Enable)")
    # 26: unlocker and EndMapTrigger overlap (DimX 5): step out 15 m in x once, then back in.
    if ($e.Enable -eq $e.Trig) {
      $p = $e.Trig.Split(",")
      $tp.Add("$l@$(($T - 2).ToString($inv)):$(([double]::Parse($p[0], $inv) + 15).ToString($inv)),$($p[1]),$($p[2])")
    }
  }
  if ($e.Patch -and -not $NoEnablePatch) { $patches.Add($e.Patch) }
  $tp.Add("$l@$($T.ToString($inv)):$($e.Trig)")
  $tp.Add("$l@$(($T + 15).ToString($inv)):$($e.Trig)")   # second attempt in case the first did not trigger
}

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$out = Join-Path $root "out\transitions\$stamp"
New-Item -ItemType Directory -Force $out | Out-Null
$logDir = Join-Path $bin "logs"
New-Item -ItemType Directory -Force $logDir | Out-Null
Get-ChildItem $logDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match "\.csv$|^lt_.*\.png$" } | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
Get-ChildItem $bin -Filter "predator.gen*.log" -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item -LiteralPath $_.FullName }

$vars = @("FCP_TEST_TELEPORT=$($tp -join '|')", "FCP_TRACE_COMMANDS=1", "FCP_TRACE_STATE=30", "FCP_FPS_STATS=10")
if ($Start -eq "devstart") { $vars += "FCP_START_LEVEL=$From" }
elseif ($MenuInput -ne "") { $vars += "FCP_INPUT_SCRIPT=$MenuInput" }
for ($k = 0; $k -lt $patches.Count; $k++) { $vars += $(if ($k -eq 0) { "FCP_TEST_PATCH=$($patches[$k])" } else { "FCP_TEST_PATCH$($k + 1)=$($patches[$k])" }) }
if ($DumpAt -ne "") { $vars += @("FCP_DUMP_FRAMES=lt_g%g_%d.png", "FCP_DUMP_FRAME_AT=$DumpAt") }
$vars += @(($Env -join ";").Split(";") | Where-Object { $_ -ne "" })
Write-Output "level_transitions: $Campaign $($levels -join ' -> '), start $Start, teleport at level clock $TeleportAt s"
Write-Output "level_transitions: FCP_TEST_TELEPORT=$($tp -join '|')"
# The variables reach run_chain.ps1 via the process environment (the patch texts contain quotes
# that do not survive a command line safely).
foreach ($kv in $vars) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { [Environment]::SetEnvironmentVariable($kv.Substring(0, $i), $kv.Substring($i + 1), "Process") }
}
$chain = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run_chain.ps1") `
  -Config $Config -Seconds $Seconds -HardSeconds $HardSeconds
$chain | Set-Content -Encoding utf8 (Join-Path $out "chain.txt")
$chain | ForEach-Object { Write-Output $_ }

# Collect the logs
$exits = @{}
foreach ($l in $chain) { if ($l -match "chain: gen (\d+) PID \d+ exit code (\S+)") { $exits[[int]$Matches[1]] = $Matches[2] } }
Copy-Item -LiteralPath (Join-Path $bin "predator.log") -Destination (Join-Path $out "g0.log") -Force
foreach ($g in Get-ChildItem $bin -Filter "predator.gen*.log") { if ($g.Name -match "gen(\d+)") { Copy-Item -LiteralPath $g.FullName -Destination (Join-Path $out "g$($Matches[1]).log") -Force } }
foreach ($c in Get-ChildItem $logDir -Filter "*.csv") { Move-Item -LiteralPath $c.FullName -Destination (Join-Path $out $c.Name) -Force }
foreach ($p in Get-ChildItem $logDir -Filter "lt_*.png") { Move-Item -LiteralPath $p.FullName -Destination (Join-Path $out ($p.Name -replace "^lt_", "")) -Force }

$md = New-Object System.Collections.Generic.List[string]
$md.Add("# Level transitions $stamp ($Config, $Campaign, start $Start)")
$md.Add("")
$md.Add("| Gen | Exit | Level | loaded (wall s) | Teleport (level s) | end command (lr) | Restart | Save game | Findings |")
$md.Add("|---:|---|---|---|---|---|---|---|---|")
$bad = 0
$gens = @(Get-ChildItem $out -Filter "g*.log" | ForEach-Object { [int]($_.BaseName.Substring(1)) } | Sort-Object)
$seenLevels = New-Object System.Collections.Generic.List[string]
foreach ($g in $gens) {
  $text = Get-Content (Join-Path $out "g$g.log")
  $lvl = ($text | Select-String "gametime: level '([^']*)' started at frame \d+, wall ([\d.]+) s" | Select-Object -First 1)
  $lvName = if ($lvl) { $lvl.Matches[0].Groups[1].Value } else { "-" }
  $lvWall = if ($lvl) { $lvl.Matches[0].Groups[2].Value } else { "-" }
  if ($lvl) { $seenLevels.Add($lvName.ToLower()) }
  $tpl = @($text | Select-String "FCP_TEST_TELEPORT: level '[^']*' at ([\d.]+) s" | ForEach-Object { $_.Matches[0].Groups[1].Value })
  $endc = @($text | Select-String 'Game_PushCommand #\d+ .* lr=([0-9A-F]+) r4="((Restart|FadeOutRestart|EndMapReboot|FadeOutEndMapReboot|ReturnToMenu)[^"]*)"' | ForEach-Object { "$($_.Matches[0].Groups[2].Value) ($($_.Matches[0].Groups[1].Value))" } | Select-Object -Unique)
  $rel = ($text | Select-String "relaunch: (started generation \d+|new process PID \d+ showed its first frame after [\d.]+ s|ERROR.*)" | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join "; "
  # Writing a save game = XamContentCreate with flags 2 (create), reading = flags 3 (open); diagnostics in imports.cpp
  $cc = @($text | Select-String 'import (first xam!|call )XamContentCreate .* flags=([0-9A-F]+)' | ForEach-Object { $_.Matches[0].Groups[2].Value })
  $saves = "{0} write, {1} read, {2} checkpoint" -f @($cc | Where-Object { $_ -eq "00000002" }).Count, @($cc | Where-Object { $_ -eq "00000003" }).Count, @($text | Select-String 'Game_PushCommand #\d+ .*r4="SaveGame checkpoint=').Count
  $fat = @($text | Select-String -CaseSensitive "FATAL|crash report|unhandled title (read|write)|ACCESS_VIOLATION|EXCEPTION_|thread dump: watchdog|bad call|unresolved (indirect|function)")
  $code = if ($exits.ContainsKey($g)) { $exits[$g] } else { "?" }
  if ($code -ne "0" -or $fat.Count -gt 0) { $bad++ }
  $f = if ($fat.Count) { ($fat[0].Line.Trim() -replace '\|', '/').Substring(0, [Math]::Min(120, $fat[0].Line.Trim().Length)) } else { "-" }
  $md.Add("| $g | $code | $lvName | $lvWall | $($tpl -join ', ') | $(($endc -join '; ') -replace '\|','/') | $rel | $saves | $f |")
}
# Expected sequence: every level of the list was started in a generation
$missing = @($levels | Where-Object { -not $seenLevels.Contains($_.ToLower()) })
$last = $table[$levels[-1]].Next
$md.Add("")
$md.Add("Expected: $($levels -join ' -> ') -> $last. Missing levels: $(if ($missing.Count) { $missing -join ', ' } else { 'none' }).")
if ($missing.Count) { $bad++ }
$md | Set-Content -Encoding utf8 (Join-Path $out "transitions.md")
$md | ForEach-Object { Write-Output $_ }
Write-Output "level_transitions: result $out"
if ($bad -eq 0) { exit 0 } else { exit 1 }
