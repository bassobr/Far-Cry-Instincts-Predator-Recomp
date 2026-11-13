# Building blocks taken from King Kong Recompiled

Origin: <https://github.com/TekRantGaming/king-kong-recompiled>, commit `e3e5d3f488cbdcd35afd0a1f08b958b0b50f03c3`,
folder `native-renderer/`. Licence: MIT, Copyright (c) 2026 TekRantGaming and contributors
(`LICENSE-KingKongRecompiled.txt`). Every file states its origin in its first three lines.

Fetched with `tools/get_renderer_deps.ps1`, copied with `tools/renderer_import_kk.ps1` (name replacement only), then
adapted by hand (below). King Kong's NVRHI version and DirectX-Headers are not copied but built from the clone under
`third_party/renderer-deps` (`src/renderer/CMakeLists.txt`).

BSD-3 as a precaution (`LICENSE-Xenia-ReXGlue-BSD3.txt`): according to King Kong's own statement, `resources/tiling.*`
and `resources/xenos.*` follow the behaviour of Xenia's/ReXGlue's `texture_util` and `xenos.h` ("the code here is our own").

No GPL code: a search for `Unleashed`, `Marathon`, `GPL-3`, `General Public License` in the whole clone outside
`thirdparty/` found only `kk/THIRD-PARTY-NOTICES.txt` (licence texts for King Kong's port binaries) and
`native-renderer/docs/design.md` (a delimitation: the renderer of Unleashed Recompiled was "studied, not copied"). None
of the copied files names a GPL origin.

## Mechanical changes (all files)

- Namespaces `kknr` → `fcp::res`, `nr` → `fcp::nr`; `kknr_test`/`kknr_tools`/`KKNR_` → `fcp_res_test`/`fcp_res_tools`/`FCPRES_`.
- Includes `kknr/…` → `kk/resources/…`, `backend/…` → `kk/backend/…`; line endings LF.

## Files

| Here | Source (`native-renderer/`) | Manual changes |
|---|---|---|
| `resources/{buffers,endian,guest_texture,render_targets,resolve,texture_cache,texture_convert,texture_workers,tiling,xenos}.{h,cpp}`, `resources/{nvrhi_format,resident_snapshot,resource_work}.h` | `resources/include/kknr/`, `resources/src/` | Comment with a King Kong guest address in `render_targets.h` removed. Otherwise unchanged. Not taken: `texture_pack.*` (King Kong's HD texture packs) |
| `resources/image_io.{h,cpp}` | `resources/tools/` | unchanged (PNG/DDS output, decoders for previews) |
| `resources/tests/*` | `resources/tests/` | unchanged; `test_texture_pack.cpp` not taken. The fetch constants in `test_roundtrip.cpp` are King Kong's test data (no addresses) |
| `backend/pipeline_cache.{h,cpp}` | `backend/` | Magic `KGPC` → `FCPC`, version 3 → 1 (own file format, same record); motion records (`MotionPipelineRecord`, `kRecordMotion`, `kRecordTessellation`) of the upscaler/tessellation removed; references to the `nr_pipelines` tool and to game names removed from comments |
| `backend/pipeline_demand.h`, `pipeline_workers.h`, `thread_scheduling.h`, `cpu_profile.h`, `driver_pipeline_cache.h`, `background_file_writer.h`, `file_lock.{h,cpp}`, `vk_pipeline_cache.{h,cpp}` | `backend/` | Comments with King Kong switches and game names cleaned up; code unchanged |
| `backend/vk_pipeline_library.{h,cpp}` | `backend/` | Switch `KK_DEV_NO_GPL` → `FCP_VK_NO_GPL`, `KK_GPL_ANY_VENDOR` → `FCP_VK_GPL_ANY_VENDOR`; reference to King Kong's CVar removed |
| `backend/vulkan_dispatch.{h,cpp}`, `vulkan_requirements.h`, `d3d12_requirements.h`, `guest_memory.h` | `backend/` | unchanged |
| `backend/guest_layout.h` | `backend/` | Comment with a King Kong guest address removed. The object offsets in it are King Kong's; the renderer reads offsets from `config/predator/renderer.toml` (`renderer/guest_device.h`), this file only serves as a reference for the decoders |
| `backend/render_thread.{h,cpp}` | `backend/` | adapted to our `DrawSink` records (`renderer/nr_types.h`); device ranges and object sizes from the configuration instead of literals (`kReadRanges`, `kSurfaceBytes` …); `DrawVerticesUP` data is copied; query `GetData` bypasses the render thread; AVX2 comparison, GPU-loss path, long-frame report and per-frame CPU timing removed |
| `backend/tests/check.h` | `backend/tests/` | `NR_TEST_MAIN()` is empty (one `main` for all test files in `src/renderer/tests/test_main.cpp`), diagnostic switches removed |
| `backend/tests/{pipeline_cache,pipeline_demand,background_file_writer,vulkan_requirements,d3d12_requirements,scheduling}_tests.cpp` | `backend/tests/` | own `main()` turned into a test case of the shared run; missing `return 0` added; cases of the `nr_pipelines` tool and of the motion records removed; benchmark entry `--bench` removed |

## Modelled on, but rewritten

- `core/translated_layout.cpp`: arrangement of the binding layouts (set 0 constants, sets 1–6 bindless) and
  fallback resources in slot 0 after `backend/game_renderer.cpp` (`GameRenderer::Initialize`).
- `TranslatedLayout::AdjustNdcForApi`: rule from `backend/game_renderer.cpp` (NVRHI flips Vulkan viewports,
  plus `-fvk-invert-y`: cancel one flip via `ndcScale`).
- `core/host_vulkan.cpp`, `core/host_d3d12.cpp`: standalone devices instead of King Kong's hosts on the SDK provider
  (`vulkan_host.cpp`, `d3d12_host.cpp`); only the requirement checks (`*_requirements.h`) are taken.
