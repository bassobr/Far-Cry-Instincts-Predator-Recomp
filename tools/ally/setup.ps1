# setup.ps1 [-Zip release.zip]: unpacks the release zip into app\ of the test folder (replacing app\ only; user\,
# runs\ and assets\ stay). Runs ON the device. Prints the unpacked size and the image's size.
param([string]$Zip = "release.zip", [string]$Root = "")
$T = if ($Root) { $Root } else { Join-Path $env:USERPROFILE "predator-test" }
$z = Join-Path $T $Zip; $app = Join-Path $T "app"; $tmp = Join-Path $T "app.unpack"
if (-not (Test-Path $z)) { "no $z"; return }
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
$t0 = Get-Date
Expand-Archive -Path $z -DestinationPath $tmp -Force
$inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
if (Test-Path $app) { Remove-Item -Recurse -Force $app }
Move-Item $inner.FullName $app
Remove-Item -Recurse -Force $tmp
$n = Get-ChildItem $app -Recurse -File | Measure-Object Length -Sum
"app: $($n.Count) files, $([math]::Round($n.Sum / 1MB, 1)) MB, unpacked in $([int]((Get-Date) - $t0).TotalSeconds) s, portable.txt: $(Test-Path (Join-Path $app 'portable.txt'))"
Get-Item (Join-Path $T "assets\predator.iso") -ErrorAction SilentlyContinue | ForEach-Object { "image: $($_.Length) bytes" }
