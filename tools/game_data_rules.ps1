# Rules that keep game data out of the repository. Dot-source this file and call Get-GameDataFindings per file:
#   . "$PSScriptRoot\game_data_rules.ps1"
#   foreach ($x in (Get-GameDataFindings -Rel "src/main.cpp" -FullPath "C:\...\src\main.cpp")) { $x.rule; $x.text }
# Used by tools/check_game_data.ps1 (the CI job "game-data" of .github/workflows/ci.yml, every tracked file) and by
# the scan of the public export, so both apply the same rules.
#
# Never in the repository: the disc image or files from it, the XEX or its decrypted image, recompiled or generated
# game code, shader packs, caches, frame dumps, build output, and the folders that hold them locally (assets/, work/,
# generated/, out/, toolchain/, third_party/, build/, screenshots/). Binary files above $GameDataBinaryMaxBytes are
# refused unless they are listed in $GameDataAllowed, which also fixes the header and the size limit of each.
# Rule ids (the export's allow list refers to them): game-folder, game-file, game-code, build-output, generated-code,
# large-file, binary-file, rexguest, pgo-profile, pipeline-list.

$GameDataFolders = '^(assets|work|generated|out|toolchain|third_party|build|screenshots)/'
$GameDataExt = '(?i)\.(xex|xexp|iso|xiso|bin|fat|dat|fcpack|fcse|bik|xma|xmv|stfs|god|profraw|profdata|spv|dxil|fcpl)$'
$GameDataBuildExt = '(?i)\.(exe|dll|lib|obj|o|a|elf|pdb|exp|ilk|pch|ninja_log|ninja_deps)$'
# predator_code.dll and its siblings; the source names the codegen writes into generated/
$GameDataNames = '(?i)(^|/)(predator_code\.[^/]+|predator_(funcs|recomp|init|register|pch)[^/]*)$'
$GameDataMaxBytes = 20MB
$GameDataBinaryMaxBytes = 256KB
# one line per recompiled guest function in generated code; a few in tests and templates are fine
$GameDataGeneratedMarker = New-Object regex('DEFINE_REX_FUNC\(\s*sub_[0-9A-Fa-f]{8}|PPC_FUNC_IMPL\(\s*__imp__sub_[0-9A-Fa-f]{8}')
$GameDataGeneratedMax = 16

# The binary files that belong in the repository (path relative to the root, forward slashes).
$GameDataAllowed = @{
    "config/predator/game_code.profdata"             = @{
        rule = "pgo-profile"; max = 20MB; head = [byte[]](0xFF, 0x6C, 0x70, 0x72, 0x6F, 0x66, 0x69, 0x81)
        bad  = "(not an indexed LLVM profile)"
        why  = "PGO profile of the game code (function names, structure hashes, execution counters, no code), shipped on purpose"
    }
    "config/predator/pipelines.fcpl"                 = @{
        rule = "pipeline-list"; max = 4MB; head = [byte[]](0x46, 0x43, 0x50, 0x4C, 1, 0, 0, 0)
        bad  = "(not a pipeline list version 1)"
        why  = "shipped pipeline list (shader container hashes and render state values, no code), shipped on purpose"
    }
    "rexglue/tests/fixtures/filesystem/gdfx.iso"     = @{
        rule = "game-file"; max = 256KB; head = $null; bad = ""
        why  = "synthetic test image of the SDK unit tests, no game data"
    }
    "rexglue/tests/fixtures/filesystem/stfs_con.bin" = @{
        rule = "game-file"; max = 256KB; head = $null; bad = ""
        why  = "synthetic STFS test package of the SDK unit tests, no game data"
    }
}

