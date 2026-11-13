<#
.SYNOPSIS
  Measures the rebuild of the generated game code (predator_recompiled + link of predator.exe) in an already
  configured and once built build folder. SDK, codegen and coverage check do not run
  (like the phase-0 measurement, where the SDK was preinstalled).

.DESCRIPTION
  Deletes the object files of predator_recompiled and predator, measures 5 s of CPU base load, builds
  "ninja -j <Jobs> predator" via tools\ninja_env.cmd and prints wall time, number of compiler invocations and the
  sum of the job wall times from .ninja_log (no CPU time). The result is also appended as a line to
  out\logs\measure_build.tsv.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\measure_build.ps1 -Preset release -Jobs 32 -Label j32_1
#>
param(
  [string]$Preset = "release",
  [int]$Jobs = 32,
  [string]$Label = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$bdir = Join-Path $repo "out\build\$Preset"
if (-not (Test-Path (Join-Path $bdir "build.ninja"))) { throw "not configured: $bdir (run build.cmd $Preset first)" }

foreach ($d in "CMakeFiles\predator_recompiled.dir", "CMakeFiles\predator.dir") {
  $p = Join-Path $bdir $d
  if (Test-Path $p) { Get-ChildItem $p -Recurse -File -Include *.obj, *.pch | Remove-Item -Force }
}
$exe = Join-Path $bdir "bin\predator.exe"
if (Test-Path $exe) { Remove-Item -Force $exe }

$logFile = Join-Path $bdir ".ninja_log"
$before = if (Test-Path $logFile) { (Get-Content $logFile).Count } else { 0 }

$load = $null
try {
  $load = (Get-Counter "\Processor(_Total)\% Processor Time" -SampleInterval 5 -MaxSamples 1).CounterSamples.CookedValue
} catch {
  try { $load = (Get-Counter "\Prozessor(_Total)\Prozessorzeit (%)" -SampleInterval 5 -MaxSamples 1).CounterSamples.CookedValue } catch { }
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
& cmd /c (Join-Path $repo "tools\ninja_env.cmd") -C $bdir -j $Jobs predator | Out-Null
$rc = $LASTEXITCODE
$sw.Stop()
if ($rc -ne 0) { throw "ninja failed ($rc)" }

$jobsSum = 0.0; $compiles = 0; $slow = @()
$lines = Get-Content $logFile | Select-Object -Skip ([Math]::Max($before, 1))
foreach ($l in $lines) {
  $f = $l -split "`t"
  if ($f.Count -lt 4) { continue }
  $dur = ([double]$f[1] - [double]$f[0]) / 1000.0
  $jobsSum += $dur
  if ($f[3] -like "*.obj") { $compiles++; $slow += [pscustomobject]@{ s = $dur; f = (Split-Path -Leaf $f[3]) } }
}
$top = ($slow | Sort-Object s -Descending | Select-Object -First 5 | ForEach-Object { "{0} {1:N1}s" -f $_.f, $_.s }) -join ", "
$loadText = if ($null -ne $load) { "{0:N1} %" -f $load } else { "?" }
"{0}: wall time {1:N2} s, -j{2}, {3} compiler invocations, sum of job wall time {4:N1} s, base load before {5}" -f $Label, $sw.Elapsed.TotalSeconds, $Jobs, $compiles, $jobsSum, $loadText
"  slowest: $top"
$tsv = Join-Path $repo "out\logs\measure_build.tsv"
New-Item -ItemType Directory -Force -Path (Split-Path $tsv) | Out-Null
("{0}`t{1}`t{2}`t{3:N2}`t{4}`t{5:N1}`t{6}" -f (Get-Date -Format s), $Label, $Jobs, $sw.Elapsed.TotalSeconds, $compiles, $jobsSum, $loadText) | Add-Content $tsv
