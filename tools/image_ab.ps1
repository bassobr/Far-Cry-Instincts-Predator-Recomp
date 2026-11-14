# Image A/B for renderer changes: starts a build with a fresh (cold) or kept user folder, dumps frames and compares
# them pixel by pixel against the dumps of another run (same frame numbers).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\image_ab.ps1 -Tag new -Bin out\build\release\bin
#       [-Frames 60,200,400] [-Seconds 20] [-Level ""] [-Extra "A=1;B=2"] [-Warm] [-Against <tag>]
# Without -Level the game boots to logos/menu (deterministic early frames); with -Level the developer start.
# Dumps: out\imgab\<Tag>\f_<n>.png (gitignored, derived from game data: delete after looking). -Against compares
# with out\imgab\<tag>: per frame "identical" or the number of differing pixels and the largest channel difference.
# Only the process started here is touched or ended (by PID).
param(
  [string]$Tag = "run",
  [string]$Bin = "out\build\release\bin",
  [string]$Frames = "60,200,400",
  [int]$Seconds = 20,
  [string]$Level = "",
  [string]$Extra = "",
  [switch]$Warm,
  [string]$Against = "",
  [switch]$CompareOnly
)
$ErrorActionPreference = "Stop"
. "$PSScriptRoot\shared_root.ps1"
$root = (Resolve-Path "$PSScriptRoot\..").Path
if (-not [IO.Path]::IsPathRooted($Bin)) { $Bin = Join-Path $root $Bin }
$exe = Join-Path $Bin "predator.exe"
$out = Join-Path $root "out\imgab\$Tag"
$userRoot = Join-Path $root "out\imgab\root_$Tag"
if (-not $CompareOnly) {
if (-not $Warm -and (Test-Path $userRoot)) { Remove-Item -Recurse -Force $userRoot }
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out, $userRoot | Out-Null
$vars = @{
  "FCP_GAME_ISO" = (Get-FcpGameIso); "FCP_USER_ROOT" = $userRoot; "FCP_NO_DIALOGS" = "1"; "FCP_MUTE" = "1"
  "FCP_EXIT_AFTER" = "$Seconds"; "FCP_DUMP_FRAMES" = "$out\f_%d.png"; "FCP_DUMP_FRAME_AT" = $Frames
}
if ($Level) { $vars["FCP_START_LEVEL"] = $Level }
foreach ($kv in $Extra.Split(";")) {
  $i = $kv.IndexOf("=")
  if ($i -gt 0) { $vars[$kv.Substring(0, $i)] = $kv.Substring($i + 1) }
}
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], "Process") }
$p = Start-Process -FilePath $exe -WorkingDirectory $Bin -PassThru
if (-not $p.WaitForExit(($Seconds + 60) * 1000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(10000) | Out-Null }
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $null, "Process") }
Copy-Item (Join-Path $userRoot "predator.log") (Join-Path $out "predator.log") -ErrorAction SilentlyContinue
}
$dumps = @(Get-ChildItem $out -Filter "f_*.png" | Sort-Object Name)
Write-Output ("image_ab {0}: {1} dumps ({2})" -f $Tag, $dumps.Count, (($dumps | ForEach-Object { $_.Name }) -join ", "))
if ($Against) {
  # Pixel comparison with System.Drawing (no Python packages needed).
  Add-Type -AssemblyName System.Drawing
  $ref = Join-Path $root "out\imgab\$Against"
  function Pixels([string]$path) {
    $bmp = New-Object System.Drawing.Bitmap $path
    try {
      $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
      $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
      $bytes = New-Object byte[] ($data.Stride * $bmp.Height)
      [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
      $bmp.UnlockBits($data)
      return , @($bmp.Width, $bmp.Height, $bytes)
    } finally { $bmp.Dispose() }
  }
  foreach ($d in $dumps) {
    $q = Join-Path $ref $d.Name
    if (-not (Test-Path $q)) { Write-Output "$($d.Name): missing in $Against"; continue }
    $a = Pixels $d.FullName; $b = Pixels $q
    if ($a[0] -ne $b[0] -or $a[1] -ne $b[1]) { Write-Output "$($d.Name): size differs"; continue }
    $x = $a[2]; $y = $b[2]; $n = 0; $max = 0
    if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$x, [byte[]]$y)) { Write-Output ("{0} vs {1}: identical" -f $d.Name, $Against); continue }
    for ($k = 0; $k -lt $x.Length; $k += 4) {
      $m = [Math]::Max([Math]::Max([Math]::Abs($x[$k] - $y[$k]), [Math]::Abs($x[$k + 1] - $y[$k + 1])), [Math]::Max([Math]::Abs($x[$k + 2] - $y[$k + 2]), [Math]::Abs($x[$k + 3] - $y[$k + 3])))
      if ($m) { $n++; if ($m -gt $max) { $max = $m } }
    }
    Write-Output ("{0} vs {1}: {2}" -f $d.Name, $Against, $(if ($n -eq 0) { "identical" } else { "$n pixels differ, max $max" }))
  }
}