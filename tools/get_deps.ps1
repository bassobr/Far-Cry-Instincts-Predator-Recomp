<#
.SYNOPSIS
  Fetches the dependencies (former Git submodules) of the frozen ReXGlue copy under rexglue\ at
  pinned commits.

.DESCRIPTION
  The default target is <repo>\third_party\rexglue-deps (gitignored). A different location can be set with
  -DepsDir or the environment variable FCP_DEPS_DIR; the build reads the same location from the CMake variable
  FCP_DEPS_DIR. Each dependency is fetched with "git fetch --depth 1 <url> <commit>" into its own directory;
  neither the global nor the repository Git configuration is changed, and nothing is written to the
  CMake user registry (this script does not start CMake at all).

  Git symlinks: on Windows without the symlink privilege, Git checks out a symlink as a small text file
  containing the target path. The script replaces every such placeholder file with a copy of its target
  (affects libmspack: cabextract\mspack\*). Real symlinks (reparse points) are left untouched.

  The pins match the submodule entries of rexglue-sdk 351bc94d (see rexglue\UPSTREAM.md).
  If the SDK version changes, the pins here must be updated along with it.

.PARAMETER DepsDir
  Target directory. Precedence: -DepsDir, then $env:FCP_DEPS_DIR, then <repo>\third_party\rexglue-deps.

.PARAMETER Optional
  Also fetches the dependencies our build does not need: glslang (Xenos Vulkan driver only)
  and catch2 (SDK unit tests only).

.PARAMETER Force
  Deletes and refetches a dependency even if it is already at the right commit.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\get_deps.ps1
#>
param(
  [string]$DepsDir = "",
  [switch]$Optional,
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $DepsDir) { $DepsDir = $env:FCP_DEPS_DIR }
if (-not $DepsDir) { $DepsDir = Join-Path $repo "third_party\rexglue-deps" }
$DepsDir = [System.IO.Path]::GetFullPath($DepsDir)

# Name (= directory under $DepsDir, as under rexglue-sdk/thirdparty), URL, commit, required
$deps = @(
  @{ n = "cli11";        u = "https://github.com/CLIUtils/CLI11";               c = "bfffd37e1f804ca4fae1caae106935791696b6a9"; r = $true },
  @{ n = "libmspack";    u = "https://github.com/kyz/libmspack.git";            c = "305907723a4e7ab2018e58040059ffb5e77db837"; r = $true },
  @{ n = "excrypt";      u = "https://github.com/emoose/ExCrypt.git";           c = "088094585aee1694cdeef9a9330d29d14aef9203"; r = $true },
  @{ n = "FFmpeg";       u = "https://github.com/wmarti/FFmpeg.git";            c = "0604b464c7cb4ebc94940cf1f324a3b26b87717c"; r = $true },
  @{ n = "tomlplusplus"; u = "https://github.com/marzer/tomlplusplus.git";      c = "30172438cee64926dc41fdd9c11fb3ba5b2ba9de"; r = $true },
  @{ n = "simde";        u = "https://github.com/simd-everywhere/simde.git";    c = "71fd833d9666141edcd1d3c109a80e228303d8d7"; r = $true },
  @{ n = "xxHash";       u = "https://github.com/Cyan4973/xxHash.git";          c = "e626a72bc2321cd320e953a0ccf1584cad60f363"; r = $true },
  @{ n = "spdlog";       u = "https://github.com/gabime/spdlog.git";            c = "79524ddd08a4ec981b7fea76afd08ee05f83755d"; r = $true },
  @{ n = "imgui";        u = "https://github.com/ocornut/imgui.git";            c = "6d910d5487d11ca567b61c7824b0c78c569d62f0"; r = $true },
  @{ n = "sdl3";         u = "https://github.com/libsdl-org/SDL";               c = "147a8ee32dbf9ac02f3794964490687b6bbda1bc"; r = $true },
  @{ n = "inja";         u = "https://github.com/pantor/inja.git";              c = "7d1b4600b68595085a949743331c2e5673f511ea"; r = $true },
  @{ n = "tlsf";         u = "https://github.com/mattconte/tlsf.git";           c = "deff9ab509341f264addbd3c8ada533678591905"; r = $true },
  @{ n = "tracy";        u = "https://github.com/wolfpld/tracy.git";            c = "05cceee0df3b8d7c6fa87e9638af311dbabc63cb"; r = $true },
  @{ n = "glslang";      u = "https://github.com/KhronosGroup/glslang.git";     c = "f4f1d8a352ca1908943aea2ad8c54b39b4879080"; r = $false },
  @{ n = "catch2";       u = "https://github.com/catchorg/Catch2.git";          c = "88abf9bf325c798c33f54f6b9220ef885b267f4f"; r = $false }
)

