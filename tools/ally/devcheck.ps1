# devcheck.ps1: device detection on the device without a game start. predator.exe runs once with a fresh, temporary
# user folder and no disc image (it writes predator.toml and exits); prints the "device:" log lines and the fps/mode
# keys of the new predator.toml, then removes the temporary folder. Runs ON the device (over SSH is enough: no window).
param([string]$Root = "")
$T = if ($Root) { $Root } else { Join-Path $env:USERPROFILE "predator-test" }
$u = Join-Path $T "user_devcheck"
Remove-Item -Recurse -Force $u -ErrorAction SilentlyContinue
Get-ChildItem Env: | Where-Object { $_.Name -like 'FCP_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:FCP_USER_ROOT = $u; $env:FCP_NO_DIALOGS = "1"
$p = Start-Process -FilePath (Join-Path $T "app\predator.exe") -WorkingDirectory (Join-Path $T "app") -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(30000)) { Stop-Process -Id $p.Id -Force; "stopped after 30 s" }
Select-String -Path (Join-Path $u "predator.log") -Pattern "device:" | ForEach-Object { $_.Line.Trim() }
Select-String -Path (Join-Path $u "predator.toml") -Pattern "^(fps|mode) =" | ForEach-Object { $_.Line }
Remove-Item -Recurse -Force $u
