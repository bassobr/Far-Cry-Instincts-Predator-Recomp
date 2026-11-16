# Several tools\issue2_pc.ps1 runs in one window of the PC measurement lock (at most ~20 min in all).
# The spec file has one run per line: the arguments of issue2_pc.ps1 as PowerShell text, e.g.
#   -App out\app -Out out\issue2\r1 -Seconds 100 -DumpEvery 10
# Lines starting with # are skipped; lines starting with ! are run as a PowerShell command (Canary reference).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\issue2_batch.ps1 -Spec out\issue2\batch1.txt -What "..."
param(
  [Parameter(Mandatory)][string]$Spec,
  [string]$What = "issue2 batch"
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
. "$PSScriptRoot\measure_lock.ps1"
$lines = @(Get-Content $Spec | Where-Object { $_.Trim() -and -not $_.TrimStart().StartsWith("#") })
TakeLock $What
$t0 = Get-Date
try {
  foreach ($l in $lines) {
    Write-Output ("issue2_batch: +{0:N0} s: {1}" -f ((Get-Date) - $t0).TotalSeconds, $l)
    # "!<command>": any other command in the same lock window (e.g. the Canary reference, tools\compare)
    if ($l.TrimStart().StartsWith("!")) { Invoke-Expression $l.TrimStart().Substring(1); continue }
    Invoke-Expression "& `"$PSScriptRoot\issue2_pc.ps1`" -NoLock $l"
  }
} finally { DropLock }
Write-Output ("issue2_batch: lock held {0:N0} s" -f ((Get-Date) - $t0).TotalSeconds)
