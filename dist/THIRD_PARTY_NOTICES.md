# Third-party notices

This release is built from the code of this project (MIT, `LICENSE`) and the third-party components below. Each
component keeps its license; the license texts are in the folder `licenses\` of this release. No part of the game
*Far Cry Instincts Predator* is included: the game code is made on the player's PC from the player's own disc image
on the first start and stays in the player's user folder.

## Where each component is in this release

| File | Contains | License (text in `licenses\`) |
|---|---|---|
| `predator.exe` | this project's runtime; ReXGlue host code; King Kong Recompiled native renderer parts; XenosRecomp shader translator core; NVRHI; SMAA (optional edge smoothing); DirectX-Headers (GUIDs); xxHash; fmt; toml++; LZMA SDK decoder (`LzmaDec.c`); the settings menu calls Dear ImGui in `rexbase.dll` | MIT (`LICENSE`); `DearImGui-MIT.txt`, `ReXGlue-BSD-3-Clause.txt`, `Xenia-ReXGlue-BSD-3-Clause.txt`, `KingKongRecompiled-MIT.txt`, `XenosRecomp-MIT.txt`, `NVRHI-MIT.txt`, `SMAA-MIT.txt`, `DirectX-Headers-MIT.txt`, `xxHash-BSD-2-Clause.txt`, `fmt-MIT.txt`, `tomlplusplus-MIT.txt`, `LZMA-SDK-public-domain.txt` |
| `rexbase.dll`, `rexppc.dll`, `rexapp.dll`, `rexapu.dll`, `rexapu-sdl3.dll`, `rexhid.dll`, `rexhid-sdl3.dll`, `rexhid-xinput.dll`, `rexui.dll`, `rexui-sdl3.dll`, `rexgpu.dll`, `rexgpu-xenos-null.dll`, `rexxam.dll`, `rexxbdm.dll` | ReXGlue SDK (with code from Xenia and XenonRecomp); spdlog; Dear ImGui; stb_image; SIMDe; toml++; xxHash | `ReXGlue-BSD-3-Clause.txt`, `Xenia-ReXGlue-BSD-3-Clause.txt`, `XenonRecomp-MIT.txt`, `spdlog-MIT.txt`, `DearImGui-MIT.txt`, `SIMDe-MIT.txt`, `tomlplusplus-MIT.txt`, `xxHash-BSD-2-Clause.txt` (stb_image: public domain / MIT, notice in its header) |
| `rexkernel.dll` | ReXGlue/Xenia kernel and XAM emulation; ExCrypt; TLSF; xxHash | `ReXGlue-BSD-3-Clause.txt`, `Xenia-ReXGlue-BSD-3-Clause.txt`, `ExCrypt-BSD-3-Clause.txt`, `TLSF-BSD.txt`, `xxHash-BSD-2-Clause.txt` |
| `rexfilesystem.dll` | ReXGlue/Xenia file system; ExCrypt; **libmspack** (`lzxd.c`, LZX decompression), linked statically | ReXGlue/Xenia as above, `ExCrypt-BSD-3-Clause.txt`, **`libmspack-LGPL-2.1.txt`** (see "LGPL and GPL components") |
| `rexffmpeg.dll` | **FFmpeg** 4.4 libavutil/libavcodec, only the XMA, WMA v2 and MP3 decoders | **LGPL 2.1 or later**: `LGPL-2.1.txt`, `FFmpeg-LICENSE.md` |
| `SDL3.dll` | SDL 3 | zlib: `SDL3-zlib.txt` |
| `dxcompiler.dll` | DirectX Shader Compiler (shader translation at run time and for the shader pack) | University of Illinois/NCSA: `DirectXShaderCompiler-NCSA.txt`; Microsoft terms of the release package: `DirectXShaderCompiler-Microsoft.txt` |
| `fcshaders.exe` | this project's shader packer (King Kong Recompiled adapter, XenosRecomp core, fmt, xxHash) | as for `predator.exe` |
| `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, `msvcp140_atomic_wait.dll` | Microsoft Visual C++ runtime | Microsoft Visual Studio license terms; redistributable files ("Distributable Code") of Visual Studio Build Tools 2022 |
| `compiler\bin\clang.exe`, `compiler\bin\lld-link.exe`, `compiler\lib\clang\22\include\` | LLVM 22.1.8 (official release build): C++ compiler, linker, compiler headers | Apache 2.0 with LLVM exception: `LLVM-Apache-2.0-with-LLVM-exception.txt` (also `compiler\LICENSE-LLVM.txt`) |
| `compiler\bin\rexglue.exe` | ReXGlue code generator; **GNU binutils PowerPC disassembler** (`ppc-dis.c`, `ppc.h`, `dis-asm.h`); CLI11; inja; nlohmann/json; toml++; xxHash | ReXGlue/Xenia/XenonRecomp as above; **`GPL-2.0.txt`** (binutils, GPL 2 or later; see below); `CLI11-BSD-3-Clause.txt`, `inja-MIT.txt`, `nlohmann-json-MIT.txt`, `tomlplusplus-MIT.txt`, `xxHash-BSD-2-Clause.txt` |
| `compiler\include\rex\` | ReXGlue SDK headers, with this project's freestanding headers `rex\guest_std` | `ReXGlue-BSD-3-Clause.txt`, `Xenia-ReXGlue-BSD-3-Clause.txt`, `XenonRecomp-MIT.txt` |
| `compiler\include\simde\` | SIMDe headers | MIT: `SIMDe-MIT.txt` (also `compiler\include\simde\COPYING`) |
| `compiler\lib\*.lib` | import libraries of `rexppc.dll`, `rexbase.dll` and of the C runtime DLLs (made from name lists by this project) | as the DLLs they name |

Not part of this release: plume, Unleashed Recompiled, Marathon (no code of them is used), Tracy, glslang, Catch2,
the Xenos GPU emulation of ReXGlue (`rexgpu-xenos-vulkan/d3d12`), `dxil.dll`.

## LGPL and GPL components

- **FFmpeg** (`rexffmpeg.dll`, LGPL 2.1 or later). Corresponding source: `https://github.com/wmarti/FFmpeg`, branch
  `xenia-ffmpeg-canary-full`, commit `0604b464c7cb4ebc94940cf1f324a3b26b87717c`, unmodified, configured with
  `--disable-everything` and the decoders `mp3,mp3float,wmav2,xmaframes` (no `--enable-gpl`, no `--enable-nonfree`);
  the build recipe is part of this project's source. `rexffmpeg.dll` can be replaced by your own build of these
  sources: the other DLLs use only the public FFmpeg API.
