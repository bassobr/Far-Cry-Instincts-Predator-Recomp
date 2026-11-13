# status.ps1 [-Tail n]: state of the test task, last lines of runs\batch.txt, own processes, battery. Runs ON the device.
param([int]$Tail = 12, [string]$Root = "")
$name = if ($env:FCP_ALLY_TASK) { $env:FCP_ALLY_TASK } else { "PredatorTest" }
$T = if ($Root) { $Root } else { Join-Path $env:USERPROFILE "predator-test" }
$task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
if ($task) { "task $name $($task.State), last result $(($task | Get-ScheduledTaskInfo).LastTaskResult)" } else { "task ${name}: none" }
Get-Content (Join-Path $T "runs\batch.txt") -Tail $Tail -ErrorAction SilentlyContinue
Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -like "$T\*" -or ($_.Name -eq 'powershell.exe' -and $_.CommandLine -like "*predator-test*batch.ps1*") } |
  ForEach-Object { "proc $($_.ProcessId) $($_.Name) session $($_.SessionId)" }
Get-CimInstance Win32_Battery | ForEach-Object { "battery $($_.EstimatedChargeRemaining) % status $($_.BatteryStatus)" }
