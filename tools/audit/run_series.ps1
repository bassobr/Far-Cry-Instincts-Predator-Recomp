# Measurement series for the frame rate audit: one scenario at several frame rates, several
# runs each, one after another (only ever one own predator.exe). Before and after each run, foreign
# predator.exe/FarCryInstincts*.exe are counted (the own run has finished by then). Per run, predator.log and
# logs\state.csv end up under out\audit\<Tag>\<fps>_<n>\ (gitignored).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit\run_series.ps1 -Tag mines_path -Seconds 60
#       -Fps 30,120 -Runs 3 -Env "FCP_START_LEVEL=05_mines;FCP_TRACE_LIVING=38.69,771.85"
param(
  [Parameter(Mandatory = $true)][string]$Tag,
  [int]$Seconds = 60,
  [string]$Fps = "30,60,120",
  [int]$Runs = 3,
  [string]$Env = "",
  [string]$Config = "release"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$outRoot = Join-Path $root "out\audit\$Tag"
New-Item -ItemType Directory -Force $outRoot | Out-Null
function Foreign {
  @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -eq "predator" -or $_.ProcessName -like "FarCryInstincts*" }) |
    ForEach-Object { "$($_.ProcessName):$($_.Id)" }
}
$meta = Join-Path $outRoot "runs.txt"
for ($n = 1; $n -le $Runs; $n++) {
  foreach ($f in ($Fps -split "[,; ]+" | Where-Object { $_ })) {
    $dir = Join-Path $outRoot ("{0}_{1}" -f $f, $n)
    New-Item -ItemType Directory -Force $dir | Out-Null
    $before = Foreign
    $envs = "FCP_FPS=$f;FCP_FPS_STATS=1"
    if ($Env) { $envs = "$envs;$Env" }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run.ps1") -Config $Config -Seconds $Seconds -Env $envs | Out-Null
    $after = Foreign
    Copy-Item (Join-Path $bin "predator.log") $dir -Force
    if (Test-Path (Join-Path $bin "logs\state.csv")) { Copy-Item (Join-Path $bin "logs\state.csv") $dir -Force }
    $line = "{0} fps {1} run {2}: foreign before [{3}] after [{4}] env {5}" -f (Get-Date -Format "HH:mm:ss"), $f, $n, ($before -join ","), ($after -join ","), $envs
    Add-Content -Path $meta -Value $line
    Write-Output $line
  }
}
