# Paired measurement of two build folders: A and B run tools\perf_measure.ps1 at the same time, one on each half of
# CCD0 (affinity 0xFF = CPUs 0-7, 0xFF00 = CPUs 8-15, each 4 cores with SMT like the Steam Deck), the halves swap every
# round. Load from other processes then hits both alike (docs/phase1/leistung-runde2-hauptthread.md, section 3).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\perf_pair.ps1 -A base -B new [-Levels 21_dunea,05_mines]
#       [-Fps 60] [-Rounds 2] [-Seconds 60] [-Tag pair] [-ExtraA "K=V;..."] [-ExtraB "K=V;..."]
# -A/-B name folders out\build\<name>\bin (copies of a bin folder). Each level waits for and takes the PC measurement
# lock (tools\measure_lock.ps1: lock, queue, 60 s pause) and releases it afterwards. Only processes started here are touched.
# Output: one summary line per run (perf_measure.ps1); logs in out\logs\perf\<Tag>_<A|B>_<level>_r<n>_1.log.
param(
  [Parameter(Mandatory = $true)][string]$A,
  [Parameter(Mandatory = $true)][string]$B,
  [string]$Levels = "21_dunea",
  [string]$Fps = "60",
  [int]$Rounds = 2,
  [int]$Seconds = 60,
  [string]$Tag = "pair",
  [string]$ExtraA = "",
  [string]$ExtraB = ""
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$tmp = Join-Path $root "out\logs\perf"
New-Item -ItemType Directory -Force $tmp | Out-Null

. "$PSScriptRoot\measure_lock.ps1"
function Start-Run([string]$cfg, [string]$aff, [string]$lv, [int]$r, [string]$extra, [string]$out) {
  $a = "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\perf_measure.ps1`" -Tag ${Tag}_${cfg}_${lv}_r$r -Runs 1" +
       " -Config $cfg -Fps $Fps -Affinity $aff -Level $lv -Seconds $Seconds"
  if ($extra) { $a += " -Extra `"$extra`"" }
  Start-Process powershell -ArgumentList $a -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
}

foreach ($lv in ($Levels -split ",")) {
  TakeLock "pair $A/$B $lv"
  try {
    for ($r = 1; $r -le $Rounds; $r++) {
      $affA = if ($r % 2) { "FF" } else { "FF00" }
      $affB = if ($r % 2) { "FF00" } else { "FF" }
      $outA = Join-Path $tmp "${Tag}_A.txt"; $outB = Join-Path $tmp "${Tag}_B.txt"
      $pa = Start-Run $A $affA $lv $r $ExtraA $outA
      Start-Sleep -Milliseconds 300
      $pb = Start-Run $B $affB $lv $r $ExtraB $outB
      $pa.WaitForExit(); $pb.WaitForExit()
      "$lv $Fps r$r A=$A ($affA) :: " + (Get-Content $outA | Select-Object -Last 1)
      "$lv $Fps r$r B=$B ($affB) :: " + (Get-Content $outB | Select-Object -Last 1)
    }
  } finally { DropLock }
}
