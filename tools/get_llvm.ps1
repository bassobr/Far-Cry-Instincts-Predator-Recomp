# Fetches the official LLVM release this project builds with, checks it against the pinned SHA-256 and extracts
# the needed parts to -Out (default: toolchain\llvm, gitignored):
#   powershell -File tools\get_llvm.ps1
# Result: -Out\bin\{clang, clang++, clang-cl, lld-link, llvm-lib, llvm-ar, llvm-mt, llvm-rc, llvm-profdata,
# llvm-objdump, llvm-nm, llvm-symbolizer}.exe, -Out\lib\clang\<major> (clang's headers and runtime libraries)
# and -Out\LICENSE.TXT.
# Derived from tools/get_llvm.ps1 of Far Cry Instincts Recomp (MIT, same author).
param(
    [string]$Out = "",
    [string]$Cache = ""  # folder that keeps the download between runs (default: -Out)
)
$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

# ---- the pinned release ----
$Version = "22.1.8"
$Archive = "clang+llvm-$Version-x86_64-pc-windows-msvc.tar.xz"
$ArchiveUrl = "https://github.com/llvm/llvm-project/releases/download/llvmorg-$Version/$Archive"
$ArchiveSha256 = "D96C2CC1736F4EB7FA43CB9BBDF56D93551A9AE0A9AADB9C99C3C3B2B712A234"
$LicenseUrl = "https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$Version/llvm/LICENSE.TXT"
$Major = $Version.Split(".")[0]

if (-not $Out) { $Out = Join-Path (Split-Path -Parent $PSScriptRoot) "toolchain\llvm" }
if (-not $Cache) { $Cache = $Out }
New-Item -ItemType Directory -Force $Out, $Cache | Out-Null

# curl and bsdtar from Windows (a tar from Git for Windows earlier in PATH would take C:\... for a host)
$curlExe = Join-Path $env:SystemRoot "System32\curl.exe"
$tarExe = Join-Path $env:SystemRoot "System32\tar.exe"
$clock = [Diagnostics.Stopwatch]::StartNew()
function Step([string]$Text) { Write-Host ("[{0,6:N1} s] {1}" -f $clock.Elapsed.TotalSeconds, $Text) }

$tar = Join-Path (Resolve-Path $Cache).Path $Archive
if (-not (Test-Path $tar) -or (Get-FileHash $tar -Algorithm SHA256).Hash -ne $ArchiveSha256) {
    Step "downloading $ArchiveUrl"
    & $curlExe -fsSL --retry 3 -o "$tar.part" $ArchiveUrl
    if ($LASTEXITCODE -ne 0) { throw "download of $ArchiveUrl failed ($LASTEXITCODE)" }
    Move-Item -Force "$tar.part" $tar
    Step ("downloaded: {0:N1} MB" -f ((Get-Item $tar).Length / 1MB))
}
$hash = (Get-FileHash $tar -Algorithm SHA256).Hash
if ($hash -ne $ArchiveSha256) { throw "$tar has SHA-256 $hash, expected $ArchiveSha256" }
Step "SHA-256 matches: $hash"
& $curlExe -fsSL --retry 3 -o (Join-Path $Out "LICENSE.TXT") $LicenseUrl
if ($LASTEXITCODE -ne 0) { throw "download of the license failed ($LASTEXITCODE)" }

$top = "clang+llvm-$Version-x86_64-pc-windows-msvc"
$tools = "clang", "clang++", "clang-cl", "lld-link", "llvm-lib", "llvm-ar", "llvm-mt", "llvm-rc", "llvm-profdata",
         "llvm-objdump", "llvm-nm", "llvm-symbolizer"
$members = @($tools | ForEach-Object { "$top/bin/$_.exe" }) + "$top/lib/clang/$Major"
$stage = Join-Path $Out "extract"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Step "extracting $($members.Count) entries with $tarExe"
& $tarExe -xf $tar -C $stage @members
if ($LASTEXITCODE -ne 0) { throw "extracting $tar failed ($LASTEXITCODE)" }
foreach ($dir in "bin", "lib") {
    if (Test-Path (Join-Path $Out $dir)) { Remove-Item -Recurse -Force (Join-Path $Out $dir) }
    Move-Item (Join-Path $stage "$top\$dir") (Join-Path $Out $dir)
}
Remove-Item -Recurse -Force $stage

$reported = & (Join-Path $Out "bin\clang.exe") --version | Select-Object -First 1
if ($reported -notmatch "clang version $([regex]::Escape($Version))\b") { throw "unexpected clang: $reported" }
Step "LLVM $Version in $Out ($reported)"
