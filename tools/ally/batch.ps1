# batch.ps1 [-Steps a,b,...] [-Rounds n] [-Levels "a b"] [-RunS n] [-Extra "name|K=V;K=V||name|..."]: the test series
# on the Windows handheld (ROG Ally X), step by step. Runs ON the device in the interactive desktop session (started
# by task.ps1 as a temporary scheduled task). Status in runs\batch.txt, "batch done" as the last line.
# Steps: p0 (start without an image: predator.toml is written, then [game] image is set), first (game code + shader
#        pack), second, dxil (other backend's pack), perf (120/60/unlimited x Vulkan/D3D12 x levels x rounds),
#        scale ([graphics] scale=auto, 120, both backends), dumps (picture dumps), extra.
# Lists (Levels, Fps, Backends) take spaces or commas; Tag is appended to the perf runs' names (repeat series).
# Every measured run: dev start, borderless at the native resolution, statistics every 5 s, walk forward from level
# second 10 to 50 (as the Deck series, tools/deck/batch.sh).
param([string]$Steps = "p0,first,second,dxil,perf,scale,dumps", [int]$Rounds = 2,
      [string]$Levels = "21_dunea 06_cliffs 00_training", [int]$RunS = 80, [string]$Fps = "120 60 unlimited",
      [string]$Backends = "vulkan d3d12", [string]$Extra = "", [string]$Tag = "", [string]$Root = "")
$T = if ($Root) { $Root } else { Join-Path $env:USERPROFILE "predator-test" }
$X = Join-Path $T "tools\run.ps1"
$B = Join-Path $T "runs\batch.txt"
New-Item -ItemType Directory -Force (Join-Path $T "runs") | Out-Null
# one status file per batch: the previous one is kept as batch-<time>.txt
if (Test-Path $B) { Move-Item $B (Join-Path $T "runs\batch-$((Get-Item $B).LastWriteTime.ToString('yyyyMMdd-HHmmss')).txt") }
function Log($m) { Add-Content -Path $B -Value "$((Get-Date).ToString('HH:mm:ss')) $m" }
function Has($s) { (",$Steps,") -like "*,$s,*" }
$M = "FCP_NO_DIALOGS=1;FCP_FPS_STATS=5;FCP_CPU_STATS=5;FCP_INPUT_SCRIPT_CLOCK=level;FCP_INPUT_SCRIPT=10-50:lup;FCP_DISPLAY_MODE=borderless"
function One($name, $secs, $vars, $wait = "", $maxWait = 1800) {
  Log "${name}: start"
  & $X -Name $name -Seconds $secs -Vars $vars -WaitLine $wait -MaxWait $maxWait -Root $T
  $r = Join-Path $T "runs\$name\run.txt"
  Log "${name}: done ($((Get-Content $r -Tail 1 -ErrorAction SilentlyContinue)))"
  Start-Sleep -Seconds 5
}
$bat = Get-CimInstance Win32_Battery | Select-Object -First 1
Log "batch start Steps=$Steps Rounds=$Rounds Levels=$Levels RunS=$RunS, battery $($bat.EstimatedChargeRemaining) % status $($bat.BatteryStatus)"
$iso = Join-Path $T "assets\predator.iso"
$toml = Join-Path $T "user\predator.toml"
if (Has "p0") {
  One "p0" 60 "FCP_NO_DIALOGS=1"
  if (Test-Path $toml) {
    $c = Get-Content $toml -Raw
    $c = [regex]::Replace($c, '(?m)^image = .*$', "image = '$iso'")
    [IO.File]::WriteAllText($toml, $c)
    Log "p0: predator.toml written, image set: $((Select-String -Path $toml -Pattern '^image').Line)"
  } else { Log "p0: no predator.toml" }
}
if (Has "first") {
  One "first" 90 "$M;FCP_START_LEVEL=21_dunea;FCP_FPS=120;FCP_EXIT_AFTER=90" `
    "first start: shader pack .* built in|shader pack FAILED|build FAILED" 2400
}
if (Has "second") { One "second" 60 "$M;FCP_START_LEVEL=21_dunea;FCP_FPS=120;FCP_EXIT_AFTER=40" }
if (Has "dxil") {
  foreach ($be in ($Backends -split '[ ,]+')) {
    One "pack_$be" 40 "$M;FCP_START_LEVEL=21_dunea;FCP_FPS=120;FCP_RENDER_BACKEND=$be;FCP_EXIT_AFTER=40" `
      "first start: shader pack .* built in|shader pack FAILED|gametime: level .* started" 900
  }
}
if (Has "perf") {
  foreach ($r in 1..$Rounds) { foreach ($lv in ($Levels -split '[ ,]+')) { foreach ($be in ($Backends -split '[ ,]+')) { foreach ($f in ($Fps -split '[ ,]+')) {
    One "p${f}_${lv}_${be}_$r$Tag" 120 "$M;FCP_START_LEVEL=$lv;FCP_FPS=$f;FCP_RENDER_BACKEND=$be;FCP_EXIT_AFTER=$RunS"
  } } } }
}
if (Has "scale") {
  foreach ($lv in ($Levels -split '[ ,]+')) { foreach ($be in ($Backends -split '[ ,]+')) {
    One "s120_${lv}_${be}" 120 "$M;FCP_START_LEVEL=$lv;FCP_FPS=120;FCP_RENDER_BACKEND=$be;FCP_RENDER_SCALE=auto;FCP_EXIT_AFTER=$RunS"
  } }
}
if (Has "dumps") {
  foreach ($lv in ($Levels -split '[ ,]+')) { foreach ($be in ($Backends -split '[ ,]+')) {
    One "d_${lv}_${be}" 90 "$M;FCP_START_LEVEL=$lv;FCP_FPS=120;FCP_RENDER_BACKEND=$be;FCP_EXIT_AFTER=60;FCP_DUMP_FRAMES=dump_%d.png;FCP_DUMP_FRAME_EVERY=1800"
  } }
}
if (Has "extra") {
  foreach ($it in ($Extra -split '\|\|')) {
    $n, $v = $it -split '\|', 2
    One $n 120 "$M;$v"
  }
}
$bat = Get-CimInstance Win32_Battery | Select-Object -First 1
Log "batch done, battery $($bat.EstimatedChargeRemaining) % status $($bat.BatteryStatus)"
