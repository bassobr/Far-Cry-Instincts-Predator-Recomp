# task.ps1 [-BatchArgs "<args for batch.ps1>"] | -Remove: starts batch.ps1 in the logged-on user's interactive
# desktop session through a temporary scheduled task (an SSH session runs without a desktop, so a game started from
# it would get no window). Runs ON the device, called over SSH (ally.py psf tools\ally\task.ps1 ...).
# The task is "interactive only" (no password stored), may start on battery, and is removed with -Remove after the
# batch. Task name: FCP_ALLY_TASK (default PredatorTest). After FCI's Ally task.ps1 (MIT, same author).
param([string]$BatchArgs = "", [switch]$Remove, [string]$Root = "")
$name = if ($env:FCP_ALLY_TASK) { $env:FCP_ALLY_TASK } else { "PredatorTest" }
$T = if ($Root) { $Root } else { Join-Path $env:USERPROFILE "predator-test" }
if ($Remove) {
  $task = Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
  if ($task -and $task.State -eq 'Running') { Stop-ScheduledTask -TaskName $name }
  Unregister-ScheduledTask -TaskName $name -Confirm:$false -ErrorAction SilentlyContinue
  "task $name present: " + [bool](Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue)
  return
}
$explorer = Get-Process explorer -IncludeUserName -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $explorer) { $explorer = Get-Process explorer -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $explorer) { "no desktop session (explorer.exe not running): nobody logged on, not started"; return }
"desktop session $($explorer.SessionId)"
$ps = "-NoProfile -ExecutionPolicy Bypass -WindowStyle Minimized -File `"$T\tools\batch.ps1`" -Root `"$T`" $BatchArgs"
$a = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $ps -WorkingDirectory $T
$s = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 6)
$p = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive
Register-ScheduledTask -TaskName $name -Action $a -Settings $s -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName $name
Start-Sleep -Seconds 3
Get-ScheduledTask -TaskName $name | ForEach-Object { "task $($_.TaskName) $($_.State)" }
