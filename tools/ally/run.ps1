# run.ps1 -Name <run> [-Seconds n] [-Vars "K=V;K=V"] [-WaitLine regex] [-MaxWait n]: one run of predator.exe on the
# Windows handheld. Runs ON the device, inside the interactive desktop session (task.ps1 starts batch.ps1 as a
# temporary scheduled task; a plain SSH session has no desktop). Everything stays under $T (default
# %USERPROFILE%\predator-test): app\ (release folder), user\ (FCP_USER_ROOT), runs\<name>\ (results).
#   Seconds   run time; with WaitLine counted from the first match of WaitLine in predator.log (first start)
#   MaxWait   give up waiting for WaitLine after this many seconds (default 1800)
# The game's own FCP_EXIT_AFTER ends a run cleanly; at the limit + 30 s only this run's processes (the started
# predator.exe and its children, followed by parent PID) are stopped by PID.
# Sensors every 2 s into sensors.csv, all without installed tools (WMI performance classes, language independent):
# GPU 3D engine utilisation of our process, CPU performance % (x base clock = effective MHz), battery charge and
# discharge rate (0 on mains power).
# Idea and structure after tools/deck/run.sh and FCI's Ally run.ps1 (MIT, same author).
param([string]$Name, [int]$Seconds = 60, [string]$Vars = "", [string]$WaitLine = "", [int]$MaxWait = 1800,
      [string]$Root = "")
$ErrorActionPreference = 'Continue'
$T = if ($Root) { $Root } else { Join-Path $env:USERPROFILE "predator-test" }
$App = Join-Path $T "app"; $U = Join-Path $T "user"; $O = Join-Path $T "runs\$Name"
New-Item -ItemType Directory -Force $O, $U | Out-Null
function Log($m) { Add-Content -Path (Join-Path $O "run.txt") -Value "$((Get-Date).ToString('HH:mm:ss')) $m" }
Get-ChildItem Env: | Where-Object { $_.Name -like 'FCP_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:FCP_USER_ROOT = $U
foreach ($kv in ($Vars -split ';')) {
  if ($kv -match '^\s*([A-Za-z_][A-Za-z0-9_]*)=(.*)$') { Set-Item "Env:$($Matches[1])" $Matches[2] }
}
Get-ChildItem Env: | Where-Object { $_.Name -like 'FCP_*' } | Sort-Object Name | ForEach-Object { "$($_.Name)=$($_.Value)" } |
  Set-Content (Join-Path $O "env.txt")
$log = Join-Path $U "predator.log"
Remove-Item -Force $log, (Join-Path $U "predator.gen*.log") -ErrorAction SilentlyContinue
$others = @(Get-Process predator -ErrorAction SilentlyContinue | Where-Object { $_.Path -notlike "$App*" })
$p = Start-Process -FilePath (Join-Path $App "predator.exe") -WorkingDirectory $App -PassThru
$known = @{}; $known[$p.Id] = 1
Log "start ${Name}: pid $($p.Id), $Seconds s, wait '$WaitLine'"
$cpuMax = (Get-CimInstance Win32_Processor | Select-Object -First 1).MaxClockSpeed
$sens = Join-Path $O "sensors.csv"
"t,gpu_busy,cpu_perf_pct,cpu_mhz_avg,cpu_util,batt_pct,batt_w,on_ac" | Set-Content $sens
$t0 = Get-Date; $limit = if ($WaitLine) { $MaxWait } else { $Seconds }; $started = $null; $last = -10
function Mine {
  Get-CimInstance Win32_Process | Where-Object {
    $known.ContainsKey([int]$_.ProcessId) -or $known.ContainsKey([int]$_.ParentProcessId) }
}
while ($true) {
  Start-Sleep -Milliseconds 500
  $el = ((Get-Date) - $t0).TotalSeconds
  $m = @(Mine)
  foreach ($x in $m) { $known[[int]$x.ProcessId] = 1 }
  if (-not ($m | Where-Object { $_.Name -eq 'predator.exe' })) { Log ("predator exited at {0:F0}s" -f $el); break }
  if ($el - $last -ge 2) {
    $last = $el
    try {
      $pids = ($m | Where-Object { $_.Name -eq 'predator.exe' } | ForEach-Object { "pid_$($_.ProcessId)_" })
      $g = 0.0
      Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -ErrorAction Stop |
        Where-Object { $_.Name -like '*engtype_3D' } | ForEach-Object {
          $n = $_.Name; if ($pids | Where-Object { $n -like "*$_*" }) { $g += [double]$_.UtilizationPercentage } }
      $pi = Get-CimInstance Win32_PerfFormattedData_Counters_ProcessorInformation -Filter "Name='_Total'" -ErrorAction Stop
      $perf = [double]$pi.PercentProcessorPerformance
      $b = Get-CimInstance Win32_Battery | Select-Object -First 1
      $bs = Get-CimInstance -Namespace root\wmi BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
      $w = if ($bs) { [math]::Round([double]$bs.DischargeRate / 1000, 2) } else { "" }
      $ac = if ($bs) { [int][bool]$bs.PowerOnline } else { "" }
      [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:F0},{1:F0},{2:F0},{3:F0},{4:F0},{5},{6},{7}",
        $el, $g, $perf, ($perf * $cpuMax / 100), [double]$pi.PercentProcessorTime, $b.EstimatedChargeRemaining, $w, $ac) |
        Add-Content $sens
    } catch { }
  }
  if ($WaitLine -and -not $started -and (Test-Path $log)) {
    if (Select-String -Path $log -Pattern $WaitLine -Quiet) {
      $started = $el; $limit = $el + $Seconds; Log ("wait line at {0:F0}s, limit now {1:F0}s" -f $el, $limit)
    }
  }
  if ($el -gt $limit + 30) { Log ("limit {0:F0}s + 30 s reached: stopping own processes" -f $limit); break }
}
Start-Sleep -Seconds 2
$m = @(Mine)
foreach ($x in $m) { Stop-Process -Id $x.ProcessId -Force -ErrorAction SilentlyContinue }
$others += @(Get-Process predator -ErrorAction SilentlyContinue | Where-Object { $_.Path -notlike "$App*" })
Copy-Item $log, (Join-Path $U "predator.gen*.log"), (Join-Path $U "predator.toml") $O -ErrorAction SilentlyContinue
$ld = Join-Path $U "logs"
if (Test-Path $ld) { New-Item -ItemType Directory -Force (Join-Path $O "logs") | Out-Null; Move-Item (Join-Path $ld "*") (Join-Path $O "logs") -Force -ErrorAction SilentlyContinue }
if ($others.Count) { "other predator.exe ran: " + (($others | Select-Object -Unique Id | ForEach-Object Id) -join ',') | Set-Content (Join-Path $O "others.txt") }
Log ("end after {0:F0}s, still running at the end {1}, processes seen {2}, exit code {3}" -f ((Get-Date) - $t0).TotalSeconds, $m.Count, $known.Count, $p.ExitCode)
