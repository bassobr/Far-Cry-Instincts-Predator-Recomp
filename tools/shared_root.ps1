# Shared paths for the test and measurement scripts (dot-source: . "$PSScriptRoot\shared_root.ps1").
#
# Get-FcpSharedRoot: the checkout that holds the shared, read-only inputs (toolchain\, third_party\, assets\,
#   work\). FCP_SHARED_ROOT if set; in a Git worktree the main checkout (parent of "git rev-parse
#   --git-common-dir"); otherwise, or without Git, this repository itself.
# Get-FcpGameIso: the disc image for test runs. FCP_GAME_ISO if set, else the image under <shared root>\assets.
$script:FcpRepoRoot = Split-Path -Parent $PSScriptRoot
$script:FcpIsoName = "Far Cry Instincts - Predator (USA, Europe) (En,Fr,De,Es,It).iso"

function Get-FcpSharedRoot {
    if ($env:FCP_SHARED_ROOT) { return $env:FCP_SHARED_ROOT }
    $common = $null
    try { $common = (& git -C $script:FcpRepoRoot rev-parse --path-format=absolute --git-common-dir 2>$null) } catch { }
    if ($LASTEXITCODE -eq 0 -and $common) {
        return [IO.Path]::GetFullPath((Join-Path ("$common".Trim()) ".."))
    }
    return $script:FcpRepoRoot
}

function Get-FcpGameIso {
    if ($env:FCP_GAME_ISO) { return $env:FCP_GAME_ISO }
    return Join-Path (Get-FcpSharedRoot) "assets\$script:FcpIsoName"
}