# Section names of a PE image (empty for anything else); only the headers are read.
function Get-GameDataPeSections([byte[]]$b) {
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return @() }
    $nt = [BitConverter]::ToInt32($b, 0x3C)
    if ($nt -lt 0 -or $nt + 24 -gt $b.Length -or $b[$nt] -ne 0x50 -or $b[$nt + 1] -ne 0x45) { return @() }
    $count = [BitConverter]::ToUInt16($b, $nt + 6)
    $opt = [BitConverter]::ToUInt16($b, $nt + 20)
    $at = $nt + 24 + $opt
    $names = @()
    for ($i = 0; $i -lt $count -and $at + 40 * $i + 8 -le $b.Length; $i++) {
        $names += ([Text.Encoding]::ASCII.GetString($b, $at + 40 * $i, 8)).TrimEnd([char]0)
    }
    return $names
}

# Findings for one file: objects with "rule" and "text" (nothing when the file is fine).
function Get-GameDataFindings([string]$Rel, [string]$FullPath) {
    $found = New-Object System.Collections.Generic.List[object]
    $Rel = $Rel -replace '\\', '/'
    $len = (Get-Item -LiteralPath $FullPath -Force).Length
    $allowed = $null
    if ($GameDataAllowed.ContainsKey($Rel)) { $allowed = $GameDataAllowed[$Rel] }

    if ($Rel -match $GameDataFolders) { $found.Add([pscustomobject]@{ rule = "game-folder"; text = "(local data folder) $Rel" }) }
    if (-not $allowed) {
        if ($Rel -match $GameDataExt) { $found.Add([pscustomobject]@{ rule = "game-file"; text = "(file type) $Rel" }) }
        if ($Rel -match $GameDataBuildExt) { $found.Add([pscustomobject]@{ rule = "build-output"; text = "(file type) $Rel" }) }
        if ($Rel -match $GameDataNames) { $found.Add([pscustomobject]@{ rule = "game-code"; text = "(file name) $Rel" }) }
    }
    if ($len -gt $GameDataMaxBytes) {
        $found.Add([pscustomobject]@{ rule = "large-file"; text = ("(size) {0:N1} MB" -f ($len / 1MB)) })
    }
    if ($allowed -and $len -gt $allowed.max) {
        $found.Add([pscustomobject]@{ rule = $allowed.rule; text = ("(larger than the {0:N0} KB allowed for this file: {1:N0} KB)" -f ($allowed.max / 1KB), ($len / 1KB)) })
    }

    # the first 64 KB: PE headers, the header of an allowed file, the binary probe
    $n = [int][Math]::Min($len, 65536)
    $head = New-Object byte[] $n
    if ($n -gt 0) {
        $fs = [IO.File]::OpenRead($FullPath)
        try { $got = 0; while ($got -lt $n) { $r = $fs.Read($head, $got, $n - $got); if ($r -le 0) { break }; $got += $r } }
        finally { $fs.Close() }
    }
    if ($allowed -and $allowed.head) {
        $ok = $n -ge $allowed.head.Length
        for ($i = 0; $ok -and $i -lt $allowed.head.Length; $i++) { if ($head[$i] -ne $allowed.head[$i]) { $ok = $false } }
        if (-not $ok) { $found.Add([pscustomobject]@{ rule = $allowed.rule; text = $allowed.bad }) }
    }
    if ((Get-GameDataPeSections $head) -contains "rexguest") {
        $found.Add([pscustomobject]@{ rule = "rexguest"; text = "(PE section rexguest: recompiled game code)" })
    }
    $binary = $false
    $probe = [Math]::Min($n, 8000)
    for ($i = 0; $i -lt $probe; $i++) { if ($head[$i] -eq 0) { $binary = $true; break } }
    if ($binary -and -not $allowed -and $len -gt $GameDataBinaryMaxBytes) {
        $found.Add([pscustomobject]@{ rule = "binary-file"; text = ("(binary file of {0:N0} KB, not on the list of allowed files)" -f ($len / 1KB)) })
    }
    if (-not $binary -and $len -le $GameDataMaxBytes) {
        $text = [IO.File]::ReadAllText($FullPath)
        $count = $GameDataGeneratedMarker.Matches($text).Count
        if ($count -gt $GameDataGeneratedMax) {
            $found.Add([pscustomobject]@{ rule = "generated-code"; text = "($count recompiled guest functions: generated game code)" })
        }
    }
    return $found.ToArray()
}
