# Short run per level for the frame rate audit: one run per level via
# tools\audit\run_series.ps1 (tag <Prefix>_<Level>, same counting of foreign processes), e.g. to find the
# player's weapons (state.csv: clip0..3) or to pre-check a scenario.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\audit\probe_levels.ps1 -Prefix weapons -Seconds 30
#       -Levels "06_cliffs,21_dunea" -Fps 30 -Env "FCP_TRACE_STATE=1;FCP_INPUT_SCRIPT_CLOCK=level;FCP_INPUT_SCRIPT=15:left"
param(
  [Parameter(Mandatory = $true)][string]$Prefix,
  [Parameter(Mandatory = $true)][string]$Levels,
  [int]$Seconds = 30,
  [string]$Fps = "30",
  [int]$Runs = 1,
  [string]$Env = "",
  [string]$Config = "release"
)
$ErrorActionPreference = "Stop"
foreach ($level in ($Levels -split "[,; ]+" | Where-Object { $_ })) {
  $envs = "FCP_START_LEVEL=$level"
  if ($Env) { $envs = "$envs;$Env" }
  & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run_series.ps1") -Tag "${Prefix}_$level" `
    -Seconds $Seconds -Fps $Fps -Runs $Runs -Env $envs -Config $Config
}
