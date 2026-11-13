# Building from source

The build never puts game code or game data into the repository: the game code is generated from your disc image
into `generated\` and `out\`, both ignored by Git.

## Requirements

As used for development:

- Windows 11 x64.
- Visual Studio Build Tools 2022 with the C++ workload and Windows SDK 10.0.26100. Only the SDK, the linker
  libraries and `vcvars64.bat` are used; the compiler is LLVM.
- CMake 3.25 or newer, Ninja, Git, Python 3 (for the setup of the build venv).
- Your disc image of the game (see the README). The development build translates the game code from it.

## One-time setup

```bat
powershell -File tools\get_llvm.ps1               rem LLVM/clang 22.1.8 into toolchain\llvm (SHA-256 checked)
powershell -File tools\get_deps.ps1               rem ReXGlue SDK dependencies into third_party\rexglue-deps
powershell -File tools\get_renderer_deps.ps1      rem renderer dependencies into third_party\renderer-deps
python -m venv toolchain\venv
toolchain\venv\Scripts\python.exe -m pip install pycryptodome capstone
```

Every dependency is pinned to a commit or a release archive with a checksum; nothing is installed system wide and
no Git configuration is changed. The renderer dependencies are King Kong Recompiled (for its renderer parts and
NVRHI), Vulkan-Headers, fmt, xxHash and a DXC release; no Vulkan SDK is needed. `tools\get_deps.ps1 -Optional` also fetches glslang and Catch2 (only for the SDK's
Xenos drivers and unit tests). The LZMA SDK decoder is fetched by `build.cmd` on first use (`tools\get_lzma.ps1`).

## Build

```bat
set FCP_GAME_ISO=D:\Games\Far Cry Instincts Predator.iso
cmd /c build.cmd release
```

Configurations: `release` (default), `relwithdebinfo`, `debug` (CMake presets of the same names, Ninja, clang).
Steps of `build.cmd`:

1. configure (only when `build.ninja` is missing or outdated),
2. `predator_coverage`: read `default.xex` from the image, run the codegen into `generated\default`, check that
   every known entry point was generated,
3. `predator.exe` (without game code), then `predator_code.dll` through `predator.exe --fcp-build-game-code`
   (the same path as the first start of a release), then the semantic test.

Output: `out\build\<config>\bin\predator.exe` and `bin\game_code\predator_code.dll`. The build puts a
`portable.txt` beside the executable, so settings, saves, logs and caches of a development build stay under `bin\`.

Environment variables read by `build.cmd` (all optional; they are stored in the CMake cache on the first
configure):

| Variable | Default | Meaning |
|---|---|---|
| `FCP_GAME_ISO` | `assets\<image name>.iso` | your disc image (read only) |
| `FCP_DEPS_DIR` | `third_party\rexglue-deps` | SDK dependencies |
| `FCP_RENDERER_DEPS_DIR` | `third_party\renderer-deps` | renderer dependencies |
| `FCP_TOOLCHAIN_DIR` | `toolchain` | folder with `llvm\` and `venv\` |
| `FCP_PYTHON` | `<toolchain>\venv\Scripts\python.exe` | Python for the build steps |
| `FCP_VCVARS` | VS Build Tools 2022, else found with `vswhere` | `vcvars64.bat` |
| `FCP_JOBS` | Ninja's default | parallel jobs |
| `FCP_LZMA_DIR` | `third_party\lzma-sdk` | LZMA SDK decoder |

CMake options (`-D<name>=<value>` on a configure):

| Option | Default | Meaning |
|---|---|---|
| `FCP_GAME_CODE_DLL` | `ON` | `OFF` links the game code into `predator.exe` (the earlier layout, no first-start build) |
| `FCP_MARCH` | `x86-64-v3` | host ISA level of all code (AVX2); `x86-64-v2` or empty goes back to the preset's level |
| `FCP_PGO` | empty | profile-guided optimisation of the game code: `generate` or `use` (`tools\pgo.ps1`; the profile stays under `out\pgo`) |
| `FCP_SEMTEST` | `ON` | build and run the semantic test as part of the default build |
| `FCP_COVERAGE_CHECK` | `ON` | entry point check after the codegen |
| `FCP_RENDERER` | `ON` | build the native renderer |
| `FCP_XENOS_GPU` | `OFF` | also build the SDK's Xenos GPU emulation drivers (not used by the port) |
| `FCP_TRACY` | `OFF` | build the SDK with the Tracy profiler client |

## Shader pack (development build)

A release builds the shader pack on the first start. For a development build:

```bat
cmd /c tools\renderer_pack.cmd release spirv      rem or dxil (Direct3D 12), or both
```

The pack lands in `out\build\<config>\bin\shaders\predator.<spirv|dxil>.fcpack`; intermediate files go to
`out\pack\`. A pack with a matching stamp is not rebuilt. Without a pack the renderer translates shaders at run time.

## Release folder

```bat
cmd /c tools\dist.cmd release
```

This builds the CMake target `dist` (it depends only on `predator`, the shader tool, `rexglue.exe` and the import
libraries, never on the codegen) and runs `tools\dist.ps1`. Result in `out\dist\`:

- `Far-Cry-Instincts-Predator-Recomp-v<version>-win64\` and its zip,
- `Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip`: the exact sources of the LGPL/GPL components (FFmpeg,
  libmspack, the binutils disassembler) with licence texts and build notes,
- `SHA256SUMS`.

`dist.ps1` refuses anything that comes from the game: `predator_code.*`, generated code, objects, PDBs, `.xex`/
`.iso`, shader packs, caches, saves, logs, `portable.txt`, settings files and any EXE/DLL with a `rexguest`
section. `-NoVcRuntime` leaves out the Visual C++ runtime DLLs. A CI workflow draft is in
`.github/workflows/release.yml`; it builds only `dist`, without a disc image.

## Tests

| Command | What it checks |
|---|---|
| `cmd /c tools\semtest.cmd [config] [--sdk]` | semantic test of the PowerPC codegen (about 41,700 cases). Report in `out\build\<config>\semtest\report.txt`; exit code 1 on new failures. Known deviations are listed with a reason in `tests\semantics\known.toml`. `--sdk` also runs the SDK's own PPC tests |
| `cmd /c tools\renderer_tests.cmd [release\|debug] [--no-disc]` | the renderer on its own: resource, interface, pack/cache and backend tests; with the disc also tests on derived data under `work\` |
| `tools\boot\run.ps1 -Config release -Seconds 60 -Env "A=1;B=2"` | one test run of `predator.exe` (muted, 640x360 window, no dialogs, ends after `-Seconds`); only the process it started is ended |
| `tools\boot\run_chain.ps1 ...` | like `run.ps1`, follows the relaunch chain of level changes |
| `tools\level_sweep.ps1 [-Levels a,b] [-Seconds 90]` | starts every story level (all 19 without `-Levels`) with a key script, frame dumps and statistics; exit code 0 = all loaded without a crash |
| `tools\level_transitions.ps1 -Campaign instincts\|evolution` | plays a campaign as a chain of real level transitions (the player is placed into each level's end trigger) up to the credits |
| `tools\stability.ps1` | menu, two levels and two restarts in one chain; checks all logs and the Windows event log |
| `tools\perf_measure.ps1 -Level 21_dunea -Fps 120` | frame rate and CPU per frame over several runs |

The run scripts use `FCP_*` switches for scripted input, traces and dumps; see [TESTING.md](TESTING.md). Frame
dumps and logs are derived from the game: they stay under `out\` and the user folder and must not be committed.

## Git worktrees

In a worktree without its own `toolchain\`, `third_party\` and `assets\`, build with
`cmd /c tools\boot\wt_build.cmd <config>`. It takes these folders from the main checkout (`FCP_SHARED_ROOT`, by
default found with `git rev-parse --git-common-dir`) and writes the build log to `out\logs\build_<config>.log`.
The test scripts find the shared disc image the same way (`tools\shared_root.ps1`).
