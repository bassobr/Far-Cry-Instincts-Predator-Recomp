# Changelog

All notable changes of Far Cry Instincts Predator Recomp. Versions follow `VERSION`; a release tag is `v<VERSION>`.
Statements about behaviour are measurements from test runs on a Windows PC unless marked otherwise; see the
[README](README.md) and [docs/en](docs/en) for the details.

## 0.1.2 (2026-11-14)

### Added

- **Shipped pipeline list** (`pipelines.fcpl` next to `predator.exe`, from `config/predator/pipelines.fcpl`): the
  graphics pipelines of the menus, videos, loading screens and all 19 levels, prewarmed at start on the renderer's
  worker threads at below-normal priority, so the first visit of a level no longer stutters while the driver compiles
  pipelines. The level being loaded moves to the front of the queue; a draw whose pipeline is still being prewarmed
  waits for exactly that one. The file holds only hashes of the game's shader containers and render state values
  (no shader code, no game data), is the same for Vulkan and D3D12 and follows the `[graphics] msaa` setting.
  `FCP_RENDER_PIPELINE_SHIPPED=0` turns it off. Tools: `tools\pipelines_collect.ps1`, `tools\pipelines_build.ps1`.
- **Display pacing** (`[display] pacing = "display"`, default on the Steam Deck): with Vulkan the game is timed by
  the display's own refresh, so every frame stays equally long on the screen (30 frames/s: three refreshes of the
  Deck OLED's 90 Hz, 45: two). Measured on the Deck in `21_dunea`: 97-98 % of the frames at exactly that length
  instead of 79-84 % with the game's own timing, at less power. `pacing = "game"` keeps the earlier timing.
- **45 frames/s** in `predator.toml` and the F10 menu (half the Deck OLED's refresh rate).
- `presentstat:` lines in the log: how long each frame stayed on the display (Vulkan, `VK_KHR_present_wait`).
- **Free render resolution:** `[graphics] resolution` = `scale` (from `[graphics] scale`, the default), `native`
  (the picture's size on the output) or a size such as `"1920x1080"`; a size below the output's is scaled up (faster).
  In the F10 menu under Graphics, switches at once. `FCP_RENDER_RESOLUTION`.
- **Vert+ for outputs narrower than 16:9** (16:10, 3:2, 4:3): the picture fills the output with the horizontal
  field of view of 16:9 and more above and below; HUD, menus, videos and loading screens stay in a 16:9 band in the
  middle. `[graphics] aspect = "16:9"` restores the bars, `FCP_RENDER_VERTPLUS=0` keeps Hor+ but turns Vert+ off.
- **Window sizes in the F10 menu:** common 16:9, 16:10, 4:3 and 21:9 sizes plus the monitor's native size.

### Changed

- **Steam Deck:** defaults to a 1280x800 window with `resolution = "native"`, so the picture fills the 16:10 display
  instead of 1280x720 with bars (`width`/`height` = 0 in an older `predator.toml` take the 1280x800 too).
- **Controller calls no longer stall the game:** reading the pad and setting its vibration do not wait for SDL on
  the game's thread any more; vibration is sent from the UI thread. On the Steam Deck the slow pad calls (2-18 ms in a
  few percent of the frames, vibration rarely up to 200 ms) caused small stutters, most while firing.
  `REX_HID_SDL_SYNC=1` restores the earlier behaviour.
- **Faster first start:** the game code is built with a capture tracking limit of 10000 instead of 1000000 (the same
  speed in the game, a shorter ThinLTO link), and on the Steam Deck without ThinLTO, which brought no measurable
  speed there. `FCP_GAME_CODE_CAPTURE_USES=1000000` and `FCP_GAME_CODE_LTO=1` build as before.
- **Steam Deck OLED:** defaults to 45 frames/s paced on the 90 Hz display (measured: 96-100 % of the frames exactly
  two refreshes at 7.8-8.6 W). A `predator.toml` from an older version keeps its value; the 60 Hz Steam Deck LCD
  keeps 30. `FCP_DECK_FPS45=0` keeps 30 on the OLED too.

### Fixed

- **"Load a Map" lists the reached levels again** (issue #1): the list was always empty. The game finds its level
  folders with patterns like `Levels\0*.*`; the folder names have no dot, and the file system layer required one.
  A pattern ending in `.*` now also matches names without a dot, as on Windows and the console. The game's own
  rule decides which levels appear: a level is unlocked once it has been started in a solo game, and the
  progress already stored in an existing profile shows up without starting anything again. `FCP_FIND_STAR_DOT_STRICT=1`
  restores the old matching.
- **Profile saves are no longer dropped** at random: the game leaves the type field of its profile settings
  uninitialised, and some values made the write get skipped (seen after "Resume Game" and before a level change; a
  later save in the same session normally caught up). The type of the game's own profile settings now comes from
  the setting ID, as on the console. `FCP_PROFILE_TITLE_TYPE_FROM_ID=0` restores the old check.
- **Fall damage at 45 frames/s** (and more exact at 60, 90 and 120): the speed the landing sends is now worked out
  from the console's 1/30 s physics steps instead of a fixed "2.4 frames old" estimate. Measured on a PC: the same
  jump sent 4.21 m/s at 45 against 4.08-4.10 at 30 (3 % more); now 4.087-4.088 m/s at 45, 60, 90 and 120. Falls of
  2.5-12 m now do the same damage at every frame rate as at 30 (0 / 0 / 11.5 / 37.8 / 50.7 health; the old estimate
  gave 9.1 / 36.9 / 54.0 at 45). `FCP_FALL_MODEL=linear` restores the previous estimate.
- **Stick turn ramp at 45 frames/s:** 1 s of full stick turned 0.4 % less than at 30 (frames of 22 ms cross the
  console's 33 ms frames); now 250.0-250.3 degrees as at 30, 60, 90 and 120. `FCP_TURN_MODEL=floor` restores it.

- **Renderer tables can no longer run full:** every texture and vertex buffer kept its slot in a fixed table of 4096
  entries for good; once a table was full, everything loaded afterwards was drawn without its texture or with broken
  geometry. Entries unused for 600 frames are now recycled when a table is three quarters full, and textures refused
  for lack of space are tried again. In normal play the tables stayed below 40 % (measured), so this is a safeguard.
  `FCP_RENDER_SLOT_RECYCLE=0` restores the old behaviour.

### Known issues

- The untextured objects and flat surfaces reported in issue #1 (river level) are not reproduced yet; the renderer
  table limit fixed above produces the same kind of picture but was far from full in every measured run.
- The first start on a Steam Deck takes about 9 minutes (game code and shader pack); later starts load at once.

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
