# Changelog

All notable changes of Far Cry Instincts Predator Recomp. Versions follow `VERSION`; a release tag is `v<VERSION>`.
Statements about behaviour are measurements from test runs on a Windows PC unless marked otherwise; see the
[README](README.md) and [docs/en](docs/en) for the details.

## 0.9.1 (2026-11-16)

### Fixed

- **Cheat codes crashed the game** (issue #6): "GiveMeItAll" and "GiveMeTheMaps" save the profile, and the profile
  save handed the runtime a setting whose type byte the game never fills in; with a stale 0 there the runtime read
  the vibration value as a data block and crashed ("predator crashed"). Every profile setting now takes its type
  from its ID, as title settings already did. Both codes now unlock what the game's code unlocks
  for them (the Evolution campaign; the maps of the unlocked campaigns), and the unlock is kept after a restart; all
  six codes were entered from the main menu and from the pause menu without a crash. `FCP_PROFILE_TYPE_FROM_ID=0`
  restores the old check.
- **The text input window can be used with a controller:** the window that opens for a cheat code or a save name
  has on-screen keys (D-pad/left stick, A types, X deletes, Y space, LB shift, Start accepts, B cancels; also by mouse
  or touch), keeps the keyboard focus, is centred and scales with the window; Escape cancels. Before, it was a small
  text field that only a keyboard could fill. `FCP_KEYBOARD_UI=0` restores it.
- **Stale buffers and textures in the renderer's cache** (issue #4, investigation; the reported picture was not
  reproduced): the renderer reuses converted guest textures, vertex and index buffers until the game writes to them,
  and learns about writes from page protection. Measured in Shanty Town: a few small per-frame index and vertex
  buffers (and now and then a small texture) change without any write being reported, and were drawn with their old
  content. Cached entries up to 128 KB are now also compared by content once per frame (`FCP_RENDER_WATCH_REHASH=0`
  turns this off), and every entry now notices each write to its pages even when another entry shares them
  (`FCP_RENDER_WATCH_EPOCH=0`: the old bookkeeping). On our PC neither change makes a visible difference; new
  diagnostics: `FCP_RENDER_WATCH_VERIFY=1` and a log line when the CPU writes into a render target's memory.

## 0.9.0 (2026-11-16)

### Added

- **Controller page in the F10 menu:** the connected controllers and their port (1-4), dead zone, vibration and
  controller driver (moved from Input), and the controller buttons of all 16 Xbox buttons with a "press a button"
  capture (Change, Add, Clear, Default, all to defaults), stored in `predator_input.toml` (`[pad] pad_bind_*`) and in
  force at once. The further buttons SDL 3 reports are bindable too: `guide`, `misc1` (share/capture/mic), the four
  paddles (Xbox Elite, Steam Deck back buttons without Steam Input), `touchpad`, `misc2`..`misc6`. Without an entry
  the paddles still act as A/B/X/Y as before. Only buttons are mapped onto buttons (no turbo, no macros).
  `FCP_PAD_EXTRA=0` restores the earlier handling of the extra buttons, `FCP_PAD_REMAP=0` turns all controller
  remapping off. Tested with virtual SDL pads; not yet with a real controller.
- **Free frame rate limit:** `[display] fps` takes 30 or any whole number from 40 to 120, or `auto` (the refresh
  rate of the window's monitor, at most 120). 31 to 39 and over 120 stay out: the game logic corrections need frames
  well under 30 ms and were not checked above 120 (see [FRAMERATE.md](docs/en/FRAMERATE.md)). The F10 menu lists
  Auto, 30, 40, 45, 50, 60, 72, 75, 90, 100, 120 and the monitor's own rate.
- **Frame rate, VSync and frame pacing switch at once** in the F10 menu, without a restart (`FCP_FPS_LIVE=0`: at the
  next start, as before).
- **`[display] frame_queue`** (F10 → Display → Frames ahead): 1-3 game frames that may wait on the GPU (2 as before;
  1 less input latency). Switches at once. Earlier only `FCP_RENDER_FRAMES_AHEAD`.
- **F11** switches between window and fullscreen like Alt+Enter (`FCP_F11=0` turns it off).
- **"Defaults" on every page of the F10 menu** sets only that page's settings back (the Input page also its key
  bindings, the Controller page also the controller buttons). On a Steam Deck or ROG Xbox Ally X the device's
  defaults are the target, and the menu marks the default of each choice ("- default", "- Steam Deck OLED default";
  `FCP_MENU_DEFAULTS=0` turns both off). A closed field too narrow for the device's name shows "- default"
  (`FCP_MENU_SHORT_MARK=0`: always the full mark).