- **libmspack** (statically in `rexfilesystem.dll`, LGPL 2.1). Source: `https://github.com/kyz/libmspack`, commit
  `30590772`. To relink `rexfilesystem.dll` with a modified libmspack, use the complete source of `rexfilesystem.dll`,
  which is part of this project's source code (ReXGlue SDK copy, BSD 3-Clause).
- **GNU binutils PowerPC disassembler** (in `compiler\bin\rexglue.exe`, GPL 2 or later). `rexglue.exe` is a separate
  program that the game only starts to translate the disc's code; `predator.exe` and the DLLs contain none of it.
  The complete corresponding source of `rexglue.exe` (the ReXGlue SDK copy with the disassembler in
  `rexglue/thirdparty/disasm`) is part of this project's source code.

Each release comes with a source archive, `Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip`, published next
to the release zip and listed in its `SHA256SUMS`. It holds the exact sources of these three components as built
(FFmpeg with the build recipe and list overlay used for `rexffmpeg.dll`, libmspack, the disassembler of
`rexglue.exe`), their license texts and a short `BUILDING.txt`. In addition, for three years from the release date
the source of these components as shipped is available on request from the project's maintainer; the public
repository of this project carries it as well.

## Other notices

- *Far Cry* is a trademark of Ubisoft Entertainment. Xbox and Xbox 360 are trademarks of Microsoft. This project is
  not affiliated with or endorsed by Ubisoft or Microsoft.
- The first start reads `default.xex` and the shader archives from the player's own disc image and writes the
  results only into the player's user folder.
