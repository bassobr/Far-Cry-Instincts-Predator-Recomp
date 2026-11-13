# Measures the first level visit of 00_training via the menu (renderer cold start: shaders and pipelines not cached).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\renderer_coldstart.ps1 [-Cold] [-Config relwithdebinfo]
#       [-Seconds 80] [-Env "FCP_...=..."] [-Tag name]
# -Cold first deletes bin\cache (shader cache, pipeline list, Vulkan driver cache file; derived data).
# Output: frames/s of the first level second (first 30-frame interval with > 300 draws), hitches (> 50 ms)
# after loading (level time > 0) and whether a foreign predator.exe was running. Log copy: work\runs\cold_<Tag>.log
# The "renderer: frame ..." pattern below matches the renderer's periodic statistics line (FCP_RENDER_STATS).
param(
  [string]$Config = "relwithdebinfo",
  [int]$Seconds = 80,
  [switch]$Cold,
  [string]$Env = "",
  [string]$Tag = "run"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$foreign = @(Get-Process predator -ErrorAction SilentlyContinue | Where-Object { $_.Path -and -not $_.Path.StartsWith($root) })
if ($Cold -and (Test-Path "$bin\cache")) { Remove-Item -Recurse -Force "$bin\cache" }
$envs = "FCP_RENDER_STATS=30;FCP_TRACE_HITCHES=50;FCP_INPUT_SCRIPT=34:start,38:start,42:a,46:a,50:a"
if ($Env) { $envs = "$envs;$Env" }
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\tools\boot\run.ps1" -Config $Config -Seconds $Seconds -Env $envs | Select-Object -Last 1
$foreign2 = @(Get-Process predator -ErrorAction SilentlyContinue | Where-Object { $_.Path -and -not $_.Path.StartsWith($root) })
$log = Join-Path $bin "predator.log"
New-Item -ItemType Directory -Force "$root\work\runs" | Out-Null
Copy-Item $log "$root\work\runs\cold_$Tag.log" -Force
$lines = Get-Content $log
$first = $null
foreach ($l in $lines) {
  if ($l -match "renderer: frame (\d+): ([\d.]+) fps, last frame (\d+) draws") {
    if ([int]$Matches[3] -gt 300) { $first = "frame $($Matches[1]): $($Matches[2]) fps"; break }
  }
}
$hitches = @($lines | Where-Object { $_ -match "hitch: frame (\d+) took ([\d.]+) ms .*level ([\d.]+) s" -and [double]$Matches[3] -gt 0.0 })
$max = 0.0
foreach ($h in $hitches) { if ($h -match "took ([\d.]+) ms") { $max = [Math]::Max($max, [double]$Matches[1]) } }
Write-Output ("coldstart {0}: cold={1} first level second: {2}; hitches in level > 50 ms: {3} (max {4:N1} ms); foreign predator.exe: before {5}, after {6}" -f `
  $Tag, [bool]$Cold, $first, $hitches.Count, $max, $foreign.Count, $foreign2.Count)
$hitches | Select-Object -First 12 | ForEach-Object { $_.Substring(0, [Math]::Min(260, $_.Length)) }
