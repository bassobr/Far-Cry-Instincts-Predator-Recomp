# Third-party notices

This project builds on third-party code. This file lists every component that is copied into this repository
or fetched and linked by the build, with its license and where it ends up. It is kept up to date together with
`rexglue/UPSTREAM.md` and `tools/get_deps.ps1`. The full license texts are in the named files of each
component (in `rexglue/` or in the dependency folder that `tools/get_deps.ps1` fetches, default
`third_party/rexglue-deps/<name>`). This list is no legal advice.

No game code or game data is part of this repository. The recompiled code is generated at build time from the
user's own disc image into `generated/` and `out/` (both ignored by git).

## Binaries built here

| Binary | What it is | Contains (statically) |
|---|---|---|
| `predator.exe` | host executable with the recompiled game code | our code (`src/`), generated code, ReXGlue host stubs, xxHash |
| `rexbase.dll` | ReXGlue base library | ReXGlue/Xenia code, spdlog, xxHash, Dear ImGui (sources compiled in), stb_image (header) |
| `rexkernel.dll` | kernel/XAM HLE | ReXGlue/Xenia code, **FFmpeg** (libavcodec, libavutil), ExCrypt, xxHash, TLSF |
| `rexfilesystem.dll` | file system module | ReXGlue/Xenia code, **libmspack** (`lzxd.c`), ExCrypt |
| `rexppc.dll`, `rexapp.dll`, `rexapu*.dll`, `rexhid*.dll`, `rexui*.dll`, `rexgpu.dll`, `rexgpu-xenos-null.dll`, `rexxam.dll`, `rexxbdm.dll` | runtime libraries and modules | ReXGlue/Xenia code, SIMDe and toml++ headers; `*-sdl3` link `SDL3.dll`; `rexgpu-xenos-null`: xxHash |
| `SDL3.dll` | SDL 3 (shared) | SDL |
| `rexglue.exe` | recompiler tool (codegen) | ReXGlue codegen, **GNU binutils PowerPC disassembler (GPL-2.0-or-later)**, CLI11, inja, nlohmann/json, toml++, xxHash |

Measured on 2026-11-12 from the link lines of `out/build/release/build.ninja`: `disasm.lib` (binutils) and
`rexcodegen.lib` are linked only into `rexglue.exe`; `libavcodec.lib`/`libavutil.lib` only into
`rexkernel.dll`; `mspack.lib` only into `rexfilesystem.dll`; `spdlog.lib` only into `rexbase.dll`; the ImGui
sources are compiled into `rexbase.dll`. Header-only parts (SIMDe, toml++, stb_image, RenderDoc API) are not
traced per binary. `predator.exe` imports `rexapp.dll`, `rexkernel.dll`, `rexppc.dll`, `rexbase.dll` and the
VC++/UCRT runtime (measured with `tools/check_exe.py`).

## Components

### Copied into this repository

