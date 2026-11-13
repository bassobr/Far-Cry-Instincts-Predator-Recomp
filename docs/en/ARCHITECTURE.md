# Architecture

The port consists of an executable without any game code, `predator.exe`, and the recompiled game code,
`predator_code.dll`, which is generated from the player's own disc image. The executable runs the game on the
ReXGlue SDK runtime; the game's Direct3D calls go to a native renderer instead of a GPU emulation.

```
 disc image (.iso)
   │ default.xex (SHA-256 checked)            shaders.dat / common.dat
   ▼                                            ▼
 rexglue.exe codegen ──► ~150 C++ files      fcshaders.exe pack ──► predator.<spirv|dxil>.fcpack
   │                                            │
   ▼                                            │
 clang + lld-link ──► predator_code.dll         │
   │ (imports hooks and kernel wrappers          │
   │  from predator.exe)                         │
   ▼                                            ▼
 predator.exe: ReXGlue runtime (kernel, XAM, file system, audio, input) + hooks + native renderer
```

| Part | What it is |
|---|---|
| `predator.exe` | host entry point (`src/main.cpp`, a `rex::app::TitleApp`), the title layer `src/fcp/` (hooks, settings, input, frame rate, first-start build driver, progress window, disc reader, test tools) and the renderer `src/renderer/` |
| `predator_code.dll` | the recompiled game code; no entry point, no C runtime |
| `rex*.dll` | ReXGlue SDK runtime libraries (frozen copy in `rexglue/`); `rexffmpeg.dll` holds the FFmpeg decoders |
| `compiler\` (release only) | `rexglue.exe`, `clang.exe`, `lld-link.exe`, headers and import libraries for the first start |
| `fcshaders.exe`, `dxcompiler.dll` | shader translator/packer and the DirectX Shader Compiler |
| `config/predator/` | title configuration: function names and addresses of every hook, data addresses, disc checksum, renderer layout |

## 1. Recompiler

The recompiler is the code generator of the ReXGlue SDK (`rexglue.exe codegen`), driven by a manifest and the
title configuration `config/predator/default.toml`. XenonRecomp was only used as a cross-check during development.

- **Input:** `default.xex` of the disc (the version without title update). The build checks its SHA-256 against
  `config/predator/disc.toml`.
- **Output:** one C++ function per PowerPC function (about 22,800 functions in about 150 files) plus tables for
  dispatch and image layout.
- **Function names:** the local table `[function_names]` (`0xADDRESS = "Name"`) gives a function a symbol name
  without changing its boundaries. Hooks in `src/` refer to these names, never to addresses. About 390 names come from
  `config/predator/default.toml` and from `config/predator/d3d_symbols.toml` (the game's Direct3D library).
- **Accuracy settings** in the title configuration: `[vector_math]` is `exact` for all approximating vector
  instructions; `[fp_rules]` applies the PowerPC rules for NaN results and the single rounding of `fmadds`. Both
  were chosen for accuracy; their cost was measured within noise (see `rexglue/UPSTREAM.md`, change 70).
- **Semantic test:** `predator_semtest` checks the generated code of every instruction form the game uses
  (about 41,700 cases; expected values partly from hardware test vectors, otherwise from a reference model). It runs
  as part of the default build and fails the build on new failures (see [BUILDING.md](BUILDING.md)).

The generated code is compiled freestanding (`REX_GUEST_FREESTANDING`, own minimal standard headers), so the first
start needs neither Visual Studio nor the Windows SDK on the player's PC. Measured: the freestanding and the hosted
compile produce identical machine code for 23,385 functions; 36 template instances differ only in register
allocation.

## 2. Executable and game code DLL

`predator.exe` is built without any game code (5.1 MB, no `rexguest` section). The game code lives in
`predator_code.dll`, which both the development build and the first start build the same way
(`predator.exe --fcp-build-game-code`, `src/fcp/game_code_build.cpp`).

Across the DLL boundary:

- **Hooks:** `predator.exe` exports every `REX_HOOK*`/`REX_STUB*`/`REX_DETOUR` (`REX_TITLE_HOOK_EXPORTS`). The
  generated code calls the hook names; the DLL links them from an import library made from the export table of the
  running executable.
- **Originals:** a hook calls the original game function through a delay import from `predator_code.dll`; the DLL
  exports exactly those names.
- **Kernel and XAM:** the executable has wrappers for every kernel/XAM export of the SDK, so it does not depend on
  which ones the game imports.
- **Checks when loading:** an ABI hash (SDK headers, codegen, title configuration, build flags, LLVM version, ISA
  level, PGO profile), a hash of the executable's export names and the SHA-256 of `default.xex`. Any mismatch
  rebuilds the DLL.

Measured: the code generated on the first start is byte-identical to the development build's (305 of 305 files);
CPU time per frame with the DLL is the same as with the game code linked into the executable (within the spread
of one build).

## 3. Hooks

All game-specific behaviour is attached through hooks on named game functions:

| Group | Examples | Source |
|---|---|---|
| Direct3D library | draw, clear, resolve, tiling, queries, shader creation | `src/fcp/d3d_hooks.cpp` |
| Engine | main loop, frame timer, video playback, command queue, relaunch, map editor launcher | `src/fcp/engine_hooks.cpp`, `devstart.cpp`, `relaunch.cpp` |
| Input | the game's look input (mouse look), pad state | `src/fcp/pc_input.cpp`, `imports.cpp` |
| Camera | field of view (`CCamera_Update`) | `src/fcp/camera_fov.cpp` |
| Frame rate corrections | landing, swimming, boats, timers, fire rate, path following | `src/fcp/frame_coupling.cpp` |

Rules: addresses stay in the title configuration (`config/predator/*.toml`), never in C++. A hook without a
verified address stays off. Every correction of the game's behaviour has an environment switch back to the original
(see [DEBUG-SWITCHES.md](DEBUG-SWITCHES.md)). Mid-function (mid-asm) hooks are not used; the build stops if one
appears.

## 4. Runtime

The ReXGlue SDK provides the Xbox 360 kernel (`xboxkrnl`), XAM, the file system (the disc image is mounted read
only as `game:` and `d:`), threads and synchronisation, audio and input. The GPU side uses the SDK's **null Xenos
driver**: it still processes the command ring that the game's own D3D library writes (fences, callbacks, vblank,
swap), but draws nothing. A local SDK interface, `IHostPresenter`, lets the project's renderer take the window and
the present calls from the null driver (`rexglue/UPSTREAM.md`, change 10).

## 5. Renderer (direct D3D HLE)

The renderer (`src/renderer/`, bridged in `src/fcp/render_bridge.cpp`) replaces the game's Direct3D functions
instead of emulating the Xenos GPU:

- **Replaced:** `DrawIndexedVertices`, `DrawVertices`, `DrawVerticesUP`, `ClearF`, `Resolve`, `BeginTiling`,
  `EndTiling`, `SetPredication`, the occlusion query and conditional rendering functions. The originals of these do
  not run.
- **Observed:** shader creation (translation into the host format), object creation where needed.
- **Left original:** all state setters (they write the device structure, which the renderer reads), fences,
  callbacks and `Swap`.
- **Host API:** Vulkan (default) or Direct3D 12 through NVRHI (MIT, in the version patched by King Kong
  Recompiled). Occlusion queries go to the native query objects. Parts of the King Kong Recompiled renderer are used
  as libraries; the draw path is the project's own.
- **Tiling, targets, resolve:** each pass is drawn once over the whole target; the predication tiles of the console
  have no meaning on the host. Resolve targets stay host textures.
- **Caches:** guest textures and buffers are watched with the SDK's write watch instead of being hashed every frame;
  pipeline lists and the Vulkan driver cache are kept in the user folder, pipelines are created ahead of time.
- **Options:** render scale, shadow map scale, anisotropic filtering, field of view, Hor+ widescreen (HUD and menus
  stay 16:9 in the middle), MSAA, SMAA 1x and the game's gamma ramp (see [CONFIGURATION.md](CONFIGURATION.md)). All default to the console's look.

## 6. Shader translation and the shader pack

`fcshaders` (`tools/shaders/`) translates the game's Xenos microcode shaders to HLSL and compiles them with DXC
to DXIL (Direct3D 12) or SPIR-V (Vulkan). Its core is XenosRecomp in the revision of King Kong Recompiled (both
MIT), which reads the early shader container format the game uses.

- **Disc shaders:** measured 1,958 of 1,958 unique shaders of the disc translate and compile in both formats. The
  pack (`predator.<spirv|dxil>.fcpack`) is built on the first start from the player's disc and stored in the user
  folder; nothing of it is shipped.
- **Shaders compiled at run time:** the game compiles about 86 shaders while it runs; they go through the same
  translator and are cached on disk.
- Without a pack (`FCP_SHADER_PACK_BUILD=0` or a failed build) the renderer translates every shader at run time
  and caches it.

## 7. Audio and input

- **Audio:** the SDK's APU and XMA decoder; XMA, WMA and MP3 are decoded with FFmpeg (LGPL, as the shared library
  `rexffmpeg.dll`), the mix is played through SDL 3.
- **Controllers:** up to four players through SDL 3 (or XInput only), with remapping, dead zone and rumble strength.
- **Keyboard and mouse:** the keyboard acts as player 1's pad; mouse look is added to the angles of the game's own
  look input function (`Player_LookInput`) after the original has run, in fixed degrees per count. No stick
  emulation, no frame rate dependence (measured at 30 and 60 frames/s).
- **Button prompts:** the renderer replaces the two font pages that carry the button glyphs with redrawn copies
  for the device used last (keyboard, PlayStation, Switch or the original Xbox symbols).

## 8. User folder

Chosen once at start (`src/fcp/user_paths.cpp`):

1. `FCP_USER_ROOT=<folder>`,
2. `portable.txt` beside `predator.exe`: the executable's folder,
3. otherwise `%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp`.

| Path in the user folder | Content |
|---|---|
| `predator.toml`, `predator_input.toml` | settings ([CONFIGURATION.md](CONFIGURATION.md)) |
| `predator.log`, `predator.log.prev`, `predator.gen<n>.log` | log, previous run, relaunch generations |
| `logs\` | traces, CSV files, dumps |
| `user\` | profiles and save games (SDK user root) |
| `game_code\` | `predator_code.dll`, its PDB, `build.log` |
| `shaders\` | shader pack built on the first start |
| `cache\` | shader and pipeline caches |

The game restarts itself for every level change (as on the console). `src/fcp/relaunch.cpp` starts the new process
and hands over window position, size, maximised and fullscreen state; the new window opens in place.

## 9. First start

1. `predator.exe` reads `[game] image` from `predator.toml` (or `FCP_GAME_ISO`); without it, it writes the file
   and asks for the path.
2. `default.xex` is read from the image with the project's own XDVDFS reader and checked by SHA-256. Other
   versions are refused with a message.
3. `compiler\bin\rexglue.exe codegen` generates the C++ code into a work folder.
4. `compiler\bin\clang.exe` compiles it (largest files first, jobs = min(logical processors, free memory /
   640 MB), below-normal priority), `compiler\bin\lld-link.exe` links `predator_code.dll`.
5. The DLL is loaded and checked (section 2).
6. The shader containers are unpacked from the disc archives (LZMA SDK decoder) and `fcshaders.exe pack` writes the
   shader pack.
7. The work files are deleted; DLL, PDB and pack stay in the user folder.

A progress window shows phases, progress and remaining time; closing it asks first and keeps finished objects for
the next start. Measured on a 16-core desktop CPU: 40.7 s game code and 17.0 s shader pack; limited to 4 cores /
8 threads: 89.9 s and 88.6 s. The second start loads the DLL in 14 ms.

The release folder is assembled by `tools/dist.ps1` and contains nothing derived from the game (it refuses game
code, generated files, shader packs, caches, saves and any binary with a `rexguest` section).

## 10. Local SDK changes

`rexglue/` is a frozen copy of the ReXGlue SDK (BSD-3-Clause) at commit `351bc94d`. Every local change carries a
comment starting with `FCP` and is listed with its reason and its switch back in
[`rexglue/UPSTREAM.md`](../../rexglue/UPSTREAM.md). Among them: dependencies outside the source tree, optional Xenos
host drivers, the disc image as game root, kernel time stamp fields, the `[function_names]` table, codegen fixes
found by the semantic test, the host presenter interface, `[fp_rules]`, FFmpeg as its own DLL, audio and XMA
statistics, the freestanding compile of the generated code and the export of title hooks from the executable.
