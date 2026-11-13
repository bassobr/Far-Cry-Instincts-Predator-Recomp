# Starts predator.exe through run.ps1 and minimizes/restores only that process's window (focus path test).
param([string]$Wt, [string]$EnvText, [int]$Seconds = 50, [int]$MinAt = 25, [int]$RestoreAt = 33)
Add-Type -Namespace W -Name U -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int cmd);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr h);
'@
$job = Start-Job -ScriptBlock {
  param($wt, $envText, $sec)
  Set-Location $wt
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\boot\run.ps1 -Config release -Seconds $sec -Env $envText
} -ArgumentList $Wt, $EnvText, $Seconds
$procId = 0
for ($i = 0; $i -lt 100 -and -not $procId; $i++) {
  Start-Sleep -Milliseconds 200
  $out = Receive-Job $job -Keep | Out-String
  if ($out -match "PID (\d+) started") { $procId = [int]$matches[1] }
}
"PID $procId"
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $MinAt) { Start-Sleep -Milliseconds 200 }
$h = (Get-Process -Id $procId).MainWindowHandle
"minimize $h at $([int]((Get-Date) - $t0).TotalSeconds) s: $([W.U]::ShowWindow($h, 6))"
while (((Get-Date) - $t0).TotalSeconds -lt $RestoreAt) { Start-Sleep -Milliseconds 200 }
"restore at $([int]((Get-Date) - $t0).TotalSeconds) s: $([W.U]::ShowWindow($h, 9)) foreground $([W.U]::SetForegroundWindow($h))"
Wait-Job $job | Out-Null
Receive-Job $job | Select-Object -Last 1