- On a 90 Hz display (Steam Deck OLED) `auto` means 90 frames/s without display pacing, which the Deck does not
  hold in every level (73-77 in `21_dunea`, measured): 45 is smoother there. The frame rate help in the F10 menu,
  the README and CONFIGURATION say so.
- **Speech volume** (`[audio] speech_volume`, 0-100 %, default 100 = as the console; F10 menu, Game tab): the game's
  speech (dialogue, radio, enemy shouts) apart from music and effects. The port recognises the speech by the game's
  language sound banks and lowers it in the XMA decoder before the game mixes it; the game's mix, 3D placement and
  timing stay as they are. Measured in `00_training` and `21_dunea`: every decoded XMA frame was assigned (0 unknown);
  at 50 % the speech is 6.0 dB quieter, at 0 % silent, music and effects unchanged. Speech can only be lowered.
  `FCP_SPEECH_VOLUME=100` (or `FCP_SPEECH_TRACK=0`) keeps the decoder's output as before.
- **Audio limiter** (`[audio] limiter`, default on; F10 menu, Game tab): a peak limiter after the downmix to the
  speakers (ceiling -0.3 dBFS, 1.3 ms look-ahead, 100 ms release) instead of hard clipping. Below the ceiling every
  sample passes unchanged (measured bit for bit, only 63 samples later). Measured in `21_dunea`: on a stereo device
  the mix stayed below full scale (the limiter acted once, -0.01 dB for 0.7 s); for a 5.1 device the left surround
  reaches +4.1 dBFS, which the limiter turns into short dips (more than 1 dB in under 1 % of the time) instead of
  clipping. `FCP_AUDIO_LIMITER=0` restores the clipping.
- **Colour blind mode** (`[graphics] colour_blind` = off | protanopia | deuteranopia | tritanopia, default off; F10
  menu, Graphics tab): daltonization of the finished picture in the last blit before the output (Vulkan and Direct3D
  12; measured against the expected colour matrix: at least 99.8 % of the pixels within 1 of 255). `off` leaves the
  final blit exactly as before (shown picture bit for bit equal to the game's frame).
- **Exclusive fullscreen with a chosen video mode:** `[display] exclusive_mode` = `desktop` (the default) or
  `"WxH@Hz"`; the F10 menu (Display, "Exclusive mode") lists the modes of the window's monitor and switches at once.
  Measured with Vulkan and Direct3D 12 on one monitor: Alt+Tab gives the desktop its mode back and the game its own
  mode on return, F11 / Alt+Enter, menu changes and a restart work without a black picture, and the frame rate
  settles at the mode's rate again. A game started behind another program no longer keeps the video mode
  (`FCP_EXCLUSIVE_BACKGROUND_MINIMIZE=0`). Vulkan makes an out-of-date swapchain anew at once
  (`FCP_RENDER_RECREATE_LOST=0`).
- **Per-monitor display scaling (DPI):** the process is per-monitor DPI aware (v2) from its first step. The built-in
  window size 1280x720 is meant at 100 % and grows with the monitor's scale (125 %: 1600x900); a window moved to a
  monitor with another scale keeps its size on the screen; the F10 menu's text is rasterized at the display's pixel
  density (sharp at 125-200 %). Sizes set in `predator.toml` stay exact pixels. `FCP_DPI_WINDOW=0`,
  `FCP_DPI_EARLY=0`, `REX_IMGUI_DPI_RASTER=0` restore the earlier behaviour. Measured at a real 125 %; 100, 150,
  200 % and moving between monitors only with a simulated scale (no second monitor available).
- **HUD at the picture's edges** (`[graphics] hud_aspect`, experimental, F10 menu "HUD layout", switches at once):
  `auto` moves radar, health, stance, ammunition, the hints at the top left and the compass to the edges of a 21:9 or
  32:9 picture (to the sides) or a 16:10 picture (up and down); a shape such as `"21:9"` uses the edges of a box of that
  shape. Crosshair, prompts in the middle, scope and binocular views, the pause menu, menus, loading screens and videos
  stay in the 16:9 band. Default `"16:9"`: the whole HUD in the band as before.
- **PC level of detail:** `[graphics] lod_distance` (distance at which vegetation, buildings and characters switch to
  simpler models and trees to sprites) and `view_distance` (how far they are drawn), each a multiple of the
  console's (0.5 to 8; default `1` = the console's values, also on the Steam Deck). They scale the engine's own
  variables `e_obj_lod_ratio`, `e_obj_sprite_switch_dist_ratio` and `e_obj_view_dist_ratio`; only drawing changes,
  not the game logic. In the F10 menu under Graphics, switch at once. `FCP_LOD_DISTANCE`, `FCP_VIEW_DISTANCE`.