function Invoke-Git {
  param([string[]]$GitArgs)
  & git @GitArgs
  if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed ($LASTEXITCODE)" }
}

function Get-Head([string]$dir) {
  if (-not (Test-Path (Join-Path $dir ".git"))) { return "" }
  $h = & git -C $dir rev-parse HEAD 2>$null
  if ($LASTEXITCODE -ne 0) { return "" }
  return "$h".Trim()
}

# Replaces placeholder files of Git symlinks with copies of their targets.
function Repair-Symlinks([string]$dir) {
  $fixed = 0
  $entries = & git -C $dir ls-files -s
  if ($LASTEXITCODE -ne 0) { throw "git ls-files in $dir failed" }
  foreach ($line in $entries) {
    if (-not $line.StartsWith("120000 ")) { continue }
    $rel = $line.Substring($line.IndexOf("`t") + 1)
    $link = Join-Path $dir ($rel -replace "/", "\")
    $item = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
    if ($null -eq $item) { continue }
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { continue }  # real symlink
    if ($item.PSIsContainer) { continue }  # already replaced by the target directory
    $target = ([System.IO.File]::ReadAllText($link)).Trim()
    $src = [System.IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $link) ($target -replace "/", "\")))
    if (Test-Path -LiteralPath $src -PathType Leaf) {
      Copy-Item -LiteralPath $src -Destination $link -Force
    } elseif (Test-Path -LiteralPath $src -PathType Container) {
      Remove-Item -LiteralPath $link -Force
      Copy-Item -LiteralPath $src -Destination $link -Recurse -Force
    } else {
      Write-Warning "symlink target missing: $rel -> $target"
      continue
    }
    $fixed++
  }
  return $fixed
}

New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null
Write-Host "dependencies to $DepsDir"
$failed = @()
foreach ($d in $deps) {
  if (-not $d.r -and -not $Optional) { continue }
  $dir = Join-Path $DepsDir $d.n
  $head = Get-Head $dir
  if ($head -eq $d.c -and -not $Force) {
    $n = Repair-Symlinks $dir
    Write-Host ("  {0,-13} {1} present{2}" -f $d.n, $d.c.Substring(0, 10), $(if ($n) { ", $n symlinks replaced" } else { "" }))
    continue
  }
  try {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    Invoke-Git @("init", "-q", $dir)
    Invoke-Git @("-C", $dir, "fetch", "-q", "--depth", "1", $d.u, $d.c)
    Invoke-Git @("-C", $dir, "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD")
    $head = Get-Head $dir
    if ($head -ne $d.c) { throw "HEAD $head instead of $($d.c)" }
    $n = Repair-Symlinks $dir
    Write-Host ("  {0,-13} {1} fetched{2}" -f $d.n, $d.c.Substring(0, 10), $(if ($n) { ", $n symlinks replaced" } else { "" }))
  } catch {
    Write-Warning "$($d.n): $_"
    $failed += $d.n
  }
}
if ($failed.Count) {
  Write-Error ("failed: " + ($failed -join ", "))
  exit 1
}
Write-Host "done"
exit 0
