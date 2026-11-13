# Far Cry Instincts Predator: static recompilation for Windows

An unofficial native Windows port of the Xbox 360 game *Far Cry Instincts Predator* (both campaigns, *Instincts*
and *Evolution*). The game's PowerPC code is translated ahead of time to C++ and compiled for x86-64 with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk); the Xbox 360 kernel, XAM, audio and input are provided by the
SDK's runtime, and the game's Direct3D calls go to a native Vulkan / Direct3D 12 renderer with the game's shaders
translated ahead of time.

The project is a beta candidate (version 0.1.0). It has not been released yet.

## Legal

- **You need your own, legally obtained copy of the game.** This project contains no part of the game: no game code,
  no game data, no shaders, no videos, no sounds, no textures. Neither this repository nor a release built from it
  includes anything derived from the game.
- The game code is translated from *your* disc image on *your* PC the first time you start the game, and it stays in
  your user folder. Do not share the files the first start creates (`predator_code.dll`, shader packs, caches).
- Please do not ask for, or post, game files or links to them in this project.
- *Far Cry* is a trademark of Ubisoft Entertainment. Xbox and Xbox 360 are trademarks of Microsoft Corporation. All
  other trademarks belong to their owners. This project is not affiliated with, endorsed by or sponsored by Ubisoft
  or Microsoft.

## Features

