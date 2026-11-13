# wait.ps1 [-Pattern regex] [-Every s] [-Max s]: runs on the PC; polls status.ps1 on the device (ally.py, FCP_ALLY_HOST
# and FCP_ALLY_USER from the environment) until its output matches Pattern (default: the batch is done), then prints
# the last status. Python: FCP_PYTHON or python on PATH.
param([string]$Pattern = "batch done", [int]$Every = 30, [int]$Max = 21600)
$py = if ($env:FCP_PYTHON) { $env:FCP_PYTHON } else { "python" }
$ally = Join-Path $PSScriptRoot "ally.py"; $st = Join-Path $PSScriptRoot "status.ps1"
$t0 = Get-Date
while ($true) {
  $out = (& $py -I $ally psf $st 2>&1 | Out-String)
  if ($out -match $Pattern) { break }
  if (((Get-Date) - $t0).TotalSeconds -gt $Max) { "wait: timeout after $Max s"; break }
  Start-Sleep -Seconds $Every
}
$out
