<#
.SYNOPSIS
  Fetches the dependencies of the native renderer (src\renderer, tools\shaders) at pinned commits.

.DESCRIPTION
  The default target is <repo>\third_party\renderer-deps (gitignored). A different location can be set with
  -DepsDir or the environment variable FCP_RENDERER_DEPS_DIR; CMake reads the same location from the variable
  FCP_RENDERER_DEPS_DIR. None of it is committed or installed globally: no Vulkan SDK, no
  registry, no Git configuration. vulkan-1.dll comes with the graphics driver, d3d12.dll with the system.

  Git repositories are fetched with "git fetch --depth 1 <url> <commit>" into their own directory. DXC comes as a
  release zip whose SHA-256 is checked before it is extracted.

  | Name                 | Source                                  | Pin                                         | License |
  |----------------------|-----------------------------------------|---------------------------------------------|--------|
  | king-kong-recompiled | TekRantGaming/king-kong-recompiled      | e3e5d3f488cbdcd35afd0a1f08b958b0b50f03c3    | MIT; contains native-renderer/thirdparty/nvrhi (NVRHI 6b96fb03 with KK patches, MIT) and DirectX-Headers v1.717.0-preview (MIT) |
  | Vulkan-Headers       | KhronosGroup/Vulkan-Headers             | v1.4.352 = 015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9 (tag object eae54063) | Apache-2.0 OR MIT |
  | fmt                  | fmtlib/fmt                              | 873670ba3f9e7bc77ec2c1c94b04f1f8bef77e9f (XenosRecomp submodule) | MIT |
  | xxHash               | Cyan4973/xxHash                         | 2bf8313b934633b2a5b7e8fd239645b85e10c852 (XenosRecomp submodule) | BSD-2-Clause |
  | dxc                  | microsoft/DirectXShaderCompiler v1.9.2609 | dxc_2026_09_29.zip, SHA-256 below         | NCSA (LLVM) |

.PARAMETER DepsDir
  Target directory. Precedence: -DepsDir, then $env:FCP_RENDERER_DEPS_DIR, then <repo>\third_party\renderer-deps.

.PARAMETER Force
  Deletes and refetches a dependency even if it is already at the right version.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\get_renderer_deps.ps1
#>
param(
  [string]$DepsDir = "",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $DepsDir) { $DepsDir = $env:FCP_RENDERER_DEPS_DIR }
if (-not $DepsDir) { $DepsDir = Join-Path $repo "third_party\renderer-deps" }
$DepsDir = [System.IO.Path]::GetFullPath($DepsDir)

$gitDeps = @(
  @{ n = "king-kong-recompiled"; u = "https://github.com/TekRantGaming/king-kong-recompiled.git"; c = "e3e5d3f488cbdcd35afd0a1f08b958b0b50f03c3" },
  @{ n = "Vulkan-Headers";       u = "https://github.com/KhronosGroup/Vulkan-Headers.git";        c = "015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9" },
  @{ n = "fmt";                  u = "https://github.com/fmtlib/fmt.git";                         c = "873670ba3f9e7bc77ec2c1c94b04f1f8bef77e9f" },
  @{ n = "xxHash";               u = "https://github.com/Cyan4973/xxHash.git";                    c = "2bf8313b934633b2a5b7e8fd239645b85e10c852" }
)
$dxc = @{
  n = "dxc"
  u = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip"
  sha = "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1"
  check = "bin\x64\dxcompiler.dll"
}

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

New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null
Write-Host "renderer dependencies to $DepsDir"
$failed = @()
foreach ($d in $gitDeps) {
  $dir = Join-Path $DepsDir $d.n
  if ((Get-Head $dir) -eq $d.c -and -not $Force) {
    Write-Host ("  {0,-21} {1} present" -f $d.n, $d.c.Substring(0, 10))
    continue
  }
  try {
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    Invoke-Git @("init", "-q", $dir)
    Invoke-Git @("-C", $dir, "fetch", "-q", "--depth", "1", $d.u, $d.c)
    Invoke-Git @("-C", $dir, "-c", "advice.detachedHead=false", "checkout", "-q", "FETCH_HEAD")
    $head = Get-Head $dir
    if ($head -ne $d.c) { throw "HEAD $head instead of $($d.c)" }
    Write-Host ("  {0,-21} {1} fetched" -f $d.n, $d.c.Substring(0, 10))
  } catch {
    Write-Warning "$($d.n): $_"
    $failed += $d.n
  }
}

# DXC: download the zip, check SHA-256, extract. The zip is kept as evidence.
$dxcDir = Join-Path $DepsDir $dxc.n
$dxcZip = Join-Path $DepsDir "dxc_2026_09_29.zip"
if ((Test-Path (Join-Path $dxcDir $dxc.check)) -and -not $Force) {
  Write-Host ("  {0,-21} v1.9.2609 present" -f $dxc.n)
} else {
  try {
    if (-not (Test-Path $dxcZip) -or $Force) {
      $ProgressPreference = "SilentlyContinue"
      Invoke-WebRequest -UseBasicParsing -Uri $dxc.u -OutFile $dxcZip
    }
    $sha = (Get-FileHash -Algorithm SHA256 $dxcZip).Hash.ToLowerInvariant()
    if ($sha -ne $dxc.sha) { Remove-Item -Force $dxcZip; throw "SHA-256 $sha instead of $($dxc.sha)" }
    if (Test-Path $dxcDir) { Remove-Item -Recurse -Force $dxcDir }
    Expand-Archive -Path $dxcZip -DestinationPath $dxcDir
    if (-not (Test-Path (Join-Path $dxcDir $dxc.check))) { throw "$($dxc.check) missing after extraction" }
    Write-Host ("  {0,-21} v1.9.2609 fetched (SHA-256 ok)" -f $dxc.n)
  } catch {
    Write-Warning "dxc: $_"
    $failed += "dxc"
  }
}

if ($failed.Count) {
  Write-Error ("failed: " + ($failed -join ", "))
  exit 1
}
Write-Host "done"
exit 0