- **Shadow distance:** `[graphics] shadow_distance` (1 to 4, default `1` = the console) makes the sun's dynamic
  shadows reach that many times further (the console: a square of 16-20 m ahead); the shadow map grows with it, so
  the shadows stay as sharp (at most 8192x8192). Needs a restart; in the F10 menu under Graphics.
  `FCP_SHADOW_DISTANCE`.
- **Soft shadow edges:** `[graphics] shadow_filter = "soft"` filters the dynamic shadows over 4x4 shadow map texels
  (3x3 bilinear comparisons) instead of the game's 2x2, without stair steps at the edges. Default `console`. In the
  F10 menu under Graphics, switches at once. `FCP_RENDER_SHADOW_FILTER`. The shader translator's version is now 8:
  the shader pack is built again at the first start.
- **Ambient occlusion:** `[graphics] ao = "ssao"` or `"gtao"` darkens creases, corners and contact areas of the
  opaque 3D scene (screen space, from the scene's depth and the game camera's projection; GTAO after Intel's XeGTAO).
  It goes in after the opaque world and before the sky, water, particles, alpha-blended grass, the weapon and the
  HUD, and fades out between 35 and 90 m. Default `off` = the console's picture (no pass at all). In the F10 menu
  under Graphics, switches at once. `FCP_RENDER_AO`; diagnostics in [DEBUG-SWITCHES](docs/en/DEBUG-SWITCHES.md).
- **Texture packs** ([docs/en/TEXTURE-PACKS.md](docs/en/TEXTURE-PACKS.md)): `[graphics] texture_dump = true` writes
  every texture the game uses once into `textures\dump\` of the user folder, named by a hash of its content, format
  and size (`<hash>_<w>x<h>_<format>.png|dds`; the same name in every run and level). Block compressed textures are
  written unchanged as DDS with their mip levels, 8-bit colour textures as uncompressed PNG, cube maps as DDS.
  `[graphics] texture_replace = true` draws images from `textures\replace\` (PNG, or DDS with BC1-BC5, BC7 or RGBA8,
  cube maps as cube DDS) instead, at any resolution, with their own mips or a generated chain; loaded on a background
  thread, uploaded with a per-frame budget, and picked up while the game runs when the folder changes. Render
  targets, resolve targets and textures the game keeps rewriting (videos) are never dumped or replaced. Both off by
  default (`FCP_TEXTURE_DUMP`, `FCP_TEXTURE_REPLACE`).
- **HDR10 output:** `[display] hdr = "on"` or `"auto"` (on when Windows HDR is on for the game's monitor) presents
  through an HDR10 swapchain (PQ, BT.2020; Vulkan and Direct3D 12). The game's SDR picture keeps its look and is shown
  with its white at `hdr_paper_white` (default 200 nits); HUD, menus and the F10 menu stay at paper white.
  `hdr_boost` (0 to 1, default 0 = off) optionally stretches the highlights towards `hdr_peak` (default: what Windows
  reports for the monitor). Without an HDR10 swapchain the log says why and the output stays SDR. Default `off` =
  the SDR output as before. In the F10 menu under Display, switches at once. `FCP_HDR`; checking without an HDR
  display: `tools\hdr_check.py` ([DEBUG-SWITCHES](docs/en/DEBUG-SWITCHES.md)).
- **Frame rates above 120, unsupported:** `[display] unsupported_uncapped_fps = true` lets `fps` and `auto` go up to
  240; the F10 menu then lists 144, 165 and 240 marked "unsupported". The game logic was audited at 144 and 240
  (falls, landing, turning, swimming, boats, drowning, fire rate, vehicles): what still differs is listed in
  [FRAMERATE.md](docs/en/FRAMERATE.md#above-120-unsupported), among it a rifle fire rate 7-12 % off at 144 and 165.

### Fixed

- **Split screen on 21:9 and 16:10** (2-4 players; Main menu → Multiplayer → Splitscreen): every player's HUD,
  crosshair and binoculars are round and inside their own view again (before they were stretched by the picture's
  shape), the views on the right no longer show a duplicated strip at their left edge, with 3 players at 16:10 no
  leftover of the lobby shows between the right-hand views, and 2 players side by side get the wider (or taller) view
  like the single player game instead of a stretched picture. `hud_aspect = "auto"` moves the HUD to the edges of each
  view; the team score and time stay together in the middle. Ambient occlusion is applied to every view (before only
  the first one). At 16:9 nothing changes. `FCP_RENDER_SPLIT_VIEW=0` restores the earlier handling. Tested with
  virtual controllers on one map with 2, 3 and 4 players, both "widescreen split" layouts, Vulkan and Direct3D 12;
  32:9 and the sniper scope in split screen are not tested.
- **Binoculars and scopes on 21:9, 32:9 and 16:10:** the mask now stays round in the middle and covers the whole
  picture; before, its frame or bars were stretched over the picture and the world showed beside the lenses or the
  circle (21:9, 32:9) or above and below them (16:10). `FCP_RENDER_SCREEN_MASK=0` restores the old classification.
- **Even camera turns above 30 frames/s** (issue #2): the game's frame timer counts whole milliseconds, so at
  60 frames/s the world moved by 15-18 ms per frame while frames reach the display every 16.7 ms, and turning moved
  the view 4 % more or less from frame to frame. Above 30 frames/s the game now gets the target frame time plus a
  tenth of the time still owed; the sum stays the clock's (measured: turn per frame sd 0.3 % instead of 3.7-4.1 %, 1 s
  of full stick 250.1 degrees as before). `FCP_FRAME_DT_SMOOTH=0` restores the whole milliseconds. The fire rate
  and script timer corrections keep reading the clock's whole milliseconds (with the even values the assault rifle
  fired 2-8 % slower and drowning hurt 8-10 % more at 60 and 120 frames/s; measured now: 8.8-9.1 shots/s and
  7.14 HP/s at 30, 45, 60 and 120, as at 30). `FCP_FRAME_DT_RAW_CLOCK=0` lets them read the even values.
- **Drowning above 30 frames/s:** a 50 ms script timer measured at 49 ms on the whole-millisecond clock took the
  correction's fallback (whole frames, 64 ms at 60 frames/s), so drowning dealt 7.3-7.4 instead of 7.14 HP/s at 60
  and 120 frames/s. The measured interval now counts from 3 ms below the period (measured: 7.14 HP/s, death after
  13.9 s at 30, 45, 60 and 120). `FCP_FRAME_TIMER_SLACK_MS=0` restores the earlier test.
- **Device allocations that run short** (issue #1, missing textures or flat objects after a long session; the cause
  of the report is not confirmed): a texture or buffer the graphics driver refuses is now logged (`refused by the
  device`) and tried again later instead of staying black or empty, and entries not bound for a while (textures,
  vertex and index buffers) are evicted after a refusal or once the estimated allocation count reaches 3/4 of the
  driver's limit (`FCP_RENDER_ALLOC_EVICT=0` turns the eviction off). Under Vulkan guest textures share 64 MiB memory
  blocks when the driver allows at most 65536 allocations (`FCP_RENDER_VK_TEXTURE_POOL=0|1`). The log states the
  driver's limit (`[vulkan] memory:`) and the allocations (`renderer: allocations:`).
- **Wheeled vehicles above 50 frames/s:** the wheel impulses (engine, brakes, suspension, tyre grip) now run at most
  50 times per second, as the console's physics does. Measured before: the Humvee in Dead Marshes reached 19.1-19.2
  instead of 17.3-17.4 m/s at 120-240 frames/s, the quad was 3 % slower at 240; now the quad accelerates as at 30 and
  the Humvee is within 1-3 % of its speed at 30 (20 m from rest up to 5 % sooner or later). `FCP_FRAME_VEHICLE=1`
  restores the old behaviour.
- **`predator.toml` stays valid TOML when the first start stores the disc image it found:** the path was written as
  `image = "D:\Games\..."`, which the game reads but other TOML tools reject (`\G` is no valid escape there). A value
  with a backslash is now written as a literal string, `image = 'D:\Games\...'` (the game reads both).
  `FCP_TOML_LITERAL=0` writes `"..."` as before.
- **F10 menu at 1280x800 (Steam Deck):** "Exclusive mode" showed "Desktop (1280 x 800 @ 90 Hz) - defau"; a closed
  field whose text does not fit now drops the default mark and, if still too long, ends in "..." (the list and the
  help show the full text; `FCP_MENU_SHORT_MARK=0` as before). The grey hints of the Controller page ("... their
  XInput number") wrap instead of being cut (`FCP_MENU_WRAP_HINTS=0` as before).
- **Release folder:** `TEXTURE-PACKS.md` is part of it; the comment of `texture_replace` in `predator.toml` points to
  it.

### Known issues

- HDR10 is checked by measurement only, not yet on a real HDR display; under Proton (Steam Deck) the game gets no HDR
  surface and stays SDR.
- Split screen: a thin strip of the world can show beside the binoculars of the right-hand player at 21:9.
- At 144 and 165 frames/s (unsupported) the assault rifle fires 7-12 % off.
- The graphics glitches of issue #1 and the lighting switch of issue #2 are not reproduced yet.

## 0.2.0 (2026-11-15)

### Added

- **Save protection** (README "Saves"): a save is written beside the old one and replaces it only when complete, so a
  crash or a forced end in the middle of a save keeps the previous save; a backup after every save (newest three in
  `save_backup\`); a check at every start that moves a damaged or unfinished save to `save_damaged\` and puts the
  newest good backup in its place; `save:` lines in the log. `FCP_SAVE_GUARD=0` turns it off.
- **The end waits for a save:** closing the window and *Exit Game* in the F10 menu wait up to 5 s for a save in
  progress, also from the pause menu.
- **Disc image found automatically:** with `[game] image` empty or not fitting, the first start looks for the image
  next to `predator.exe`, in its `assets` folder, in the user folder and a few other usual places, checks it and
  stores its path in `predator.toml`. Without a hit a window lists the places it looked in.
- **CI on every push and pull request** (`.github/workflows/ci.yml`): a game data check of every tracked file
  (`tools\check_game_data.ps1`: no disc image or files from it, no XEX, no recompiled or generated code, no build
  output, no binary file above 256 KB except the PGO profile, the pipeline list and the SDK's test fixtures) and the
  build of `predator.exe`, the SDK and the release folder without a disc image, followed by the renderer's own tests.
  The public export applies the same game data rules (`tools/game_data_rules.ps1`).

### Fixed

- **`tools\get_deps.ps1` works again on a second run** (or on dependencies restored from a cache): it read the
  copies it had made of libmspack's symlink targets as link paths and stopped with an error.

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