| Component | Where | License | Notes |
|---|---|---|---|
| ReXGlue SDK `351bc94d` | `rexglue/` | BSD-3-Clause, `rexglue/LICENSE` (Copyright (c) 2026 Tom Clay) | frozen copy with local changes, see `rexglue/UPSTREAM.md` |
| Xenia (parts inside ReXGlue) | `rexglue/src`, `rexglue/include` | BSD-3-Clause (Copyright (c) Ben Vanik and Xenia contributors), notices in the files | kernel, XAM, file system, GPU, APU code |
| XenonRecomp (parts inside ReXGlue) | `rexglue/src/codegen`, `rexglue/include/rex/ppc` | MIT (Copyright (c) hedge-dev and contributors), notices in the files | recompiler code |
| GNU binutils PowerPC disassembler | `rexglue/thirdparty/disasm` (`ppc-dis.c`, `ppc.h`, `dis-asm.h`, ...) | **GPL-2.0-or-later** (Free Software Foundation) | only in `rexglue.exe`, a separate program. Whoever ships `rexglue.exe` ships a GPL program: license text and source offer go with it. |
| DXBCChecksum | `rexglue/thirdparty/dxbc` | Copyright (c) 2008-2016 Advanced Micro Devices, Inc. (Xenia's copy, see the file header) | only in the Xenos D3D12 driver, which our build does not build (`FCP_XENOS_GPU=OFF`) |
| DirectXShaderCompiler API headers | `rexglue/thirdparty/dxc` | University of Illinois/NCSA Open Source License (Microsoft) | headers only, D3D12 driver only (not built) |
| NT image format header | `rexglue/thirdparty/pe/pe_image.h` | Copyright (c) Microsoft Corporation (Xenia's copy, header only) | structure definitions for PE parsing |
| RenderDoc in-application API | `rexglue/thirdparty/renderdoc/renderdoc_app.h` | MIT | header only |
| stb_image | `rexglue/thirdparty/stb/stb_image.h` | MIT or public domain (Unlicense), choice of the user | in `rexbase.dll` / `rexui.dll` |
| FFmpeg list overlay | `rexglue/thirdparty/ffmpeg-overlay` | part of the FFmpeg build (LGPL-2.1-or-later) | generated codec/parser/bsf lists |

### Fetched by `tools/get_deps.ps1` (not committed)

| Component | Pinned commit | License (file) | Linked into |
|---|---|---|---|
| FFmpeg (fork wmarti/FFmpeg, branch xenia-ffmpeg-canary-full) | `0604b464` | **LGPL-2.1-or-later** as configured (`config_windows_x86_64.h`: `FFMPEG_LICENSE "LGPL version 2.1 or later"`, `CONFIG_GPL 0`, `CONFIG_NONFREE 0`); `COPYING.LGPLv2.1`, `LICENSE.md` | `rexkernel.dll` (static). Shipping obligations of the LGPL (source, relinking) apply; since 2026-11-12 FFmpeg is its own DLL (section below). |
| libmspack | `30590772` | **LGPL-2.1** (`libmspack/COPYING.LIB`) | `rexfilesystem.dll` (static, `lzxd.c` only); same LGPL obligations |
| SDL 3 (release-3.4.x) | `147a8ee3` | zlib (`LICENSE.txt`) | `SDL3.dll` |
| spdlog | `79524ddd` | MIT (`LICENSE`) | `rexbase.dll` |
| Dear ImGui | `6d910d54` (v1.92.5) | MIT (`LICENSE.txt`) | `rexbase.dll` (sources compiled in), UI modules; `predator.exe` calls its API through `rexbase.dll` for the settings menu (F10) and the frame rate display (`src/fcp/settings_menu.cpp`), our NVRHI overlay pass (`src/renderer/core/overlay_pass.cpp`) draws its draw lists; no ImGui source is compiled into `predator.exe` |
| toml++ | `30172438` | MIT (`LICENSE`) | `rexglue.exe`, runtime settings |
| SIMDe | `71fd833d` | MIT (`COPYING`) | headers, all binaries |
| xxHash | `e626a72b` | BSD-2-Clause (library part of `LICENSE`) | runtime |
| ExCrypt | `08809458` | BSD-3-Clause (`LICENSE`) | `rexkernel.dll`, `rexfilesystem.dll` |
| TLSF | `deff9ab5` | BSD-3-Clause ("Released under the BSD license", `README.md`, `tlsf.c`) | runtime guest allocator |
| CLI11 | `bfffd37e` | BSD-3-Clause (`LICENSE`) | `rexglue.exe` |
| inja (with nlohmann/json, MIT, in `third_party/include`) | `7d1b4600` | MIT (`LICENSE`) | `rexglue.exe` (codegen templates) |
| Tracy | `05cceee0` | BSD-3-Clause (`LICENSE`) | only with `FCP_TRACY=ON` (`TracyClient.dll`); off by default |
| glslang | `f4f1d8a3` | BSD-3-Clause and others (`LICENSE.txt`) | only with `FCP_XENOS_GPU=ON` (Xenos Vulkan driver); not fetched by default |
| Catch2 | `88abf9bf` | BSL-1.0 (`LICENSE.txt`) | only for the SDK unit tests; not fetched by default |

### Native renderer (`src/renderer`, `tools/shaders`), added 2026-11-12

Copied into this repository:

| Component | Where | License | Notes |
|---|---|---|---|
| King Kong Recompiled `e3e5d3f` (native renderer: resources, pipeline cache, render thread, host helpers, tests) | `src/renderer/kk/` | MIT, Copyright (c) 2026 TekRantGaming and contributors, `src/renderer/kk/LICENSE-KingKongRecompiled.txt` | file list and changes in `src/renderer/kk/CHANGES.md`; no GPL-derived code found (same file) |
| King Kong Recompiled `e3e5d3f` (shader translator adapter, prelude) | `tools/shaders/` | MIT, `tools/shaders/LICENSE-KingKongRecompiled.txt` | unchanged from Phase 1 (`tools/shaders/xenosrecomp/CHANGES.md`) |
| XenosRecomp `990d03b` (translator core, King Kong's rework) | `tools/shaders/xenosrecomp/` | MIT (hedge-dev), `tools/shaders/xenosrecomp/LICENSE-XenosRecomp.md` | unchanged from Phase 1 |
| Xenia / ReXGlue behaviour (tiling address functions, Xenos format fields, render target format numbers) | `src/renderer/kk/resources/tiling.*`, `xenos.*`, `src/renderer/core/target_cache.cpp` | precautionary BSD-3-Clause notice, `src/renderer/kk/LICENSE-Xenia-ReXGlue-BSD3.txt` | King Kong states the code is its own, following Xenia's behaviour |
| SMAA (iryoku/smaa: reference shader `SMAA.hlsl`, area and search textures), packaged by SCDA-XBERecomp (MIT, TGP482, Joshhhuaaa), taken from Far Cry Instincts Recomp `src/gpu/smaa_shader.h`, `smaa_textures.h` (MIT) | `src/renderer/smaa/` | MIT, Copyright (C) 2013 Jorge Jimenez et al., `src/renderer/smaa/LICENSE-SMAA.txt` | optional SMAA 1x on the final picture (`[graphics] smaa`, `src/renderer/core/smaa_pass.cpp`), added 2026-11-13; in `predator.exe` |

Fetched by `tools/get_renderer_deps.ps1` into `third_party/renderer-deps` (not committed):

| Component | Pinned | License (file) | Linked into |
|---|---|---|---|
| NVRHI, King Kong's patched copy of `6b96fb03` (inside the King Kong clone, `native-renderer/thirdparty/nvrhi`) | KK `e3e5d3f` | MIT, Copyright (c) 2014-2021 NVIDIA CORPORATION (`LICENSE.txt`; `ThirdPartyLicenses.txt` for what it bundles) | `predator_renderer` (static), its tests and `renderer_probe` |
| DirectX-Headers v1.717.0-preview (inside the King Kong clone) | KK `e3e5d3f` | MIT (`LICENSE`) | headers and `dxguids.cpp` |
| Vulkan-Headers v1.4.352 | `015e25c3` | Apache-2.0 OR MIT (`LICENSE.md`, `LICENSES/`) | headers only; `vulkan-1.dll` comes from the graphics driver |
| fmt | `873670ba` | MIT (`LICENSE`) | `fcshaders` (header-only) |
| xxHash | `2bf8313b` | BSD-2-Clause (`LICENSE`) | `fcshaders`, renderer resources (header-only) |
| DirectXShaderCompiler v1.9.2609 (`dxc_2026_09_29.zip`, SHA-256 `ad31b1fc…a7e7f1`) | release zip | University of Illinois/NCSA (`LICENSE-LLVM.txt`), Microsoft terms for the redistributable parts (`LICENSE-MS.txt`) | `dxcompiler.dll` loaded at run time, copied next to the renderer programs; `dxil.dll` is not used |
| toml++ (from `tools/get_deps.ps1`) | `30172438` | MIT | `predator_renderer` (configuration reader) |

Shader packs and caches (`*.fcpack`, `*.fcse`) and everything under `work/` are derived from the user's disc and are
never committed or shipped.

### FFmpeg as its own DLL (`rexffmpeg.dll`), added 2026-11-12

This section supersedes what the tables above say about FFmpeg being linked statically into `rexkernel.dll`.
Since `rexglue/UPSTREAM.md` change 33 the Windows build puts the FFmpeg code into its own library:

| Binary | What it is | Contains |
|---|---|---|
| `rexffmpeg.dll` (`rexffmpegrd.dll` in RelWithDebInfo, `rexffmpegd.dll` in Debug) | FFmpeg 4.4 libavutil and libavcodec, only the parts the XMA decoder needs (decoders `xmaframes`, `wmav2`, `mp3`/`mp3float`, parser `mpegaudio`; list in `rexglue/thirdparty/ffmpeg-overlay`) | FFmpeg only, **LGPL-2.1-or-later** |
| `rexkernel.dll` | kernel/XAM HLE | no FFmpeg code any more; it imports 12 functions of the public FFmpeg API from `rexffmpeg.dll` (`avcodec_find_decoder`, `avcodec_alloc_context3`, `avcodec_open2`, `avcodec_send_packet`, `avcodec_receive_frame`, `avcodec_free_context`, `av_packet_alloc`, `av_buffer_alloc`, `av_frame_alloc`, `av_frame_free`, `av_strerror`, `av_log_set_callback`; measured with `llvm-objdump -p`) |

- **Source** (corresponding source for the LGPL): the FFmpeg fork `https://github.com/wmarti/FFmpeg`, branch
  `xenia-ffmpeg-canary-full`, commit `0604b464c7cb4ebc94940cf1f324a3b26b87717c` (pinned in `tools/get_deps.ps1`
  and `rexglue/UPSTREAM.md`), unmodified, plus the build recipe in `rexglue/thirdparty/CMakeLists.txt` (source
  list, defines) and the three list files in `rexglue/thirdparty/ffmpeg-overlay`. The configuration is the
  fork's `config_windows_x86_64.h`: `--disable-everything ... --enable-decoder='mp3,mp3float,wmav2,xmaframes'`,
  `CONFIG_GPL 0`, `CONFIG_NONFREE 0`, `CONFIG_VERSION3 0`, no `--enable-gpl`, no `--enable-nonfree`.
- **Shipping** (when a build is released): put `COPYING.LGPLv2.1` and `LICENSE.md` of the fork next to
  `rexffmpeg.dll`, name FFmpeg and the LGPL in the release notes, and either ship the source above with the
  binaries or include a written offer to provide it. `tools/dist.ps1` writes the source archive
  `<name>-v<version>-sources.zip` next to the release zip (switch `-Sources`, on with `-Zip`; FFmpeg, libmspack and
  the binutils disassembler, each from its pinned commit, with license texts and `BUILDING.txt`). Users may replace `rexffmpeg.dll` by their own build of the
  same sources (same exported names); `rexkernel.dll` only uses the public API listed above.
- `REXGLUE_FFMPEG_SHARED=OFF` (CMake) restores the static linking into `rexkernel.dll` (upstream).

Also since 2026-11-12: `predator.exe` imports `SDL3.dll` (zlib) for the virtual test pads (`src/fcp/virtual_pads.cpp`).

### Toolchain and system components (not shipped by this repository)

| Component | License | Use |
|---|---|---|
| LLVM/clang/lld 22.1.8 (`tools/get_llvm.ps1`) | Apache-2.0 with LLVM exception | compiler and linker |
| MSVC STL, VC++ runtime, UCRT, Windows SDK (VS Build Tools 2022) | Microsoft license terms | headers, import libraries; at run time `VCRUNTIME140*.dll`, `MSVCP140*.dll`, UCRT |
| Python packages in `toolchain\venv` (pycryptodome: BSD-2-Clause/public domain parts; capstone: BSD-3-Clause) | as named | build scripts (`tools/prepare_xex.py`), analysis |

Our own code (`src/`, `cmake/`, `tools/`, `config/`) is not covered by the notices above.

### Game code as a DLL and the release folder, added 2026-11-13

- `predator.exe` no longer contains the recompiled game code; it is `predator_code.dll`, built by the development
  build and on the player's first start. The release folder (`tools/dist.ps1`)
  carries its own notices: `dist/THIRD_PARTY_NOTICES.md` and the license texts in `dist/licenses/`.
- **LZMA SDK 24.09** (`LzmaDec.c`, Igor Pavlov), fetched by `tools/get_lzma.ps1` into `third_party/lzma-sdk`
  (SHA-256 pinned), **public domain**; compiled into `predator.exe` (reads the disc's DAT2 archives for the shader
  pack of the first start).
- `rexglue/include/rex/guest_std`: freestanding replacements of the standard headers the generated code needs,
  written for this project (BSD-3-Clause like the SDK, `rexglue/UPSTREAM.md` change 140); no code of a C or C++
  standard library.
- The release bundles LLVM 22.1.8 `clang.exe`, `lld-link.exe` and clang's own headers (Apache-2.0 with LLVM
  exception), `rexglue.exe` (contains the GPL-2.0-or-later binutils disassembler, see above), SIMDe headers (MIT),
  the SDK headers, `dxcompiler.dll` (NCSA) and, unless `-NoVcRuntime`, the Visual C++ runtime DLLs (Microsoft
  redistributable files). Whether the VC++ runtime, the MIT license of this project and the GPL/LGPL source offers
  are final is still open (decision of the maintainer).
