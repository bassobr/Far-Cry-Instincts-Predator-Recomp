<#
.SYNOPSIS
  One-time, mechanical import of the generic renderer building blocks from King Kong Recompiled to
  src\renderer\kk (version: e3e5d3f488cbdcd35afd0a1f08b958b0b50f03c3, MIT).

.DESCRIPTION
  Copies the files listed below from the clone (tools\get_renderer_deps.ps1) and only replaces names:
    namespace kknr -> fcp::res, namespace nr -> fcp::nr, kknr_test/kknr_tools/KKNR_ -> fcp_res_test/fcp_res_tools/FCPRES_,
    #include "kknr/..." -> "kk/resources/...", #include "backend/..." -> "kk/backend/...".
  Every file gets an origin line. Everything else (removing game-specific parts, adapting to our
  interfaces) is manual work afterwards and is listed in src\renderer\kk\CHANGES.md. A second run overwrites
  this manual work: only with -Force and only to compare a new upstream version (then write the output to a
  different folder with -OutDir).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\renderer_import_kk.ps1 -OutDir third_party\kk-import-check
#>
param(
  [string]$KkDir = "",
  [string]$OutDir = "",
  [switch]$Force
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if (-not $KkDir) {
  $deps = $env:FCP_RENDERER_DEPS_DIR
  if (-not $deps) { $deps = Join-Path $repo "third_party\renderer-deps" }
  $KkDir = Join-Path $deps "king-kong-recompiled\native-renderer"
}
if (-not $OutDir) { $OutDir = Join-Path $repo "src\renderer\kk" }
$commit = "e3e5d3f"

# source (relative to native-renderer) -> target (relative to OutDir)
$map = [ordered]@{}
foreach ($h in "buffers","endian","guest_texture","nvrhi_format","render_targets","resident_snapshot","resolve","resource_work","texture_cache","texture_convert","tiling","xenos") {
  $map["resources/include/kknr/$h.h"] = "resources/$h.h"
}
foreach ($c in "buffers","endian","guest_texture","render_targets","resolve","texture_cache","texture_convert","texture_workers","tiling","xenos") {
  $map["resources/src/$c.cpp"] = "resources/$c.cpp"
}
$map["resources/tools/image_io.h"] = "resources/image_io.h"
$map["resources/tools/image_io.cpp"] = "resources/image_io.cpp"
foreach ($t in "test.h","test_main.cpp","test_tiling.cpp","test_roundtrip.cpp","test_tail_mips.cpp","test_convert.cpp","test_convert_workers.cpp","test_buffers.cpp","test_cache.cpp","test_render_targets.cpp","test_resolve.cpp","test_nvrhi_format.cpp") {
  $map["resources/tests/$t"] = "resources/tests/$t"
}
foreach ($b in "pipeline_cache.h","pipeline_cache.cpp","pipeline_demand.h","pipeline_workers.h","thread_scheduling.h","cpu_profile.h",
               "vk_pipeline_cache.h","vk_pipeline_cache.cpp","vk_pipeline_library.h","vk_pipeline_library.cpp",
               "vulkan_dispatch.h","vulkan_dispatch.cpp","file_lock.h","file_lock.cpp","driver_pipeline_cache.h",
               "vulkan_requirements.h","d3d12_requirements.h","background_file_writer.h","guest_memory.h","guest_layout.h",
               "render_thread.h","render_thread.cpp") {
  $map["backend/$b"] = "backend/$b"
}
foreach ($t in "check.h","pipeline_cache_tests.cpp","pipeline_demand_tests.cpp","background_file_writer_tests.cpp",
               "vulkan_requirements_tests.cpp","d3d12_requirements_tests.cpp","scheduling_tests.cpp") {
  $map["backend/tests/$t"] = "backend/tests/$t"
}

foreach ($src in $map.Keys) {
  $from = Join-Path $KkDir ($src -replace "/", "\")
  $to = Join-Path $OutDir ($map[$src] -replace "/", "\")
  if ((Test-Path $to) -and -not $Force) { Write-Host "  present, skipped: $($map[$src])"; continue }
  $t = [IO.File]::ReadAllText($from)
  $t = $t -replace '#include "kknr/', '#include "kk/resources/'
  $t = $t -replace '#include "\.\./tools/image_io\.h"', '#include "kk/resources/image_io.h"'
  $t = $t -replace '#include "test\.h"', '#include "kk/resources/tests/test.h"'
  $t = $t -replace '#include "backend/', '#include "kk/backend/'
  $t = $t -replace '\bnamespace kknr_test\b', 'namespace fcp_res_test'
  $t = $t -replace '\bkknr_test::', 'fcp_res_test::'
  $t = $t -replace '\bnamespace kknr_tools\b', 'namespace fcp_res_tools'
  $t = $t -replace '\bkknr_tools::', 'fcp_res_tools::'
  $t = $t -replace '\bKKNR_', 'FCPRES_'
  $t = $t -replace '\bnamespace kknr\b', 'namespace fcp::res'
  $t = $t -replace '\bkknr::', 'fcp::res::'
  $t = $t -replace '\bnamespace nr\b', 'namespace fcp::nr'
  $t = $t -replace '(?<![\w:])nr::', 'fcp::nr::'
  $head = "// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, $commit, native-renderer/$src),`n" +
          "// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.`n" +
          "// Changes: src/renderer/kk/CHANGES.md.`n"
  New-Item -ItemType Directory -Force -Path (Split-Path -Parent $to) | Out-Null
  [IO.File]::WriteAllText($to, $head + $t)
  Write-Host "  $src -> $($map[$src])"
}
Copy-Item -Force (Join-Path $KkDir "..\LICENSE") (Join-Path $OutDir "LICENSE-KingKongRecompiled.txt")
Write-Host "done ($($map.Count) files)"
