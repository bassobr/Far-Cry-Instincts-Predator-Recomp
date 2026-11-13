# Standard stability run: scripted through menu and levels of both games with two restarts, then evaluation
# of the logs and the Windows event log.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\stability.ps1 [-Config release] [-GenSeconds 95]
#       [-LastSeconds 100] [-Env "FCP_FPS_LIMIT=120;..."] [-Keep]
# Sequence (one process chain, tools\boot\run_chain.ps1):
#   Gen 0: logos, intro, main menu via input script, story -> 00_training (Instincts), after GenSeconds
#          "FadeOutRestartStartLevel level=21_dunea" -> restart
#   Gen 1: 21_dunea (Evolution), after GenSeconds the same command -> restart
#   Gen 2: 21_dunea, runs LastSeconds until the time limit
# About 290 s in total with the defaults. Exit code 0 = no findings, 1 = findings, 2 = run not evaluable.
# Result: summary on stdout and in out\logs\stability_<time>.txt. The chain's logs are deleted
# afterwards (unless -Keep).
param(
  [string]$Config = "release",
  [int]$GenSeconds = 95,
  [int]$LastSeconds = 100,
  [string[]]$Env = @(),
  [switch]$Keep,
  [switch]$Strict
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..").Path
$bin = Join-Path $root "out\build\$Config\bin"
$exe = Join-Path $bin "predator.exe"
if (-not (Test-Path $exe)) { Write-Output "stability: missing $exe"; exit 2 }
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$report = Join-Path $root "out\logs\stability_$stamp.txt"
New-Item -ItemType Directory -Force (Split-Path $report) | Out-Null

# Gen 0 and 1 end through the restart before the time limit; the time limit (FCP_EXIT_AFTER) applies per process
# and ends gen 2.
$limit = [Math]::Max($LastSeconds, $GenSeconds + 30)
$vars = @(
  "FCP_INPUT_SCRIPT=34:start,38:start,42:a,46:a,50:a,54:a,58:a,62:a",
  "FCP_TEST_COMMAND=$($GenSeconds):FadeOutRestartStartLevel level=21_dunea",
  "FCP_TEST_COMMAND_GENS=0,1",
  "FCP_TRACE_HITCHES=250",
  "FCP_TRACE_COMMANDS=1"
) + $Env
$start = Get-Date
$chain = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root "tools\boot\run_chain.ps1") `
  -Config $Config -Seconds $limit -HardSeconds ($GenSeconds * 2 + $limit + 120) -Env ($vars -join ";")
$chain | ForEach-Object { Write-Output $_ }
$dur = ((Get-Date) - $start).TotalSeconds

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("stability: $stamp, configuration $Config, duration {0:N0} s" -f $dur)
$findings = 0
$logs = @(Get-ChildItem $bin -Filter "predator*.log" | Where-Object { $_.LastWriteTime -ge $start } | Sort-Object Name)
if ($logs.Count -eq 0) { $lines.Add("stability: no logs"); $lines | Set-Content -Encoding utf8 $report; $lines; exit 2 }
$patterns = [ordered]@{
  "FATAL" = "FATAL";
  "crash" = "crash report|unhandled title (read|write)|ACCESS_VIOLATION|EXCEPTION_";
  "Watchdog" = "thread dump: watchdog|shutdown did not finish";
  "bad call" = "bad call|unresolved (indirect|function)|no function at";
  "shader error" = "renderer: Shader .* failed|unsupported interpolator|fcshaders.*error";
  "validation" = "VUID-|Validation Error|validation layer|D3D12 ERROR";
  "restart error" = "relaunch: ERROR";
}
# Known errors that are tracked elsewhere: counted and reported, but not rated as a finding
# (-Strict rates them too). FOG0: the renderer does not yet map the FOG0 interpolator semantic.
$known = "unsupported interpolator semantic FOG0"
foreach ($log in $logs) {
  $text = Get-Content $log.FullName
  $swaps = ($text | Select-String "stat t=.*swaps=(\d+)" | Select-Object -Last 1)
  $levels = ($text | Select-String "gametime: level '([^']+)' started" | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ","
  $hitches = @($text | Select-String "^\s*[\d.]+\s+\d+ hitch:").Count
  $clock = ($text | Select-String "clock: .*= ([\d.]+) fps" | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
  $fps = if ($clock) { "{0:N1}..{1:N1}" -f ($clock | Measure-Object -Minimum).Minimum, ($clock | Measure-Object -Maximum).Maximum } else { "-" }
  $lines.Add(("{0}: {1} lines, levels [{2}], fps {3}, hitches >250 ms {4}, last stat: {5}" -f $log.Name,
    $text.Count, $levels, $fps, $hitches, ($(if ($swaps) { $swaps.Line.Trim().Substring(0, [Math]::Min(60, $swaps.Line.Trim().Length)) } else { "-" }))))
  foreach ($k in $patterns.Keys) {
    $hits = @($text | Select-String -CaseSensitive -Pattern $patterns[$k])
    if (-not $Strict) {
      $kn = @($hits | Where-Object { $_.Line -match $known })
      if ($kn.Count -gt 0) { $lines.Add(("  known {0}: {1} lines ({2})" -f @($k, $kn.Count, $known))) }
      $hits = @($hits | Where-Object { $_.Line -notmatch $known })
    }
    if ($hits.Count -gt 0) {
      $findings += $hits.Count
      $first = $hits[0].Line.Trim()
      if ($first.Length -gt 200) { $first = $first.Substring(0, 200) }
      $lines.Add(("  FINDING {0}: {1} lines, first: {2}" -f @($k, $hits.Count, $first)))
    }
  }
}
foreach ($l in ($chain | Select-String "exit code")) {
  if ($l.Line -match "gen (\d+) PID (\d+) exit code (\S+)" -and $Matches[3] -ne "0") {
    $findings++
    $lines.Add("  FINDING exit code: $($l.Line.Trim())")
  }
}
if (-not ($chain | Select-String "still running: 0")) { $findings++; $lines.Add("  FINDING: processes of the chain are still running") }
# Windows event log: Application Error (1000) for predator.exe from this build folder since the start.
try {
  $events = @(Get-WinEvent -FilterHashtable @{ LogName = "Application"; Id = 1000; StartTime = $start } -ErrorAction Stop |
    Where-Object { $_.Message -match "predator\.exe" -and $_.Message -match [regex]::Escape($bin) })
} catch { $events = @() }
$lines.Add("Event log: $($events.Count) Application Error (1000) for $exe")
foreach ($e in $events) { $findings++; $lines.Add(("  FINDING event {0}: {1}" -f $e.TimeCreated, ($e.Message -split "`n")[0])) }
$gens = @($chain | Select-String "chain: gen \d+ PID \d+ \(parent").Count
$lines.Add(("Result: {0} processes ({1} restarts), {2} findings -> {3}" -f ($gens + 1), $gens, $findings,
  $(if ($findings -eq 0) { "PASSED" } else { "FAILED" })))
$lines | Set-Content -Encoding utf8 $report
$lines | ForEach-Object { Write-Output $_ }
Write-Output "stability: report $report"
if (-not $Keep) {
  foreach ($log in $logs) { Remove-Item $log.FullName -ErrorAction SilentlyContinue }
  Get-ChildItem (Join-Path $bin "logs") -Filter "*.csv" -ErrorAction SilentlyContinue | Remove-Item -ErrorAction SilentlyContinue
}
if ($findings -eq 0) { exit 0 } else { exit 1 }
