# Checks that the repository holds no game data (CI job "game-data" of .github/workflows/ci.yml):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\check_game_data.ps1 [-Repo <folder>]
# Every tracked file of <Repo> (default: this repository; git ls-files, as checked out) is tested with the rules of
# tools/game_data_rules.ps1: no disc image or files from it, no XEX, no recompiled or generated code, no shader packs,
# no build output, nothing under the local data folders, no file above 20 MB and no binary file above 256 KB except
# the allowed ones (PGO profile and pipeline list, each with its header checked, and the SDK's synthetic test
# fixtures). Works with Windows PowerShell 5.1 and PowerShell 7.
# Exit code: 0 = nothing found, 1 = findings (listed as path: rule: detail), 2 = usage or tool error.
param([string]$Repo = "")
$ErrorActionPreference = "Stop"
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
if (-not (Test-Path -LiteralPath $Repo -PathType Container)) { Write-Host "check_game_data: no such folder: $Repo"; exit 2 }
$Repo = (Get-Item -LiteralPath $Repo).FullName
. (Join-Path $PSScriptRoot "game_data_rules.ps1")

# git prints the paths as UTF-8 (core.quotepath=off); read them as such
$encoding = [Console]::OutputEncoding
try {
    [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
    $paths = @(& git -C $Repo -c core.quotepath=off ls-files)
    $rc = $LASTEXITCODE
} finally {
    [Console]::OutputEncoding = $encoding
}
if ($rc -ne 0) { Write-Host "check_game_data: git ls-files failed in $Repo ($rc)"; exit 2 }

$findings = New-Object System.Collections.Generic.List[string]
$checked = 0
foreach ($rel in $paths) {
    if (-not $rel) { continue }
    $full = Join-Path $Repo ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        # tracked but not in the working tree (deleted, or a submodule): the path rules still apply
        foreach ($rx in $GameDataFolders, $GameDataExt, $GameDataBuildExt, $GameDataNames) {
            if ($rel -match $rx -and -not $GameDataAllowed.ContainsKey($rel)) { $findings.Add("${rel}: tracked path of game data or build output (not in the working tree)"); break }
        }
        continue
    }
    $checked++
    foreach ($x in (Get-GameDataFindings -Rel $rel -FullPath $full)) { $findings.Add("${rel}: $($x.rule): $($x.text)") }
}

Write-Host ("check_game_data: {0} tracked files checked, {1} allowed binary files, {2} findings" -f $checked, $GameDataAllowed.Count, $findings.Count)
if ($findings.Count -gt 0) {
    foreach ($l in $findings) {
        # an annotation per finding in a GitHub Actions run
        if ($env:GITHUB_ACTIONS -eq "true") { Write-Host "::error file=$($l.Split(':')[0])::$l" } else { Write-Host "  $l" }
    }
    Write-Host "check_game_data: game data, generated code or build output in the repository; remove it (see tools/game_data_rules.ps1)"
    exit 1
}
exit 0
