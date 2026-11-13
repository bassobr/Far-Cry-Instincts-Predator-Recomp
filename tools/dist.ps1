# Assembles the release folder (and its zip with SHA256SUMS): predator.exe without game code, the runtime DLLs,
# the shader tool and the compiler folder with which predator.exe builds the game code from the player's disc image
# on the first start (src/fcp/game_code.cpp). Needs no game data (CI):
#   powershell -File tools\get_llvm.ps1 -Out C:\llvm
#   powershell -File tools\dist.ps1 -Llvm C:\llvm [-Build out\build\release] [-Deps third_party\rexglue-deps]
#                                   [-Out out\dist] [-Name <name>] [-Zip] [-Sources[:$false]] [-NoVcRuntime]
# -Sources also writes the source archive of the LGPL/GPL components next to it (default: on with -Zip).
# -Build is a build of the release preset, with or without disc image (bin\game_code is never packaged).
# Refuses everything that comes from the game: predator_code.dll, generated code, objects, PDBs, .xex/.iso, shader packs,
# shader/pipeline caches, save games, logs, portable.txt and every EXE/DLL with the section rexguest.
# Derived from tools/dist.ps1 of Far Cry Instincts Recomp (MIT, same author).
param(
    [Parameter(Mandatory = $true)][string]$Llvm,  # tools\get_llvm.ps1 -Out: bin\clang.exe, bin\lld-link.exe, lib\clang\<major>
    [string]$Build = "",
    [string]$Deps = "",
    [string]$Out = "",
    [string]$Name = "",
    [switch]$Zip,
    [switch]$Sources,     # source archive of the LGPL/GPL components (default: on with -Zip; -Sources:$false = off)
    [switch]$NoVcRuntime  # without the VC++ runtime DLLs (the player then needs the VC++ Redistributable)
)
$ErrorActionPreference = "Stop"
$makeSources = if ($PSBoundParameters.ContainsKey("Sources")) { [bool]$Sources } else { [bool]$Zip }
$root = Split-Path -Parent $PSScriptRoot
if (-not $Build) { $Build = Join-Path $root "out\build\release" }
if (-not $Deps) { $Deps = Join-Path $root "third_party\rexglue-deps" }
if (-not $Out) { $Out = Join-Path $root "out\dist" }
$Build = (Resolve-Path $Build).Path
$Llvm = (Resolve-Path $Llvm).Path
$version = if (Test-Path (Join-Path $root "VERSION")) { (Get-Content -Raw (Join-Path $root "VERSION")).Trim() } else { "dev" }
if (-not $Name) { $Name = "Far-Cry-Instincts-Predator-Recomp-v$version-win64" }
$LlvmMajor = "22"  # tools\get_llvm.ps1 pins 22.1.8; the game code flags and the ABI depend on this version
$bin = Join-Path $Build "bin"
$stage = Join-Path $Out $Name

# ---- what belongs in the release, nothing else: target (relative to the release folder) -> source ----
$files = [ordered]@{}
$files["predator.exe"] = Join-Path $bin "predator.exe"
# SDK DLLs that predator.exe loads (rexglue_configure_target), FFmpeg (LGPL, separate DLL), SDL3
$runtime = "rexapp", "rexapu", "rexapu-sdl3", "rexbase", "rexfilesystem", "rexgpu", "rexgpu-xenos-null", "rexhid",
    "rexhid-sdl3", "rexhid-xinput", "rexkernel", "rexppc", "rexui", "rexui-sdl3", "rexxam", "rexxbdm", "rexffmpeg", "SDL3"