Everything listed here was measured in test runs (frame dumps, logs, scripted input) on a Windows PC. Points that are
not yet verified on real hardware are listed under [Known issues](#known-issues).

- **Both campaigns.** All 19 story levels load; all level transitions of *Instincts* and *Evolution* and both end
  credits run (scripted test runs). Saves are written at checkpoints and "Resume Game" restores level and position
  after a restart.
- **Native renderer** on Vulkan (default) or Direct3D 12, the game's shaders translated ahead of time into a shader
  pack. In the scenes compared, the picture matches reference captures made with the Xenia emulator.
- **Frame rate 30 (as the console), 60 or 120.** Game time runs at wall-clock speed at every rate (ratio 1.000), and
  places where the game logic depends on the frame count are corrected (see
  [Frame rate and game logic](#frame-rate-and-game-logic)).
- **Graphics options:** render resolution scale (up to 4x, or `auto` = the output height, e.g. 2x at 1440p), shadow
  map resolution, anisotropic filtering up to 16x, field of view, MSAA as the console or 2x/4x/8x, optional SMAA 1x,
  the game's gamma ramp. All of them default to the console's look.
- **Mouse and keyboard:** direct mouse look on the game's view angles (fixed degrees per count, no stick emulation,
  the same at every frame rate), optional acceleration and smoothing by time, zoom scaling, invert Y; every pad
  button and stick direction bindable to keys, mouse buttons or the wheel.
- **Controllers:** up to four players through SDL 3 (Xbox, PlayStation, Switch and other pads, hot plug with stable
  player slots), or XInput only; button remapping with Xbox or PlayStation names, extra dead zone, rumble strength.
- **Button prompts** in hints and menus follow the device used last: the bound key for keyboard and mouse, PlayStation
  or Switch symbols for those pads, or the original Xbox symbols.
- **Settings menu (F10 or Back + Start)** for all settings; the game pauses while it is open.
- **Comfort:** skip the start-up logo and intro videos, frame rate display, game language (English, French, German,
  Spanish, Italian, or the Windows language; checked for texts), pause when the window loses focus, borderless
  fullscreen (Alt+Enter toggles), window size and position kept across the game's own restarts.
- **No gameplay changes.** Difficulty, mechanics and aim assist stay as on the console. Every correction has an
  environment switch back to the original behaviour.

## Documentation

- [Architecture](docs/en/ARCHITECTURE.md): recompiler, game code DLL, hooks, renderer, shaders, first start.
- [Building from source](docs/en/BUILDING.md): requirements, build configurations, release folder, tests.
- [Configuration](docs/en/CONFIGURATION.md): every key of `predator.toml` and `predator_input.toml`, the F10 menu.
- [Frame rate and game logic](docs/en/FRAMERATE.md): the frame rate corrections and the open points in detail.
- [Test tools](docs/en/TESTING.md) and [switches back to the original](docs/en/DEBUG-SWITCHES.md), for contributors.

## Installation and first start

1. Extract the release zip into a folder you can write to (for example `Documents\Games`).
2. Start `predator.exe` once. It creates the user folder `%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp` with the
   settings file `predator.toml` and asks you to set your disc image.
3. Open `predator.toml` and enter the path of your disc image:

   ```toml
   [game]
   image = "D:/Games/Far Cry Instincts Predator.iso"
   ```

4. Start `predator.exe` again. A progress window prepares the game for your PC, once:
   - reads `default.xex` from the disc image and checks its SHA-256,
   - translates it to C++ (`compiler\bin\rexglue.exe`),
   - compiles and links it (`compiler\bin\clang.exe`, `compiler\bin\lld-link.exe`) into
     `game_code\predator_code.dll` in the user folder,
   - builds the shader pack from the disc's shaders (`fcshaders.exe`).

   Measured: on a 16-core desktop CPU about 41 s for the game code and 17 s for the shader pack (the game is running
   after about one minute); limited to 4 cores / 8 threads about 90 s plus 89 s (about 3 minutes). Slower CPUs take
   longer. The game starts by itself afterwards; later starts load the prepared code in a fraction of a second.

If you close the progress window during the build, it asks first; what is finished is kept and the next start
continues. If a step fails, a window shows the end of `game_code\build.log`. A new release that changes the
recompiler or the hooks prepares the game code again on its first start.

To keep everything next to `predator.exe` instead of `%LOCALAPPDATA%` (portable), create an empty file `portable.txt`
beside it.

## Requirements

- Windows 10 or 11, 64-bit.
- A CPU with AVX2 (x86-64-v3: Intel Haswell, AMD Zen or newer).
- A GPU with Vulkan or Direct3D 12 (tested on an NVIDIA RTX 40-series card).
- About 250 MB for the release folder, plus temporary space for the first start (the work files are deleted after
  the build).
- Your disc image of the game (see below).

The release folder includes the Microsoft Visual C++ runtime DLLs, so no separate redistributable is needed.

## Providing the game

The game is read directly from a disc image (`.iso`) of the retail disc *Far Cry Instincts – Predator* (Xbox 360,
USA/Europe release, languages En/Fr/De/Es/It), without a title update. Nothing has to be extracted. The image is only
read, never written.

The first start checks `default.xex` in the image against the SHA-256
`0ABD35C4E4C4F39DD330CEBFE051D28A313EF2BDCD80921CACA8F1F2956B0B56` and refuses other versions with a message. Other
releases of the game are not supported.

`[game] image` may also point to a folder with the extracted disc. A relative path is relative to the user folder.
The environment variable `FCP_GAME_ISO` overrides the setting.

## Building from source

Details, CMake options and tests: [docs/en/BUILDING.md](docs/en/BUILDING.md).

Requirements (as used for development):

- Windows 11 x64, Visual Studio Build Tools 2022 with the C++ workload and Windows SDK 10.0.26100 (only the SDK,
  the linker libraries and `vcvars64.bat` are used; the compiler is LLVM),
- CMake 3.25 or newer, Ninja, Git, Python 3 (for the build scripts),
- your disc image (the development build translates the game code from it as part of the build).

One-time setup:

```bat
powershell -File tools\get_llvm.ps1                rem LLVM/clang 22.1.8 into toolchain\llvm (SHA-256 checked)
powershell -File tools\get_deps.ps1                rem ReXGlue SDK dependencies into third_party\rexglue-deps
powershell -File tools\get_renderer_deps.ps1       rem renderer dependencies into third_party\renderer-deps
python -m venv toolchain\venv
toolchain\venv\Scripts\python.exe -m pip install pycryptodome capstone
```

All dependencies are pinned to fixed commits or release archives; nothing is installed system-wide. The LZMA SDK
decoder is fetched by `build.cmd` and `tools\dist.cmd` on first use (`tools\get_lzma.ps1`).

Build:

```bat
set FCP_GAME_ISO=D:\Games\Far Cry Instincts Predator.iso
cmd /c build.cmd release
```

`build.cmd` configures the `release` preset (also `relwithdebinfo`, `debug`), translates `default.xex` from the
image to `generated\default`, builds `predator.exe` (without game code) and then `predator_code.dll` the same way the
first start does. Output: `out\build\release\bin\`. The development build puts a `portable.txt` next to the EXE, so
settings, saves and caches stay under `bin\`.

Shader pack for the development build (the release builds it on the first start):

```bat
cmd /c tools\renderer_pack.cmd release spirv       rem or dxil for Direct3D 12
```

Release folder and zip (no game data; refuses anything that comes from the game). This needs no disc image: after the\none-time setup, it works in a fresh clone without `FCP_GAME_ISO`.

```bat
cmd /c tools\dist.cmd release
```

The result is `out\dist\Far-Cry-Instincts-Predator-Recomp-v<version>-win64\` with a zip, the source archive
`Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip` (exact sources of the LGPL/GPL components FFmpeg, libmspack
and the binutils disassembler, with license texts and build notes) and `SHA256SUMS` next to it.
`tools\dist.ps1 -NoVcRuntime` leaves out the Visual C++ runtime DLLs. The release workflow
`.github/workflows/release.yml` runs the same steps on a tag (only the `dist` target, no disc image); the release
checklist is [docs/en/RELEASING.md](docs/en/RELEASING.md), the changes per version are in [CHANGELOG.md](CHANGELOG.md).

## Running

Start `predator.exe`. Logs are written to `predator.log` in the user folder (the previous run is kept as
`predator.log.prev`).

| Key | Action |
|---|---|
| F10, or Back + Start on a pad | open / close the settings menu |
| Alt+Enter | switch between window and fullscreen (the choice is stored in `predator.toml`) |

Environment switches for the first start:

| Variable | Effect |
|---|---|
| `FCP_GAME_CODE_REBUILD=1` | build the game code again even if it matches |
| `FCP_GAME_CODE_BUILD=0` | never build; show an error instead |
| `FCP_GAME_CODE_JOBS=<n>` | number of compiler processes (default: logical processors, limited by free memory, 640 MB each) |
| `FCP_GAME_CODE_CPUS=<n>` | run all build processes on the first n logical processors |
| `FCP_GAME_CODE_KEEP=1` | keep the work folder (generated code, objects) |
| `FCP_SHADER_PACK_BUILD=0` | do not build the shader pack (the renderer then translates shaders while playing) |
| `FCP_USER_ROOT=<folder>` | use this user folder |

## Configuration

Both files are written with their defaults and comments when they are missing, in the user folder. Every key of
`predator.toml` is also an environment variable (shown in the file); a variable set in the environment wins over the
file. Most settings can be changed in the F10 menu; those marked "restart" there take effect at the next start.
The full reference is [docs/en/CONFIGURATION.md](docs/en/CONFIGURATION.md).

### `predator.toml`

| Section | Key | Default | Values |
|---|---|---|---|
| `[game]` | `image` | `""` | path of the disc image or extracted folder (`FCP_GAME_ISO`) |
| | `pause_on_focus_loss` | `false` | `true` = the game opens its pause menu, sound is muted and vibration stops while the window has no focus |
| | `skip_intro` | `false` | `true` = skip the logo and intro videos at start; story cutscenes stay |
| | `fps_counter` | `false` | `true` = frame rate and frame time display in the corner |
| | `language` | `"english"` | `english`, `french`, `german`, `spanish`, `italian`, `auto` (Windows display language); restart |
| `[display]` | `mode` | `"windowed"` | `windowed`, `borderless`, `exclusive` |
| | `width`, `height` | `0` | window client size in pixels; `0` = 1280x720 |
| | `monitor` | `0` | `0` = primary, `1`..`n` = display number |
| | `fps` | `30` | `30` (console), `60`, `90` (Steam Deck OLED display), `120`; restart |
| | `vsync` | `"auto"` | `auto` (on up to 60 frames/s, off above), `on`, `off` |
| `[graphics]` | `backend` | `"auto"` | `auto` (`renderer.toml` next to the EXE, else Vulkan), `vulkan`, `d3d12` |
| | `msaa` | `"console"` | `console` (2x on the screen, 4x on the reflection, as the console), `off`, or `2`, `4`, `8` samples on the screen-sized targets (limited by the GPU); restart. Older files: `true` = console, `false` = off |
| | `smaa` | `false` | `true` = SMAA 1x edge smoothing on the finished picture; switches at once |
| | `gamma` | `true` | the game's gamma ramp (its brightness setting) |
| | `scale` | `1` | render resolution of screen-sized targets: `1` = 1280x720 as the console, `auto` = output height / 720, or a factor / percentage from 0.5 to 4 (`1.5`, `150%`) |
| | `shadow_scale` | `1` | shadow map resolution as a multiple of the console's 1344x1344 (up to 4) |
| | `aniso` | `1` | anisotropic filtering 1-16 (`1` = as the game sets it) |
| | `fov` | `1` | field of view as a multiple of the game's (0.5 to 2); the weapon keeps its own view |
| `[input]` | `file` | `"predator_input.toml"` | keyboard, mouse and pad file (relative to the user folder) |
| | `button_prompts` | `"auto"` | `auto` (device used last), `xbox` (console), `keyboard`, `playstation`, `switch` |

### `predator_input.toml`

| Section | Key | Default | Values |
|---|---|---|---|
| `[keyboard]` | `enabled` | `true` | keyboard as player 1's pad (`FCP_KEYBOARD=0` turns it off) |
| `[mouse]` | `enabled` | `true` | mouse look and buttons (`FCP_MOUSE=0` turns it off) |
| | `sensitivity` | `1.0` | degrees per 10 counts; `sensitivity_x` / `sensitivity_y` set the axes separately |
| | `invert_y` | `false` | invert vertical look |
| | `zoom_scale` | `"fov"` | while zoomed: `fov` (field of view / 90 degrees, as the game scales the stick), `tan`, `off` |
| | `zoom_sensitivity` | `1.0` | extra factor while zoomed (0.05-10) |
| | `acceleration` | `0.0` | extra gain per count/ms of mouse speed; `0` = off |
| | `acceleration_cap` | `2.0` | highest gain of the acceleration (>= 1) |
| | `smoothing` | `0` | milliseconds each motion is spread over (up to 200); `0` = off |
| `[pad]` | `backend` | `"sdl"` | `sdl` (all controllers, up to four players), `xinput`, `off`; restart |
| | `dead_zone` | `0.0` | extra radial dead zone (0.05-0.9); `0` = only the game's own |
| | `rumble` | `100` | vibration strength in percent; `0` = off |
| | `pad_bind_<button>` | the button itself | controller buttons for an Xbox pad button (`a b x y lb rb lt rt ls rs start back up down left right`); PlayStation names (`cross circle square triangle l1 r1 l2 r2 l3 r3 options select share create`) and `south east west north` work too; several with commas, `none` for none |
| `[bindings]` | `<target>` | see below | keys for a pad button or stick direction; several with commas, `none` for none |

Default key bindings (`[bindings]`):

| Target | Keys | In the game |
|---|---|---|
| `a` | Space | jump; select in menus |
| `b` | C, Backspace | kick; back in menus |
| `x` | E, R | reload / use |
| `y` | Q | feral mode |
| `lb` | F | switch projectile |
| `rb` | G | next selection |
| `lt` | right mouse button | secondary fire |
| `rt` | left mouse button | fire |
| `ls` | Shift, Ctrl | stance |
| `rs` | V, middle mouse button | zoom |
| `start` | Enter, Escape | pause menu |
| `back` | Tab | scoreboard |
| `up`, `down`, `left`, `right` | arrow keys, 4, 3, 1, 2 | binoculars, special weapon, primary weapon, secondary weapon |
| `lup`, `ldown`, `lleft`, `lright` | W, S, A, D | move |
| `rup`, `rdown`, `rleft`, `rright` | I, K, J, L | look with the keyboard (through the stick) |

Key names: `a`-`z`, `0`-`9`, `f1`-`f24`, `num0`-`num9`, `space`, `enter`, `escape`, `tab`, `backspace`, `shift`,
`ctrl`, `alt`, `up`, `down`, `left`, `right`, `insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, `minus`,
`equals`, `comma`, `period`, `slash`, `semicolon`, `apostrophe`, `lbracket`, `rbracket`, `backslash`, `grave`,
`mouse1`-`mouse5`, `wheelup`, `wheeldown`, `0x<virtual key>`. A key that the file binds to one target is removed from
the defaults of the others.

## Settings menu (F10)

F10 or Back + Start opens the menu over the game; F10, Escape, Back + Start, B or "Close" close it. While it is open
the game receives no input and opens its own pause menu (as with the console's guide button); after closing, the
game stays in its pause menu until you resume.

| Tab | Contents |
|---|---|
| Display | display mode, monitor, window size, frame rate, VSync |
| Graphics | graphics API, render scale, shadow resolution, anisotropic filtering, field of view, MSAA, SMAA, gamma ramp |
| Input | mouse sensitivity (both axes and vertical), invert Y, acceleration and its limit, smoothing, zoomed behaviour, keyboard, mouse, controller driver, dead zone, vibration, button prompts, all 24 key bindings (Change, Clear, Default) |
| Game | pause on focus loss, skip intro videos, frame rate display, language |
| About | version, commit, graphics API, file paths, licence notice |

Changes are written into the files at once (comments in the files are kept). Settings that only apply at the next
start are marked "restart required". A key that an environment variable sets is shown as "set by environment".

## Frame rate and game logic

The game was written for 30 frames per second. At higher frame rates some of its logic ran per frame instead of per
second, or relied on a step of 1/30 s. Each place below was measured at 30, 60 and 120 frames/s (usually three runs
per rate) and is corrected only above 30 frames/s; at the default of 30 the game runs as on the console. Each
correction can be switched off for comparison (column "Original"). Causes, more measurements and the experiments:
[docs/en/FRAMERATE.md](docs/en/FRAMERATE.md).

| Area | Original at 120 frames/s (measured) | Now | Original |
|---|---|---|---|
| Fall damage | landing speed taken ~2.4 frames before impact: the same jump landed at 4.60 instead of 4.09 m/s, more damage | as at 30 | `FCP_FRAME_FALL=1` |
| Drowning damage | damage per timer tick times frame time: 2.9 instead of 7.3-7.9 HP/s, death after 33.7 instead of 11.2 s | as at 30 | `FCP_FRAME_TIMERS=1` |
| View bob in water | summed per frame: tilt up to 13.8 instead of 3.3-5.0 degrees | as at 30 | `FCP_FRAME_BOB=1` |
| Boats (computer-controlled and the player's) | drag applied per physics step: patrol boat 5.5 instead of 11.8 m/s, player's boat slower | the console's water model rate; 11.9 m/s and the same speeds as at 30 | `FCP_FRAME_WATER=1` |
| Swimming: buoyancy and currents | applied per frame: after the jump from the helicopter in `13_black_forest` the player stayed under water and drowned | 0.367 m/s rise at every rate, surface after 3.8-4.7 s | `FCP_FRAME_SWIM=1` |
| First landing after take-off | flight over a ramp lasted 0.02 instead of 0.60 s | at least as long as at 30 | `FCP_FRAME_LANDING=1` |
| Entering water | swim mode one step after crossing the water line: too shallow, surfaced earlier | the same depth as at 30 | `FCP_FRAME_DIVE=1` |
| Stick turn ramp | 1 s of full stick turned 253 instead of 250 degrees | 250 degrees at every rate | `FCP_FRAME_TURN=1` |
| Path following of computer-controlled characters | look-ahead of one frame step (3-4 cm instead of 13-15 cm) | the console's look-ahead | `FCP_FRAME_PATH=1` |
| Fire rate (assault rifle) | fire test per frame: 9.6 instead of 9.0 shots/s, faster after reloading | the console's 1/30 s grid: 8.97 shots/s at 30, 60 and 120 | `FCP_FRAME_FIRE=1` |

Measured without a dependency on the frame rate: walking speed, jump height, breath under water, the mounted machine
gun's fire rate, video playback, menu key repeat, game time against wall time.

**At most 120 frames/s.** The frame rate setting accepts 30, 60, 90 and 120; the game logic was measured at 30, 60
and 120 and is not checked above 120. The corrections above apply to every rate above 30.

**Still open** (measured, not corrected):
- Computer-controlled characters think every fourth frame (7.5 times per second at 30, 30 times at 120). In
  `05_mines` their timeline at 120 is 0.12-0.44 s earlier; two latencies of three frames explain part of it. One
  character in `05_mines` stops after about 25 s at 120 (once also at 30); the cause is not known.
- A scripted drive in `21_dunea` starts 0.35-0.47 s earlier at high frame rates.
- Wheeled vehicles are 1.5-3 % faster at 120.
- Reloading is about 0.05 s shorter at high frame rates; machine gun and shotgun series are not fully measured.
- Not yet examined: ladder and vine ends, slope edges, particles and HUD animations.

## Steam Deck

Tested on a Steam Deck OLED (SteamOS 3.9, Proton 10.0), started with Proton from a folder in Game Mode by test
scripts (adding it to Steam as a non-Steam game has not been tried yet):

- The first start (building the game code and the shader packs on the Deck) takes about 7-8 minutes; later starts
  load a level in about 10 seconds. The user folder is in the Proton prefix's `AppData\Local`.
- The built-in controls are detected through SDL3. The Deck is detected and defaults to 30 frames/s.
- Measured frame rates (build without the profile-guided optimisation described in BUILDING): at a 30 frames/s target
  30.0 in all tested levels; at 60, 56-60 depending on the level; at 90, 58-75. The limit is the CPU (main thread)
  together with the handheld's ~15 W power limit. Vulkan and Direct3D 12 perform the same within about 2 frames/s.
- The first visit of a level can stutter once (130-450 ms) while the graphics pipelines are created.

Higher frame rates on the Deck are work in progress.

## Architecture

More in [docs/en/ARCHITECTURE.md](docs/en/ARCHITECTURE.md).

| Part | What it is |
|---|---|
| `predator.exe` | the runtime: ReXGlue kernel/XAM emulation, hooks into the game (renderer, input, frame rate corrections, settings), the first-start build driver and progress window, a disc image reader |
| `predator_code.dll` | the recompiled game code, generated from `default.xex` (about 150 C++ files) and compiled on the player's PC; no C runtime, no entry point |
| `rex*.dll` | the ReXGlue SDK runtime libraries; `rexffmpeg.dll` holds the FFmpeg decoders for the game's XMA audio |
| `src/renderer` | native renderer on NVRHI (Vulkan, Direct3D 12), based on the King Kong Recompiled renderer |
| `tools/shaders`, `fcshaders.exe` | shader translator (XenosRecomp core) and packer for the shader pack |
| `config/predator` | title configuration: function names and addresses of all hooks, disc checksum |
| `rexglue/` | frozen copy of the ReXGlue SDK with local changes listed in `rexglue/UPSTREAM.md` |

Addresses of game functions are kept in the title configuration, not in the C++ code. The generated code is checked
on every build by a semantics test of the PowerPC instruction forms (about 41,700 cases, with expected values partly
from hardware test vectors).

## Known issues

- No complete playthrough by a person yet. Levels, transitions and saves were tested with scripted runs; mission
  objectives, bosses, vehicles and mid-level videos still need real play.
- Not yet verified with real hardware: controllers and rumble (tested with virtual SDL pads), mouse feel, the F10
  and Alt+Enter keys from a real keyboard (tested with injected window messages), audio by ear.
- Ultrawide (Hor+, `[graphics] aspect`): the HUD, menus, videos and loading screens stay in the 16:9 centre. Binocular
  and scope masks, subtitles, feral vision and the D3D12 backend have not been checked at ultrawide sizes yet.
- Exclusive fullscreen and monitor selection are not tested.
- User folder paths with non-ASCII characters are not tested for the shader pack.
- `03_river` is slightly brighter than the console reference.
- The runtime's developer overlays (F3, F4, F7, the backtick console) are off; `FCP_DEV_OVERLAYS=1` turns them on for development.
- The map editor on the disc is not supported; choosing it returns to the main menu.
- Multiplayer (split screen, system link, online) is not tested.
- The open frame rate points above.

## Credits

This port builds on the work of others. The full list of components, versions and licenses is in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md) (repository) and `THIRD_PARTY_NOTICES.md` with the license texts in
`licenses\` (release).

| Project | Used for | License |
|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) | recompiler and runtime (frozen copy in `rexglue/`) | BSD-3-Clause |
| [Xenia](https://github.com/xenia-project/xenia) | kernel, XAM, file system, GPU and APU code inside ReXGlue | BSD-3-Clause |
| [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) | recompiler code inside ReXGlue | MIT |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | shader translator core | MIT |
| [King Kong Recompiled](https://github.com/TekRantGaming/king-kong-recompiled) | native renderer parts and shader translator adapter | MIT |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | graphics abstraction over Vulkan and Direct3D 12 | MIT |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | shader compilation (`dxcompiler.dll`) | NCSA, Microsoft terms |
| [SDL 3](https://github.com/libsdl-org/SDL) | controllers, window, audio output | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | settings menu and frame rate display | MIT |
| [FFmpeg](https://ffmpeg.org) | XMA, WMA and MP3 decoding (`rexffmpeg.dll`, shared library) | LGPL-2.1-or-later |
| [libmspack](https://github.com/kyz/libmspack) | LZX decompression | LGPL-2.1 |
| GNU binutils PowerPC disassembler | inside the recompiler tool `rexglue.exe` only | GPL-2.0-or-later |
| [LLVM](https://llvm.org) (clang, lld) | compiler and linker, shipped for the first start | Apache-2.0 with LLVM exception |
| [SMAA](https://github.com/iryoku/smaa) | optional SMAA 1x (shader and tables as packaged by SCDA-XBERecomp, by way of the author's *Far Cry Instincts* port) | MIT |
| [LZMA SDK](https://www.7-zip.org/sdk.html) | decoder for the disc's archives (shader pack) | public domain |
| spdlog, fmt, toml++, SIMDe, xxHash, ExCrypt, TLSF, CLI11, inja, nlohmann/json, stb_image, Vulkan-Headers, DirectX-Headers | support libraries | MIT, BSD, Apache-2.0 or public domain (see the notices) |
| [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp) | the first-start progress window and the SMAA packaging (by way of the author's *Far Cry Instincts* port) | MIT |
| the author's earlier *Far Cry Instincts* (Xbox) recompilation | settings file and menu design, input layer, frame rate audit method, first-start build and release scripts | same author |

No code from GPL-3 projects is used. The exact sources of the LGPL/GPL components (FFmpeg, libmspack, the binutils
disassembler) are published with every release as `Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip`.

## Licence

The code of this project is released under the MIT License, see [`LICENSE`](LICENSE). This covers only the
project's own code: no game code or game data is part of this repository or of a release.

The licenses of the third-party components are listed in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
