# Changelog

All notable changes of Far Cry Instincts Predator Recomp. Versions follow `VERSION`; a release tag is `v<VERSION>`.
Statements about behaviour are measurements from test runs on a Windows PC unless marked otherwise; see the
[README](README.md) and [docs/en](docs/en) for the details.

## 0.1.1 (2026-11-13)

### Added

- **Exit Game in the F10 menu** (bottom left): asks "Exit to desktop?", then ends the game like the window's close
  button (exit code 0; tested in the main menu, in a level and in the game's pause menu). Works with a pad alone
  (A confirms, B goes back), e.g. on a Steam Deck in Game Mode.
- **Apply (restarts the game)** in the F10 menu, shown while a setting marked "restart required" was changed:
  restarts the game at once with window and display mode kept, back to the main menu; in a level it asks first.
  The language can now be switched from the menu this way (tested: German and French main menu after the restart; a
  save made before quitting loads with "Resume Game").

## 0.1.0 (beta, 2026-11-13)

First version: a native Windows x64 port of the Xbox 360 game *Far Cry Instincts Predator* (both campaigns,
*Instincts* and *Evolution*) by static recompilation. The release contains no game code and no game data; the game
code is translated from the player's own disc image on the first start.

### Features

- **Both campaigns.** All 19 story levels load; all level transitions of both campaigns and both end credits run
  (scripted test runs). Saves are written at checkpoints, and "Resume Game" restores level and position after a
  restart.
- **First start.** `predator.exe` reads `default.xex` from the disc image (`.iso` or extracted folder), checks its
  SHA-256, translates it to C++ and compiles it with the bundled LLVM into `predator_code.dll` in the user folder,
  then builds the shader pack. About one minute on a 16-core desktop CPU, about three minutes limited to
  4 cores / 8 threads; later starts load the prepared code at once. Other versions of the game are refused with a
  message.
- **Native renderer** on Vulkan (default) or Direct3D 12 with the game's shaders translated ahead of time. In the
  scenes compared, the picture matches reference captures made with the Xenia emulator.
- **Frame rate 30 (as the console), 60, 90 or 120.** Game time runs at wall-clock speed at every rate. Places where
  the game logic depends on the frame count are corrected above 30 frames/s: fall damage, drowning damage, view bob
  in water, boats, swimming, the first landing after take-off, entering water, the stick turn ramp, path following
  of computer-controlled characters, the assault rifle's fire rate. Each correction has an environment switch back
  to the original ([FRAMERATE.md](docs/en/FRAMERATE.md)).
- **Graphics options:** render resolution scale (up to 4x or `auto`), shadow map resolution, anisotropic filtering
  up to 16x, field of view, MSAA as the console or 2x/4x/8x, optional SMAA 1x, the game's gamma ramp, ultrawide Hor+
  (HUD, menus and videos in the 16:9 centre). All default to the console's look except Hor+ on outputs wider than 16:9.
- **Handhelds:** Steam Deck OLED (Proton 10) detected, defaults to 30 frames/s; ROG Xbox Ally X detected, defaults to
  120 frames/s in borderless fullscreen (118-120 frames/s measured).
- **Mouse and keyboard:** direct mouse look on the game's view angles, optional acceleration and smoothing, zoom
  scaling, invert Y; every pad button and stick direction bindable.
- **Controllers:** up to four players through SDL 3 (or XInput only), hot plug with stable player slots, button
  remapping, extra dead zone, rumble strength.
- **Button prompts** follow the device used last (keyboard keys, Xbox, PlayStation or Switch symbols).
- **Settings menu** (F10 or Back + Start) for all settings; the game pauses while it is open.
- **Comfort:** skip the start-up logo and intro videos, frame rate display, game language, pause on focus loss,
  borderless fullscreen (Alt+Enter), window size and position kept across the game's own restarts, portable mode
  (`portable.txt`).
- **No gameplay changes:** difficulty, mechanics and aim assist stay as on the console.
- **Release package:** the Visual C++ runtime is included; every release comes with
  `Far-Cry-Instincts-Predator-Recomp-v<version>-sources.zip`, the exact sources of the LGPL/GPL components (FFmpeg,
  libmspack, the GNU binutils PowerPC disassembler), and `SHA256SUMS`.
- **Optimised game code:** the first start compiles the game code with ThinLTO and the bundled PGO profile
  (`compiler\pgo\predator.profdata`, the repository's `config/predator/game_code.profdata`: function names and
  execution counters, no code or data of the game). Measured on a desktop CPU limited to 4 cores / 8 threads at
  90 frames/s: main thread 12-14 % less time per frame, process CPU per frame 8 % less, than the same build
  without the profile.

### Known limitations

- No complete playthrough by a person yet; levels, transitions and saves were tested with scripted runs. Mission
  objectives, bosses, vehicles and mid-level videos still need real play.
- Not verified with real hardware: controllers and rumble (tested with virtual SDL pads), mouse feel, the F10 and
  Alt+Enter keys from a real keyboard, audio by ear.
- Ultrawide: HUD, menus, videos and loading screens stay in the 16:9 centre; masks, subtitles, feral vision and the
  Direct3D 12 backend are not checked at ultrawide sizes.
- Exclusive fullscreen, monitor selection and user folder paths with non-ASCII characters (shader pack) are not
  tested.
- `03_river` is slightly brighter than the console reference.
- The map editor on the disc is not supported; choosing it returns to the main menu.
- Multiplayer (split screen, system link, online) is not tested.
- At most 120 frames/s. Open frame rate points: the timeline of computer-controlled characters in `05_mines` at 120
  is 0.12-0.44 s earlier, one character there stops after about 25 s; a scripted drive in `21_dunea` starts
  0.35-0.47 s earlier; wheeled vehicles are 1.5-3 % faster at 120; reloading is about 0.05 s shorter; ladder and
  vine ends, slope edges, particles and HUD animations are not examined yet.
- Only the retail disc *Far Cry Instincts – Predator* (USA/Europe, En/Fr/De/Es/It) without a title update is
  supported.