foreach ($d in $runtime) { $files["$d.dll"] = Join-Path $bin "$d.dll" }
# Renderer: DXC (runtime compilation and shader pack), layout file, shader packer for the first start
$files["dxcompiler.dll"] = Join-Path $bin "dxcompiler.dll"
if (Test-Path (Join-Path $bin "dxil.dll")) { $files["dxil.dll"] = Join-Path $bin "dxil.dll" }
$files["fcshaders.exe"] = Join-Path $bin "fcshaders.exe"
$files["renderer.toml"] = Join-Path $root "config\predator\renderer.toml"
# Compiler folder: clang, lld-link, codegen, headers, import libraries, configuration, DLL entry point
$files["compiler\bin\clang.exe"] = Join-Path $Llvm "bin\clang.exe"
$files["compiler\bin\lld-link.exe"] = Join-Path $Llvm "bin\lld-link.exe"
$files["compiler\bin\rexglue.exe"] = Join-Path $bin "rexglue.exe"
$files["compiler\LICENSE-LLVM.txt"] = Join-Path $Llvm "LICENSE.TXT"
$files["compiler\lib\rexppc.lib"] = Join-Path $bin "rexppc.lib"
$files["compiler\lib\rexbase.lib"] = Join-Path $bin "rexbase.lib"
foreach ($def in Get-ChildItem (Join-Path $root "src\fcp\game_code") -Filter *.def) {
    $files["compiler\lib\$($def.BaseName).lib"] = Join-Path $Build "game_code_libs\$($def.BaseName).lib"
}
$files["compiler\src\entry.cpp"] = Join-Path $root "src\fcp\game_code\entry.cpp"
$files["compiler\src\runtime.cpp"] = Join-Path $root "src\fcp\game_code\runtime.cpp"
$files["compiler\include\fcp\game_code_abi.h"] = Join-Path $root "src\fcp\game_code_abi.h"
$files["compiler\config\default.toml"] = Join-Path $root "config\predator\default.toml"
$files["compiler\config\disc.toml"] = Join-Path $root "config\predator\disc.toml"
$files["compiler\config\d3d_names.toml"] = Join-Path $root "generated\d3d_names.toml"  # only from d3d_symbols.toml
# Headers: SDK (BSD-3, with rex\guest_std), SIMDe (MIT), clang's own headers (Apache-2.0 WITH LLVM-exception)
foreach ($h in Get-ChildItem (Join-Path $root "rexglue\include") -Recurse -File) {
    $rel = $h.FullName.Substring((Join-Path $root "rexglue\include").Length + 1)
    if ($rel -match '\.in$') { continue }
    $files["compiler\include\$rel"] = $h.FullName
}
$simde = Join-Path $Deps "simde\simde"
foreach ($h in Get-ChildItem $simde -Recurse -File -Include *.h) {
    $files["compiler\include\simde\simde\" + $h.FullName.Substring($simde.Length + 1)] = $h.FullName
}
$files["compiler\include\simde\COPYING"] = Join-Path $Deps "simde\COPYING"
$clangInc = Join-Path $Llvm "lib\clang\$LlvmMajor\include"
foreach ($h in Get-ChildItem $clangInc -Recurse -File) {
    $files["compiler\lib\clang\$LlvmMajor\include\" + $h.FullName.Substring($clangInc.Length + 1)] = $h.FullName
}
# Documents (English)
foreach ($doc in "README.md", "LICENSE", "THIRD_PARTY_NOTICES.md") {
    $files[$doc] = Join-Path $root "dist\$doc"
}
$files["VERSION"] = Join-Path $root "VERSION"
foreach ($lic in Get-ChildItem (Join-Path $root "dist\licenses") -File) { $files["licenses\$($lic.Name)"] = $lic.FullName }
# VC++ runtime (Distributable Code per redist.txt of the Build Tools), so the player needs no Redistributable
if (-not $NoVcRuntime) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    $redist = Get-ChildItem (Join-Path $vs "VC\Redist\MSVC") -Directory | Where-Object { $_.Name -match '^\d' } |
        Sort-Object { [version]$_.Name } | Select-Object -Last 1
    $crt = Join-Path $redist.FullName "x64\Microsoft.VC143.CRT"
    foreach ($d in "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll") {
        $files[$d] = Join-Path $crt $d
    }
}
foreach ($src in $files.Values) {
    if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { throw "missing $src (build the release preset, tools\get_llvm.ps1)" }
}

# ---- checks before copying ----
# Every license text that THIRD_PARTY_NOTICES.md names is in licenses\ (e.g. DearImGui-MIT.txt for the
# settings menu); mandatory texts even when not named.
$notices = [IO.File]::ReadAllText((Join-Path $root "dist\THIRD_PARTY_NOTICES.md"))
$named = @([regex]::Matches($notices, '`([A-Za-z0-9.+-]+\.(txt|md))`') | ForEach-Object { $_.Groups[1].Value } |
    Where-Object { $_ -notin "THIRD_PARTY_NOTICES.md", "README.md", "LICENSE-LLVM.txt", "HOW_TO_START.txt", "portable.txt", "BUILDING.txt" } |
    Sort-Object -Unique)
$missingLic = @(($named + "DearImGui-MIT.txt", "NVRHI-MIT.txt", "SDL3-zlib.txt", "LGPL-2.1.txt") | Sort-Object -Unique |
    Where-Object { -not (Test-Path -LiteralPath (Join-Path $root "dist\licenses\$_") -PathType Leaf) })
if ($missingLic) { throw "license texts named in THIRD_PARTY_NOTICES.md missing in dist\licenses: $($missingLic -join ', ')" }
$reported = & (Join-Path $Llvm "bin\clang.exe") --version | Select-Object -First 1
if ($reported -notmatch "clang version $LlvmMajor\.") { throw "bundled clang is '$reported', the game code needs LLVM $LlvmMajor" }

# PE sections of a file (for: no EXE/DLL with generated code, section "rexguest")
function Get-Sections([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { return @() }
    $nt = [BitConverter]::ToInt32($b, 0x3C)
    $count = [BitConverter]::ToUInt16($b, $nt + 6)
    $opt = [BitConverter]::ToUInt16($b, $nt + 20)
    $at = $nt + 24 + $opt
    $names = @()
    for ($i = 0; $i -lt $count; $i++) {
        $names += ([Text.Encoding]::ASCII.GetString($b, $at + 40 * $i, 8)).TrimEnd([char]0)
    }
    return $names
}

if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
foreach ($entry in $files.GetEnumerator()) {
    $dst = Join-Path $stage $entry.Key
    New-Item -ItemType Directory -Force (Split-Path -Parent $dst) | Out-Null
    Copy-Item -LiteralPath $entry.Value -Destination $dst
}
$stage = (Get-Item -LiteralPath $stage).FullName
$assetsNote = "Set the path of your disc image (.iso) in predator.toml ([game] image), in the user folder " +
    "%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp (created on the first start).`r`n" +
    "The first start then prepares the game for this PC once (game code and shaders, a few minutes).`r`n"
[IO.File]::WriteAllText((Join-Path $stage "HOW_TO_START.txt"), $assetsNote)

# ---- never the game or anything from it ----
$allowedDll = @($runtime | ForEach-Object { "$_.dll" }) + "dxcompiler.dll", "dxil.dll", "vcruntime140.dll",
    "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_atomic_wait.dll"
$problems = @()
foreach ($item in Get-ChildItem -LiteralPath $stage -Recurse -Force) {
    $rel = $item.FullName.Substring($stage.Length + 1)
    $n = $item.Name
    if ($item.PSIsContainer) {
        if ($n -match '^(game_code|generated|gen|work|obj|cache|user|logs|shaders|pack|default)$' -and
            $rel -notmatch '^compiler\\(include|lib)\\') { $problems += "$rel (folder)" }
        continue
    }
    if ($n -match '\.(xex|iso|xiso|bin|fat|dat|pdb|obj|o|ilk|exp|profraw|profdata|fcpack|spv|dxil|pipeline|log|prev|tmp)$') { $problems += $rel }
    if ($n -match '^(portable\.txt|predator\.toml|predator_input\.toml|rexglue\.toml|predator_code\.(dll|lib|def)|sources\.cmake|codegen\..*)$') { $problems += $rel }
    if ($n -match '^predator_(funcs|recomp|init|register|pch)') { $problems += $rel }
    if ($n -match '\.cpp$' -and $rel -notin "compiler\src\entry.cpp", "compiler\src\runtime.cpp") { $problems += $rel }
    if ($n -match '\.dll$' -and $allowedDll -notcontains $n) { $problems += "$rel (DLL not on the list)" }
    if ($n -match '\.(exe|dll)$') {
        $sections = Get-Sections $item.FullName
        if ($sections -contains "rexguest") { $problems += "$rel (has the section rexguest: recompiled code)" }
    }
}
$exe = Get-Item (Join-Path $stage "predator.exe")
if ($exe.Length -gt 40MB) { $problems += "predator.exe is $([int]($exe.Length / 1MB)) MB: it looks like it contains game code" }
if ($problems) { throw "refusing to package:`n  " + ($problems -join "`n  ") }
$packed = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
if ($packed.Count -ne $files.Count + 1) { throw "expected $($files.Count + 1) files, found $($packed.Count)" }

$size = ($packed | Measure-Object Length -Sum).Sum
Write-Host ("release folder: {0} ({1:N1} MB, {2} files, bundled {3})" -f $stage, ($size / 1MB), $packed.Count, $reported)
$groups = $packed | Group-Object { $r = $_.FullName.Substring($stage.Length + 1); if ($r -match '^(compiler\\lib\\clang|compiler\\[^\\]+\\)') { $Matches[1] } elseif ($r -match '^licenses\\') { "licenses\" } else { $r } }
foreach ($g in $groups | Sort-Object Name) {
    Write-Host ("  {0,-46} {1,5} files {2,10:N1} MB" -f $g.Name, $g.Count, (($g.Group | Measure-Object Length -Sum).Sum / 1MB))
}

if ($Zip) {
    $zipFile = Join-Path $Out "$Name.zip"
    Remove-Item -Force $zipFile -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::Open($zipFile, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $packed) {
            $entry = "$Name/" + ($f.FullName.Substring($stage.Length + 1) -replace '\\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $archive.Dispose()
    }
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $zipFile).Hash.ToLowerInvariant()
    $sums = "$hash  $Name.zip`n"
    Write-Host ("zip: {0} ({1:N1} MB), sha256 {2}" -f $zipFile, ((Get-Item $zipFile).Length / 1MB), $hash)
}

# ---- source archive of the LGPL/GPL components (-Sources, default on with -Zip) ----
# <Name with -win64 replaced by -sources>.zip: the exact sources of what the release ships under the LGPL/GPL, each
# from its pinned commit (tools\get_deps.ps1) or from this repository: FFmpeg (rexffmpeg.dll) with the build recipe
# and the list overlay, libmspack (rexfilesystem.dll), the GNU binutils PowerPC disassembler (rexglue.exe), the
# license texts and BUILDING.txt. Written straight into the zip (git archive --format=zip, entries copied over), so
# symbolic links of the source trees need no privilege on Windows.
$sourcesZip = $null
if ($makeSources) {
    $srcName = if ($Name -match '-win64$') { $Name -replace '-win64$', '-sources' } else { "$Name-sources" }
    $sourcesZip = Join-Path $Out "$srcName.zip"
    $depsText = [IO.File]::ReadAllText((Join-Path $root "tools\get_deps.ps1"))
    function Get-Pin([string]$dep) {
        $m = [regex]::Match($depsText, 'n = "' + [regex]::Escape($dep) + '";[^\r\n]*?u = "([^"]+)";[^\r\n]*?c = "([0-9a-f]{40})"')
        if (-not $m.Success) { throw "no pin for $dep in tools\get_deps.ps1" }
        return @{ url = $m.Groups[1].Value; commit = $m.Groups[2].Value }
    }
    $projectCommit = "unknown"
    try { $c = (& git -C $root rev-parse HEAD 2>$null); if ($LASTEXITCODE -eq 0 -and $c) { $projectCommit = "$c".Trim() } } catch { }
    New-Item -ItemType Directory -Force $Out | Out-Null
    Remove-Item -Force $sourcesZip -ErrorAction SilentlyContinue
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    $srcArchive = [IO.Compression.ZipFile]::Open($sourcesZip, [IO.Compression.ZipArchiveMode]::Create)
    $pins = [ordered]@{}
    try {
        function Add-SourceFile([string]$path, [string]$entry) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "missing $path" }
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($srcArchive, $path, "$srcName/$entry",
                [IO.Compression.CompressionLevel]::Optimal)
        }
        foreach ($dep in @(@{ n = "FFmpeg"; d = "ffmpeg/source" }, @{ n = "libmspack"; d = "libmspack/source" })) {
            $repo = Join-Path $Deps $dep.n
            $pin = Get-Pin $dep.n
            $ok = $null
            try { $ok = (& git -C $repo rev-parse -q --verify "$($pin.commit)^{commit}" 2>$null) } catch { }
            if (-not $ok) { throw "$repo does not hold the pinned commit $($pin.commit) (tools\get_deps.ps1)" }
            $tmp = Join-Path $Out "$srcName-$($dep.n).tmp.zip"
            Remove-Item -Force $tmp -ErrorAction SilentlyContinue
            & git -C $repo archive --format=zip -o $tmp $pin.commit
            if ($LASTEXITCODE -ne 0) { throw "git archive failed for $repo" }
            $in = [IO.Compression.ZipFile]::OpenRead($tmp)
            try {
                foreach ($e in $in.Entries) {
                    if ($e.FullName.EndsWith("/")) { continue }
                    $o = $srcArchive.CreateEntry("$srcName/$($dep.d)/$($e.FullName)", [IO.Compression.CompressionLevel]::Optimal)
                    $o.LastWriteTime = $e.LastWriteTime
                    $rs = $e.Open(); $ws = $o.Open()
                    try { $rs.CopyTo($ws) } finally { $ws.Dispose(); $rs.Dispose() }
                }
            } finally {
                $in.Dispose()
            }
            Remove-Item -Force $tmp
            $pins[$dep.n] = $pin
        }
        foreach ($f in Get-ChildItem (Join-Path $root "rexglue\thirdparty\disasm") -File) {
            Add-SourceFile $f.FullName "binutils-disasm/source/$($f.Name)"
        }
        Add-SourceFile (Join-Path $root "rexglue\thirdparty\CMakeLists.txt") "build-recipe/rexglue-thirdparty-CMakeLists.txt"
        foreach ($f in Get-ChildItem (Join-Path $root "rexglue\thirdparty\ffmpeg-overlay") -File) {
            Add-SourceFile $f.FullName "build-recipe/ffmpeg-overlay/$($f.Name)"
        }
        Add-SourceFile (Join-Path $root "rexglue\LICENSE") "build-recipe/LICENSE-ReXGlue-BSD-3-Clause.txt"
        foreach ($lic in "GPL-2.0.txt", "LGPL-2.1.txt", "FFmpeg-LICENSE.md", "libmspack-LGPL-2.1.txt") {
            Add-SourceFile (Join-Path $root "dist\licenses\$lic") "licenses/$lic"
        }
        $building = @(
            "Sources of the LGPL/GPL components of $Name",
            "",
            "This archive holds the complete corresponding source of the parts of the release that are under the GNU LGPL",
            "or GPL, exactly as they were built. Project version $version, project commit $projectCommit.",
            "",
            "ffmpeg/source/        FFmpeg, $($pins['FFmpeg'].url) commit $($pins['FFmpeg'].commit), unmodified",
            "                      (LGPL-2.1-or-later). Built into rexffmpeg.dll: libavutil and libavcodec with the",
            "                      fork's configuration header config_windows_x86_64.h (--disable-everything, decoders",
            "                      mp3, mp3float, wmav2, xmaframes; no --enable-gpl, no --enable-nonfree).",
            "libmspack/source/     libmspack, $($pins['libmspack'].url) commit $($pins['libmspack'].commit),",
            "                      unmodified (LGPL-2.1). Only libmspack/mspack/lzxd.c is built, statically into",
            "                      rexfilesystem.dll.",
            "binutils-disasm/source/  the GNU binutils PowerPC disassembler as carried by the ReXGlue SDK copy",
            "                      (rexglue/thirdparty/disasm, GPL-2.0-or-later). Built only into rexglue.exe, the",
            "                      separate recompiler program in compiler\bin.",
            "build-recipe/         rexglue/thirdparty/CMakeLists.txt (source lists and defines of all three) and the",
            "                      generated FFmpeg list files (ffmpeg-overlay: codec, parser and bsf lists), BSD-3-Clause.",
            "licenses/             GPL-2.0, LGPL-2.1, FFmpeg's LICENSE.md, libmspack's COPYING.LIB.",
            "",
            "Building: the whole project builds with build.cmd from its source repository (clang 22.1.8, Ninja, the",
            "VS Build Tools 2022); tools\get_deps.ps1 fetches these same commits. To build your own rexffmpeg.dll, use",
            "the FFmpeg source above with the source list and defines of the libavutil, libavcodec and rexffmpeg targets",
            "in build-recipe/rexglue-thirdparty-CMakeLists.txt, and ffmpeg-overlay/*.c as libavcodec/codec_list.c,",
            "parser_list.c and bsf_list.c. The release's other DLLs use only the public FFmpeg API, so a rexffmpeg.dll built from these sources",
            "(same exported names) can replace the shipped one. rexfilesystem.dll links lzxd.c statically; relinking it",
            "with a modified libmspack needs the project's source (the ReXGlue SDK copy, BSD-3-Clause), which the public",
            "repository of the project carries.",
            ""
        ) -join "`r`n"
        $be = $srcArchive.CreateEntry("$srcName/BUILDING.txt", [IO.Compression.CompressionLevel]::Optimal)
        $bw = New-Object IO.StreamWriter($be.Open(), (New-Object Text.UTF8Encoding($false)))
        try { $bw.Write($building) } finally { $bw.Dispose() }
    } finally {
        $srcArchive.Dispose()
    }
    $check = [IO.Compression.ZipFile]::OpenRead($sourcesZip)
    try { $srcCount = $check.Entries.Count } finally { $check.Dispose() }
    Write-Host ("sources: {0} ({1:N1} MB, {2} files; FFmpeg {3}, libmspack {4})" -f $sourcesZip,
        ((Get-Item $sourcesZip).Length / 1MB), $srcCount, $pins['FFmpeg'].commit.Substring(0, 8), $pins['libmspack'].commit.Substring(0, 8))
}

if ($Zip -or $sourcesZip) {
    if (-not $Zip) { $sums = "" }
    if ($sourcesZip) {
        $srcHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourcesZip).Hash.ToLowerInvariant()
        $sums += "$srcHash  $(Split-Path -Leaf $sourcesZip)`n"
    }
    [IO.File]::WriteAllText((Join-Path $Out "SHA256SUMS"), $sums)
}