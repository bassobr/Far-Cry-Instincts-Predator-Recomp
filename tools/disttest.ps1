# Tests the release folder as on another PC: copies it to out\disttest\<name>, starts predator.exe with its own
# user folder under out\disttest (FCP_USER_ROOT, never %LOCALAPPDATA%) and without a Visual Studio environment
# (INCLUDE, LIB, LIBPATH and VS paths removed from PATH).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\disttest.ps1 -Case <case> [-Release <folder>]
#       [-Root <user-folder-name>] [-Iso <image>] [-Seconds 60] [-Env "A=1;B=2"] [-Fresh]
# Cases: noiso | wrongiso | first (image in predator.toml, first start + level) | again (second start) | raw (start
# only, switches from -Env). Only its own process is ended (by PID). Result: lines from predator.log.
# -Iso default: FCP_GAME_ISO, else the image under <shared root>\assets (tools\shared_root.ps1).
param(
  [Parameter(Mandatory = $true)][ValidateSet("noiso", "wrongiso", "first", "again", "raw")][string]$Case,
  [string]$Release = "",
  [string]$Root = "user",
  [string]$Iso = "",
  [int]$Seconds = 60,
  [string]$Env = "",
  [switch]$Fresh,
  [switch]$CopyRelease
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
if (-not $Iso) { $Iso = Get-FcpGameIso }
$repo = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $repo "out\dist"
if (-not $Release) {
  $Release = (Get-ChildItem $dist -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
}
$name = Split-Path -Leaf $Release
$testRoot = Join-Path $repo "out\disttest"
$app = Join-Path $testRoot $name
if ($CopyRelease -or -not (Test-Path (Join-Path $app "predator.exe"))) {
  if (Test-Path $app) { Remove-Item -LiteralPath $app -Recurse -Force }
  New-Item -ItemType Directory -Force $testRoot | Out-Null
  Copy-Item -LiteralPath $Release -Destination $app -Recurse
  Write-Output "disttest: $Release -> $app"
}
$user = Join-Path $testRoot $Root
if ($Fresh -and (Test-Path $user)) { Remove-Item -LiteralPath $user -Recurse -Force }
New-Item -ItemType Directory -Force $user | Out-Null

# a clean environment: no VS variables, PATH without Visual Studio and without the LLVM toolchain
foreach ($v in "INCLUDE", "LIB", "LIBPATH", "VSINSTALLDIR", "VCINSTALLDIR", "VCToolsInstallDir", "WindowsSdkDir") {
  [Environment]::SetEnvironmentVariable($v, $null, "Process")
}
$cleanPath = ($env:PATH.Split(";") | Where-Object { $_ -and $_ -notmatch 'Visual Studio|Windows Kits|toolchain\\llvm|\\LLVM\\' }) -join ";"
[Environment]::SetEnvironmentVariable("PATH", $cleanPath, "Process")
$vars = @{
  "FCP_USER_ROOT" = $user; "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"; "FCP_EXIT_AFTER" = "$Seconds"
  "REX_UI_WINDOW_WIDTH" = "640"; "REX_UI_WINDOW_HEIGHT" = "360"; "FCP_GAME_ISO" = $null
}
foreach ($kv in $Env.Split(";")) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }

# predator.toml: [game] image, the way a player enters it
function Set-Image([string]$path) {
  $toml = Join-Path $user "predator.toml"
  if (-not (Test-Path $toml)) { throw "predator.toml missing in $user (run -Case noiso first)" }
  $text = [IO.File]::ReadAllText($toml)
  $val = $path -replace '\\', '/'
  $text = [regex]::Replace($text, '(?m)^image\s*=.*$', "image = `"$val`"")
  [IO.File]::WriteAllText($toml, $text)
}
switch ($Case) {
  "wrongiso" { Set-Image $Iso }
  "first" { Set-Image $Iso }
}

$exe = Join-Path $app "predator.exe"
$start = Get-Date
$p = Start-Process -FilePath $exe -WorkingDirectory $app -PassThru
$limit = $Seconds + 900
if (-not $p.WaitForExit($limit * 1000)) {
  Write-Output "disttest: PID $($p.Id) still running after $limit s, ending it (this process only)"
  Stop-Process -Id $p.Id -Force
  $p.WaitForExit(10000) | Out-Null
}
$dur = ((Get-Date) - $start).TotalSeconds
Write-Output ("disttest {0}: done after {1:N1} s, exit code {2}" -f $Case, $dur, $p.ExitCode)
$log = Join-Path $user "predator.log"
if (Test-Path $log) {
  Select-String -Path $log -Pattern 'game code:|first start:|imports: the game|level .* started|title exit|FATAL|settings: .*image' |
    Select-Object -First 40 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(300, $_.Line.Length)) }
}
