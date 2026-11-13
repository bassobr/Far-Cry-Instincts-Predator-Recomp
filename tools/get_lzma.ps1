# Fetches the LZMA decoder of the LZMA SDK (Igor Pavlov, public domain) in the pinned version 24.09 to
# third_party\lzma-sdk (gitignored). On the first start predator.exe uses it to unpack the shader containers from
# shaders.fat/common.fat of the disc image (src/fcp/first_start.cpp), like tools/xfat.py does with Python's lzma.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\get_lzma.ps1 [-Out <folder>]
param([string]$Out = "")
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
if (-not $Out) { $Out = Join-Path (Split-Path -Parent $PSScriptRoot) "third_party\lzma-sdk" }
New-Item -ItemType Directory -Force $Out | Out-Null
$base = "https://raw.githubusercontent.com/ip7z/7zip/24.09/C"
$files = [ordered]@{
    "LzmaDec.c"   = "B9CA2B8707400347C75D6FEF288D1354A3B4CF93E8DA42F26207B8BD4C4D5E59"
    "LzmaDec.h"   = "3AAF07B4AE4173A2D103179455DC7089B5DDBC7FC3DB3C0E40964A7499C69266"
    "7zTypes.h"   = "5DE943C886D5D7BBCF16203E9B292A9E7BE327B43FE3DFD9F4AD07D6D0870EF6"
    "Compiler.h"  = "959DAC87AC7109502F6467E910D0722B932176E243153BF8C663479AA8C29B20"
    "Precomp.h"   = "FEA249C753DFBBDD69D9397E91497205452B784279F70CAD4658DC567CD2F85F"
    "7zWindows.h" = "8E7D4ED4F6599CA599514D5C1710668A61865F0C11C44B47994AEDE33F4CAC16"
}
$curl = Join-Path $env:SystemRoot "System32\curl.exe"
foreach ($e in $files.GetEnumerator()) {
    $dst = Join-Path $Out $e.Key
    if ((Test-Path $dst) -and (Get-FileHash $dst -Algorithm SHA256).Hash -eq $e.Value) { continue }
    & $curl -fsSL --retry 3 -o "$dst.part" "$base/$($e.Key)"
    if ($LASTEXITCODE -ne 0) { throw "download of $base/$($e.Key) failed ($LASTEXITCODE)" }
    $h = (Get-FileHash "$dst.part" -Algorithm SHA256).Hash
    if ($h -ne $e.Value) { Remove-Item "$dst.part"; throw "$($e.Key): SHA-256 $h instead of $($e.Value)" }
    Move-Item -Force "$dst.part" $dst
}
Write-Host "LZMA SDK 24.09 (LzmaDec) in $Out"
