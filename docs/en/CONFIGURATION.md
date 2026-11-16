# Configuration

All settings are in two text files in the user folder (`%LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp`, or the
folder of `predator.exe` when a `portable.txt` lies beside it, or `FCP_USER_ROOT`):

- `predator.toml`: game, display, graphics and the input file's name,
- `predator_input.toml`: keyboard, mouse and controllers.

Both are written with their defaults and comments when they are missing. Values may be quoted (`"..."` or
`'...'`; a backslash is taken literally), `#` starts a comment, a UTF-8 BOM is accepted. An invalid value is logged
and the default is used; an unknown key is logged and ignored. The log (`predator.log`) shows the file and every
value at start.

**Environment variables.** Every key of `predator.toml` and most keys of `predator_input.toml` are also
environment variables (the name is shown in the file and in the tables below). A variable set in the environment wins
over the file; the F10 menu then shows the key as "set by environment".

**F10 menu.** Most settings can be changed in the settings menu (section [Settings menu](#settings-menu-f10)). The
menu writes the files at once and keeps their comments. "Applies" in the tables says whether a change takes effect
at once or at the next start.

## `predator.toml`

### `[game]`

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `image` | `FCP_GAME_ISO` | `""` | path of the disc image (`.iso`, full dump or plain XISO) or of a folder with the extracted disc; relative to the user folder. Empty or not fitting: the start looks for the image (next to the EXE, its `assets` folder, the user folder, the folder above the EXE, Downloads/Desktop/Documents at their top level) and stores the one whose `default.xex` fits | next start |
| `pause_on_focus_loss` | `FCP_PAUSE_ON_FOCUS_LOSS` | `false` | `true`: while the window has no focus the game opens its pause menu (in a level), the sound is muted and vibration stops; back in the window the game stays in its pause menu | at once |
| `skip_intro` | `FCP_SKIP_INTRO` | `false` | `true`: skip the logo and intro videos at start; story cutscenes stay | at once |
| `fps_counter` | `FCP_FPS_COUNTER` | `false` | `true`: frame rate and frame time display in a corner | at once |
| `language` | `FCP_LANGUAGE` | `"english"` | `english`, `french`, `german`, `spanish`, `italian`, `auto` (the Windows display language; English if it is none of these). Sets the console's system language that the game reads | next start (F10: Apply restarts) |

### `[display]`

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `mode` | `FCP_DISPLAY_MODE` | `"windowed"` | `windowed`, `borderless` (fullscreen window), `exclusive` (fullscreen in its own video mode, see `exclusive_mode` and [Exclusive fullscreen](#exclusive-fullscreen)). Alt+Enter or F11 toggles window/fullscreen and stores the choice here | at once |
| `exclusive_mode` | `FCP_EXCLUSIVE_MODE` | `"desktop"` | video mode of exclusive fullscreen: `desktop` (the monitor's own mode) or `"WxH@Hz"` from the monitor's list (`"2560x1440@120"`; `"WxH"` alone: the desktop's refresh rate). The F10 menu lists the modes of the window's monitor | at once |
| `width`, `height` | `FCP_WINDOW_WIDTH`, `FCP_WINDOW_HEIGHT` | `0` | window client size in pixels; `0` = 1280x720 at 100 % display scale, scaled with the monitor's scale (125 %: 1600x900; at most the monitor's work area), Steam Deck: 1280x800. The render resolution is `[graphics] resolution` / `scale` | at once |
| `monitor` | `FCP_MONITOR` | `0` | `0` = the system's choice (primary), `1`..`n` = display number (not tested) | next start |
| `fps` | `FCP_FPS` | `30` (Steam Deck OLED: `45`) | frame rate limit: `30` (as the console), any whole number from `40` to `120` (`45`, `60`, `90`, `120`, ...), or `auto` (the refresh rate of the monitor the window is on, at most 120; looked at again every 2 s). Smooth only when the display's refresh rate is a multiple of it (90 Hz: 30, 45, 90; 60 Hz: 30, 60; 120 Hz: 30, 40, 60, 120). On a 90 Hz display (Steam Deck OLED) 45 is smoother than `auto`: `auto` means 90 there, which runs without display pacing and which the Deck does not hold in every level. 31-39 and over 120 are refused unless `unsupported_uncapped_fps` is set (see [FRAMERATE.md](FRAMERATE.md#which-frame-rates)) | at once |
| `unsupported_uncapped_fps` | `FCP_UNSUPPORTED_UNCAPPED_FPS` | `false` | `true`: `fps` and `auto` also take 121 to 240, and the F10 menu lists 144, 165 and 240 marked "unsupported". **Not supported**: audited at 144 and 240 only; the rifle fires 7-12 % faster or slower at 144 and 165, the Humvee accelerates up to 5 % differently ([FRAMERATE.md](FRAMERATE.md#above-120-unsupported)) | next start |
| `pacing` | `FCP_FPS_PACING` | `"game"` (Steam Deck: `"display"`) | `display`: every frame stays the same number of refreshes on the display, timed by the display itself (Vulkan; for 30 and 45 on a 90 Hz display, 30 on 60 Hz); `game`: the game's own timing. See [FRAMERATE.md](FRAMERATE.md#display-pacing) | at once |
| `vsync` | `FCP_RENDER_VSYNC` | `"auto"` | `auto` (wait for the monitor up to 60 frames/s, not above), `on`, `off` | at once |
| `frame_queue` | `FCP_RENDER_FRAMES_AHEAD` | `2` | frames ahead: `1`-`3` game frames that may wait on the GPU before the game waits for it. `1` = least input latency, `3` = evens out single slow frames for up to one frame more latency. While display pacing is on, the game also waits after every frame until the previous one is on the display | at once |
| `hdr` | `FCP_HDR` | `"off"` | HDR10 output: `off` (SDR, as before), `on` (HDR10 when the display and driver offer it), `auto` (on when Windows HDR is on for the game's monitor). Without an HDR10 swapchain the log says why and the output stays SDR. See [HDR10](#hdr10) | at once |
| `hdr_paper_white` | `FCP_HDR_PAPER_WHITE` | `200` | HDR10: brightness of the game's white in nits (80 to 1000); `auto` = Windows' "SDR content brightness". HUD, menus and the F10 menu use it too | at once |
| `hdr_peak` | `FCP_HDR_PEAK` | `0` | HDR10: the display's peak brightness in nits (100 to 10000); `0` = what Windows reports for the monitor, else 1000. Only the boost uses it | at once |
| `hdr_boost` | `FCP_HDR_BOOST` | `0` | HDR10: `0` to `1`, stretches the bright half of the picture's luminance towards `hdr_peak`; `1` = the game's white reaches the peak. `0` = off: the picture looks as in SDR | at once |

"At once" for `fps`, `pacing` and `vsync` holds for the F10 menu: the game side changes with the next frame, the
present mode (Vulkan: the swapchain is made anew; Direct3D 12: the sync interval) and display pacing with the next
present. `FCP_FPS_LIVE=0` keeps them for the next start, as before. An edit of `predator.toml` itself is read at
the next start.

#### HDR10

The game renders an SDR picture. With `hdr = on` (or `auto` and Windows HDR on) the swapchain is HDR10
(`R10G10B10A2`, SMPTE ST 2084 "PQ", BT.2020 primaries; Vulkan `VK_COLOR_SPACE_HDR10_ST2084_EXT`, Direct3D 12
`DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`), and the last step of the output maps the SDR picture per pixel:
SDR value -> linear light (gamma 2.2, as an SDR PC monitor shows it) -> optional highlight boost -> BT.709 to
BT.2020 -> x `hdr_paper_white` nits -> PQ. Order of the output: the game's gamma ramp, SMAA, scaling into the window
with bars and the colour blind mode, then HDR10, then the F10 menu and the frame rate display (at paper white, never boosted). The HUD is part
of the game's picture: at `hdr_boost = 0` it stays at paper white; a boost brightens its white too.

- With Windows HDR off, `hdr = on` depends on the driver (measured on an NVIDIA RTX 4070 Ti SUPER): Direct3D 12
  accepts the HDR10 swapchain and Windows shows it converted to SDR; Vulkan offers no HDR10 format, so the output
  stays SDR with a log line. The output was checked numerically from the presented HDR10 values
  (`tools\hdr_check.py`); it has not been looked at on a real HDR monitor yet.
- The F10 menu switches HDR10 at once (a new swapchain at the next frame); paper white, peak and boost too.
- Moving the window to another monitor does not re-check `auto`; choose the option again in the F10 menu.
- `FCP_HDR=off` turns it off whatever the file says.

### `[graphics]`

All graphics defaults give the console's picture.

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `backend` | `FCP_RENDER_BACKEND` | `"auto"` | `auto` (the `backend` of `renderer.toml` beside the EXE, else Vulkan), `vulkan`, `d3d12` | next start |
| `msaa` | `FCP_RENDER_MSAA` | `"console"` | `console` (2x on the screen targets, 4x on the reflection, as the console), `off`, or `2`, `4`, `8` samples on the screen-sized targets (limited to what the GPU supports). Older files: `true` = console, `false` = off | next start |
| `smaa` | `FCP_RENDER_SMAA` | `false` | `true`: SMAA 1x edge smoothing on the finished picture | at once |
| `gamma` | `FCP_RENDER_GAMMA` | `true` | the game's gamma ramp (its brightness setting) | at once |
| `colour_blind` | `FCP_RENDER_COLOUR_BLIND` | `"off"` | `off` (as the console), `protanopia`, `deuteranopia`, `tritanopia`: daltonization of the finished picture (Machado 2009 simulation, Fidaner's correction) in the last blit before the output, after the gamma ramp and SMAA, below the F10 menu | at once |
| `scale` | `FCP_RENDER_SCALE` | `1` | render resolution of the screen-sized targets: `1` = 1280x720 as the console, `auto` = output height / 720 (follows window size changes), or a factor or percentage from 0.5 to 4 (`1.5`, `150%`, `2x`). HUD and text get sharper with it. Used while `resolution` is `scale` | at once |
| `resolution` | `FCP_RENDER_RESOLUTION` | `"scale"` | `scale` = from `scale`; `native` = the picture's size on the output (window or monitor; with bars the size inside them); or `WxH` (`"1280x800"`, 320x180 to 10240x5760) host pixels for the 1280x720 screen targets. Smaller than the output is scaled up; a shape other than the picture's is stretched to it. Steam Deck default: `native` | at once |
| `shadow_scale` | `FCP_RENDER_SHADOW_SCALE` | `1` | shadow map resolution as a multiple of the console's 1344x1344 (0.5 to 4) | next start |
| `shadow_distance` | `FCP_SHADOW_DISTANCE` | `1` | how far the sun's dynamic shadows reach, as a multiple of the console's (1 to 4; the console covers a square of 16-20 m ahead of the camera). The shadow map grows with it (`shadow_scale` x `shadow_distance`, at most 8192x8192), so a shadow texel keeps its size | next start |
| `shadow_filter` | `FCP_RENDER_SHADOW_FILTER` | `"console"` | edges of the dynamic shadows: `console` (the game's own filter, a bilinear comparison over 2x2 shadow map texels), `soft` (an even filter over 4x4 texels: 3x3 bilinear comparisons; smoother edges without stair steps). Applies to every shadow map the game samples with its shadow filter (sun and lamps) | at once |
| `ao` | `FCP_RENDER_AO` | `"off"` | ambient occlusion over the opaque 3D scene: `off` (as the console), `ssao` (16 samples per pixel) or `gtao` (ground truth AO after Intel's XeGTAO, a little sharper in corners). See below | at once |
| `lod_distance` | `FCP_LOD_DISTANCE` | `1` | distance at which vegetation, buildings and characters switch to their simpler models and trees and bushes to sprites, as a multiple of the console's (0.5 to 8; `1` = the console's values) | at once |
| `view_distance` | `FCP_VIEW_DISTANCE` | `1` | how far vegetation, buildings and characters are drawn, as a multiple of the console's (0.5 to 8). The level's fog and far plane still end the view | at once |
| `aniso` | `FCP_RENDER_ANISO` | `1` | anisotropic filtering 1-16 for linearly filtered samplers; `1` = as the game sets it | next start |
| `fov` | `FCP_RENDER_FOV` | `1` | field of view as a multiple of the game's (0.5 to 2, `1.2` or `120%`); the weapon and hands keep their own projection | next start |
| `texture_dump` | `FCP_TEXTURE_DUMP` | `false` | `true`: every texture the game uses is written once into `textures\dump\` of the user folder, named by a hash of its content; see [TEXTURE-PACKS.md](TEXTURE-PACKS.md) | next start |
| `texture_replace` | `FCP_TEXTURE_REPLACE` | `false` | `true`: images in `textures\replace\` (PNG or DDS, any resolution) whose name holds a texture's hash are drawn instead of it; changes in the folder show while the game runs; see [TEXTURE-PACKS.md](TEXTURE-PACKS.md) | next start |
| `aspect` | `FCP_RENDER_ASPECT` | `"auto"` | shape of the 3D picture: `auto` = the output's shape (wider than 16:9: Hor+, the console's vertical field of view, more to the sides; narrower: Vert+, the console's horizontal field of view, more above and below), `16:9` (or `off`) = as the console with bars, or a fixed shape (`16:10`, `4:3`, `21:9`, `32:9`, `2.39`). `FCP_RENDER_VERTPLUS=0` shows narrower outputs with bars as before | next start |
| `hud_aspect` | `FCP_RENDER_HUD_ASPECT` | `"16:9"` | HUD layout on a picture wider or narrower than 16:9 (experimental): `16:9` = the whole HUD in the 16:9 band in the middle (as before); `auto` = the edge elements (radar, health, stance, ammunition, the hints at the top left, the compass) at the picture's edges: to the sides on 21:9/32:9, up and down on 16:10; or a shape such as `21:9` = at the edges of a box of that shape (on a 32:9 screen). Crosshair, prompts in the middle, scope and binocular views, the pause menu, menus, loading screens and videos stay in the band | at once |

Which targets are scaled: targets at least 640 pixels wide and wider than high get `scale`, square targets from
1024 get `shadow_scale`, all smaller post-processing targets stay at console size (the game passes their pixel sizes
in shader constants). Measured in `21_dunea` at 120 frames/s: `scale = 1` 119.97-120.00 frames/s, `scale = auto`
(2560x1440 internally) 119.89-120.00 frames/s on a desktop GPU.

**Level of detail (`lod_distance`, `view_distance`).** The 3D engine picks every object's model, sprite and view
distance per frame from its distance, its radius, per-object factors of the level and three global engine variables:
`e_obj_lod_ratio` (20 on the console), `e_obj_sprite_switch_dist_ratio` (14.4) and `e_obj_view_dist_ratio` (120).
`lod_distance` multiplies the first two, `view_distance` the third, right after the engine registers them and again
when the F10 menu changes them; with `1` nothing is written. Nothing in the game changes them later (measured in
several levels with `FCP_TRACE_LOD=1`). They only decide what is drawn: the game logic, the enemies' behaviour and physics do not read
them (read from the code; details in the source `src/fcp/detail_distance.cpp`). The idea is from Far Cry Instincts
Recomp; the values and offsets were read from this game. More detail costs CPU time (measured in `21_dunea` at
120 frames/s: 798 draws per frame and 104 % of a core for the process with the console's values, 873 / 109 % at 4 + 4,
917 / 112 % at 8 + 8, still 120 frames/s; the GPU difference was not measurable at 1280x720). The default, also on
the Steam Deck, is the console's.

**Shadows (`shadow_distance`, `shadow_filter`).** The game renders one shadow map per frame for the sun (or up to four
lamps). The sun's map covers a square of side max(R, 2R sin(fov/2)) ahead of the camera, R = the level's
`DynamicShadowRadius` (10 m); beyond it only the baked shadows remain. `shadow_distance` multiplies R for the shadow
pass (the level's and the eased value stay the game's) and the renderer enlarges the map by the same factor. Measured
in `02_native` at 4: 65 instead of 19 draws into the shadow map, and the rocks and river bank 20-30 m ahead get the
shadow of the trees above them. Cost: GPU memory of the larger map (5376x5376 at 4) and the extra casters.
`shadow_filter = "soft"` replaces the game's 2x2 comparison (`BilinearSampleShadow` in the game's shaders) by
3x3 bilinear comparisons over 4x4 texels in the translated pixel shaders; the renderer turns it on per draw only
for textures that are depth resolves. Both ideas are from Far Cry Instincts Recomp, rewritten for this game.

**Ambient occlusion (`ao`).** A screen space pass over the opaque 3D scene: from the scene's depth and the game
camera's projection it darkens creases, corners and contact areas (16 samples for `ssao`; for `gtao` three horizon
slices after Intel's XeGTAO), blurs the result without crossing depth edges and multiplies it into the picture. It
goes in once per frame, right after the opaque world (terrain, buildings, rocks, vegetation, characters, vehicles)
and before the first draw that blends or stops writing depth (in the levels: the sky), so the sky, water, particles,
alpha-blended grass and leaves, the weapon and hands, the HUD and menus get none. It fades out between 35 and 90 m.
It follows the field of view (`fov`, zoom), Hor+/Vert+, `scale`/`resolution` and `msaa` (per sample). Where something
does not fit (no camera projection yet, a projection of another shape than the view), that frame is left without it.
With `off` no pass is made and nothing is created. Above 720 lines the occlusion is computed at half resolution
(`FCP_RENDER_AO_RESOLUTION=full` changes that). Measured on a desktop GPU (RTX 4070 Ti SUPER, `02_native`, 120
frames/s held in every case): 0.46-0.49 ms GPU per frame at 1280x720, 0.30-0.35 ms at 2560x1440 (half resolution),
0.98 ms at 2560x1440 with `full`. Not measured on the Steam Deck, where it stays `off` by default (expected several
milliseconds there). The kernels come from the author's *Far Cry Instincts* port (there
from SCDA-XBERecomp); see [THIRD-PARTY-NOTICES](../../THIRD-PARTY-NOTICES.md).

**Wide outputs (21:9, 32:9).** With `aspect = "auto"` the 3D picture fills the width; HUD, text, button prompts and the pause menu stay 16:9 in the middle, the main menu, loading screens and videos stay 16:9 with bars. Measured at 1720x720, 2560x1080, 3440x1440 and 32:9: HUD undistorted, 120 frames/s in `21_dunea` at 3440x1440 as with bars. Binoculars and scopes stay round in the middle and cover the whole picture, also on 16:10 (measured at 2560x1080, 3840x1080 and 1280x800 in `02_native`). With `hud_aspect = "auto"` the edge elements of the HUD move to the picture's edges; while a scope or the binoculars are up, the HUD stays in the band. Not verified: subtitles, Direct3D 12, a window resized while playing.

### `[audio]`

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `speech_volume` | `FCP_SPEECH_VOLUME` | `100` | volume of the speech in percent, `0`..`100`; `100` = as the console. Music and effects stay as they are | at once |
| `limiter` | `FCP_AUDIO_LIMITER` | `true` | peak limiter after the downmix to the speakers: where the mix would go above full scale (explosions, gunfire), the whole output is turned down for that moment instead of clipping; below full scale nothing changes. `false` = hard clipping as before | at once |

**Speech volume.** The game keeps its speech (dialogue, radio, enemy shouts) in its language sound banks
(`sounds_dvd\<language>\*.sb4` in `dvdsound.dat`), music and effects in the others. The port follows which bank the
game reads into which memory and lowers the decoded speech before the game mixes it, so the game's own mix, 3D
placement and timing stay unchanged. Speech can only be made quieter, not louder (the decoder's 16-bit output has no
headroom); the game's own option "combat music volume" lowers the music. Not covered: speech inside the videos
(intro, cutscene videos), which comes from the video file.

**Limiter.** Ceiling -0.3 dBFS, look-ahead 1.33 ms (the sound is 63 samples, 1.3 ms, later), release 100 ms, one gain
for all channels. See the measurements in the changelog.

### `[input]`

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `file` | `FCP_INPUT_CONFIG` | `"predator_input.toml"` | keyboard, mouse and controller file, relative to the user folder | next start |
| `button_prompts` | `FCP_BUTTON_PROMPTS` | `"auto"` | `auto` (the device used last), `xbox` (the console's symbols), `keyboard`, `playstation`, `switch` | at once |

## `predator_input.toml`

### `[keyboard]`, `[mouse]`

| Key | Variable | Default | Values |
|---|---|---|---|
| `[keyboard] enabled` | `FCP_KEYBOARD` | `true` | keyboard as player 1's controller |
| `[mouse] enabled` | `FCP_MOUSE` | `true` | mouse look and mouse buttons; the mouse is captured only while the game reads look input (free in menus) |
| `sensitivity` | - | `1.0` | degrees per 10 mouse counts (1.0 = 0.1 degrees per count, measured exact); `sensitivity_x`, `sensitivity_y` set one axis |
| `invert_y` | - | `false` | invert vertical look |
| `zoom_scale` | `FCP_MOUSE_ZOOM_SCALE` | `"fov"` | while zoomed (scope, binoculars, vehicle view): `fov` = slower by field of view / 90 degrees, as the game scales the stick; `tan` = by tan(fov / 2); `off` |
| `zoom_sensitivity` | `FCP_MOUSE_ZOOM_SENSITIVITY` | `1.0` | extra factor while zoomed (0.05-10) |
| `acceleration` | `FCP_MOUSE_ACCEL` | `0.0` | extra gain per count/ms of mouse speed (0.05: 1.5 times at 10 counts/ms); `0` = off. Measured over time, so the same at any frame rate |
| `acceleration_cap` | `FCP_MOUSE_ACCEL_CAP` | `2.0` | highest gain of the acceleration (>= 1) |
| `smoothing` | `FCP_MOUSE_SMOOTHING` | `0` | milliseconds each motion is spread over (up to 200), by time; the total angle stays exact, the delay is half this on average; `0` = off |

Mouse look adds degrees to the angles of the game's own look input after it has run; there is no stick emulation
and no dependence on the frame rate.

### `[pad]`

| Key | Variable | Default | Values |
|---|---|---|---|
| `backend` | `FCP_PAD_BACKEND` | `"sdl"` | `sdl` (all controllers through SDL 3, up to four players, hot plug with stable player slots), `xinput` (XInput pads only), `off` (no controllers; keyboard and mouse stay). Read at start |
| `dead_zone` | `FCP_PAD_DEAD_ZONE` | `0.0` | extra radial dead zone (0.05-0.9) for drifting sticks; `0` = only the game's own |
| `rumble` | `FCP_PAD_RUMBLE` | `100` | vibration strength in percent; `0` = off |
| `pad_bind_<button>` | `FCP_PAD_REMAP` | the button itself (`a`..`y` also a paddle, below) | the controller buttons that act as this button of the Xbox pad, for all players |
| `pad_bind_none` | - | `""` | controller buttons that do nothing (only needed for the paddles, see below) |

Buttons for `pad_bind_<button>`: `a b x y lb rb lt rt ls rs start back up down left right` (`up`..`right` =
D-pad), by their place on an Xbox pad. Values: the same names; PlayStation names work too (`cross circle square
triangle l1 r1 l2 r2 l3 r3 options select share create`), as do `south east west north`. Several buttons with
commas, `none` for none. A button that the file gives to another button is removed from the defaults; to swap two
buttons, set both. A trigger as the source presses a digital button from 30/255; a digital source on `lt`/`rt`
gives 0 or 255. Only the controller's buttons are mapped onto the Xbox pad's buttons: no turbo, no macros, the
sticks stay as they are.

Further controller buttons that SDL 3 reports (controller driver `sdl`) work as values too:

| Name | Aliases | Button |
|---|---|---|
| `guide` | `home`, `ps` | Xbox, PS or Home button |
| `misc1` | `capture`, `mic` | Xbox Series share, PS5 microphone, Switch capture |
| `rpaddle1`, `rpaddle2` | `p1`, `p2`, `r4`, `r5` | right paddles (Xbox Elite P1/P2, Steam Deck R4/R5) |
| `lpaddle1`, `lpaddle2` | `p3`, `p4`, `l4`, `l5` | left paddles (Xbox Elite P3/P4, Steam Deck L4/L5) |
| `touchpad` | - | PlayStation touchpad click |
| `misc2`..`misc6` | - | further buttons of some controllers |

The paddles act as `a b x y` as long as no `pad_bind_` line names them (`lpaddle2` = A, `lpaddle1` = B, `rpaddle2`
= X, `rpaddle1` = Y; this is how the SDK mapped them before). `guide`, `misc1`, `touchpad` and `misc2`..`misc6` do
nothing unless bound. To make a paddle do nothing, list it in `pad_bind_none`. Under Steam Input (Steam Deck, or a
controller with Steam Input on) Steam maps the back buttons itself; the game then only sees what Steam sends.

| Switch | Effect |
|---|---|
| `FCP_PAD_EXTRA=0` | paddles, misc and touchpad as before this feature (the SDK folds the paddles onto A/B/X/Y, misc1 and the touchpad onto the guide bit the game does not get); they cannot be bound |
| `FCP_PAD_REMAP=0` | no controller remapping at all: every `pad_bind_` line is ignored, the extra buttons as with `FCP_PAD_EXTRA=0` |

The settings menu's **Controller** page edits all of this (see below).

### `[bindings]`

Each target is a pad button, trigger or stick direction of player 1; the value is a list of keys (commas between
them, `none` for none). A key that the file binds to one target is removed from the defaults of the others.

| Target | Default keys | In the game |
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
| `rup`, `rdown`, `rleft`, `rright` | I, K, J, L | look with the keyboard (through the right stick) |

The actions in the last column are taken from the game's default action map; the pairing of a few of them (for
example LB/RB) is read from the game data and not verified by play.

Key names: `a`-`z`, `0`-`9`, `f1`-`f24`, `num0`-`num9`, `space`, `enter`, `escape`, `tab`, `backspace`, `shift`,
`ctrl`, `alt`, `up`, `down`, `left`, `right`, `insert`, `delete`, `home`, `end`, `pageup`, `pagedown`, `minus`,
`equals`, `comma`, `period`, `slash`, `semicolon`, `apostrophe`, `lbracket`, `rbracket`, `backslash`, `grave`,
`mouse1`-`mouse5`, `wheelup`, `wheeldown`, `0x<virtual key code>`.

## Button prompts

The game shows buttons in hints and menu footers as glyphs of two font pages. The port replaces these pages with
redrawn copies; position, size and texture coordinates stay the game's.

| Style | Shows |
|---|---|
| `keyboard` | the first key bound to the target in `[bindings]` (key cap, mouse with the button marked, arrow keys) |
| `playstation` | cross/circle/square/triangle, L1/R1/L2/R2/L3/R3, OPTIONS/SHARE/CREATE (generation from the pad) |
| `switch` | B/A/Y/X by position, L/R/ZL/ZR, +/- |
| `xbox` | the game's original glyphs |
| `auto` | the device used last: a bound key, a mouse button or mouse motion selects `keyboard`; a pressed button, trigger or stick of a real pad selects the pad's family. Before any input: a connected pad, else keyboard |

Controller buttons are shown as remapped in `pad_bind_<button>`. The D-pad glyphs and unbound targets keep the
game's glyph. All symbols are drawn by the port itself (shapes and a system font); none come from other games.

## Settings menu (F10)

F10, or Back + Start on a pad, opens the menu over the game; F10, Escape, Back + Start, B or "Close" close it.
While it is open the game gets no input and opens its own pause menu (as with the console's guide button); after
closing, the game stays in its pause menu until you resume.

| Tab | Contents |
|---|---|
| Display | display mode, exclusive mode, monitor, window size, frame rate, VSync, frame pacing, frames ahead, HDR10 output, HDR paper white, HDR peak brightness, HDR highlight boost |
| Graphics | graphics API, render scale, render resolution, shadow resolution, shadow distance, shadow filter, ambient occlusion, detail distance, object view distance, anisotropic filtering, field of view, widescreen (Hor+), HUD layout, MSAA, SMAA, gamma ramp, colour blind mode |
| Input | mouse sensitivity (both axes, vertical), invert Y, acceleration and its limit, smoothing, zoomed behaviour and sensitivity, keyboard, mouse, button prompts, all 24 key bindings (Change, Clear, Default) |
| Controller | the controllers SDL or XInput sees and their port (1-4; port 1 also has keyboard and mouse), dead zone, vibration, controller driver, the controller buttons of all 16 Xbox buttons (Change, Add, Clear, Default, all to defaults) |
| Game | pause on focus loss, skip intro videos, frame rate display, speech volume, audio limiter, language |
| About | version, commit, graphics API, file paths, licence notice |

Settings that need a restart are marked "restart required" after a change. The menu can be navigated with keyboard
or pad.

On the **Controller** page, Change and Add wait for a controller button ("press a controller button"): the
capture starts once every button was let go (the A that pressed Change does not count), takes the next button,
trigger (past half) or extra button pressed on any controller, and gives up after 8 seconds or with Escape. Change
puts that button in place of the row's list, Add adds it; either way the button leaves every other row (a
controller button acts as one Xbox button). To swap A and B: Change on A, press B; Change on B, press A. Changed
rows are shown in blue. The bindings are stored in `predator_input.toml` (`[pad] pad_bind_*`) and work at once. The
menu itself always uses the controller's own buttons, so a mapping cannot lock you out of it. A controller keeps
its port until it is unplugged; a new one takes the lowest free port (XInput pads their XInput number).

**Defaults** (bottom right, on every page but About) sets the settings of the page shown back to their defaults;
the other pages stay as they are. On the Input page it also sets the 24 key bindings back, on the Controller page the
controller buttons of all 16 Xbox buttons (dead zone, vibration and controller driver are rows of that page). Settings decided by an
environment variable are left alone. On a Steam Deck or a ROG Xbox Ally X the device's defaults are the target (for
example frame pacing "Display" and 1280x800 on the Deck). Every choice list marks its default ("30 (console) -
default", on the Deck "Display - Steam Deck OLED default"), and the help of every setting names it.
`FCP_MENU_DEFAULTS=0` removes the button and the marks.

**Apply (restarts the game)** appears at the bottom while a "restart required" setting differs from the value the
game started with. It restarts the game as a new process, the way the game restarts itself between levels: window
position, size and display mode are kept. In the menus it restarts at once; in a level it first asks "Restart the
game?", because progress since the last checkpoint is lost. After the restart the game is in its main menu (with the
profile selected); on the title screen ("Press Start") it starts afresh. The **language** works this way: the game
reads the system language only once while it starts (measured: `XGetLanguage` is called only from the game's
start-up code), so it cannot change without a restart.

**Exit Game** (bottom left) asks "Exit to desktop?" ("The game ends. Progress since the last checkpoint is lost; the
settings are kept.") and then ends the game exactly like the window's close button: the sound stops, the window
closes and the process ends with exit code 0. Settings are already stored when they are changed.

In both questions A or Enter confirms and B or Escape goes back to the settings. F10 or Back + Start close the menu
and answer the question with no.

| Switch | Effect |
|---|---|
| `FCP_MENU_RESTART=host` | Apply always starts the game afresh (title screen) instead of using the game's own "return to main menu" restart |

Other keys: **Alt+Enter** and **F11** switch between window and borderless fullscreen (exclusive fullscreen when
`mode = "exclusive"`) and store the mode in `predator.toml` (not when `FCP_DISPLAY_MODE` is set in the environment);
the game never sees this Enter or F11, so F11 cannot be a key binding (`FCP_F11=0` leaves F11 to the bindings).

Not verified with a real keyboard: F10, F11 and Alt+Enter were tested with window messages sent to the game window.

## Exclusive fullscreen

`[display] mode = "exclusive"` puts the monitor into the video mode of `[display] exclusive_mode` while the game has
the focus: `desktop` keeps the monitor's own mode, `"WxH@Hz"` (for example `"2560x1440@60"`) takes that mode from the
monitor's list (the closest one when it is not listed exactly; `"WxH"` alone takes the desktop's refresh rate). In
the F10 menu: Display, "Display mode" and "Exclusive mode" (the list shows the modes of the monitor the window is on,
32 bit, 48 Hz and more); both switch at once.

- **Alt+Tab or a click on another window:** the game window minimizes and the desktop gets its own mode back; back in
  the game (Alt+Tab, task bar) the window goes fullscreen in its mode again.
- **F11 / Alt+Enter:** window <-> exclusive fullscreen; the window gets the desktop's mode.
- **End of the game, Alt+F4, a crash of the window:** the desktop's mode comes back.
- **A game started behind another program** (clicked while it loaded) does not keep the video mode: its window
  minimizes after about 1.5 s until you switch to it.
- **Restart** (Apply in the F10 menu, a level change): the new process takes the same mode; the monitor does not
  switch back and forth.
- `[display] fps = "auto"` follows the refresh rate of the mode in force (checked every 2 s).

Measured on a 3440x1440 120 Hz monitor at 125 % (one monitor), Vulkan and Direct3D 12, in `21_dunea`, keys sent with
`SendInput`: exclusive at 2560x1440@60, Alt+Tab away (desktop back at 3440x1440@120, window minimized) and back
(2560x1440@60 again), F11 and Alt+Enter twice each (window 1600x900 at the desktop mode, then exclusive again), and in
the F10 menu 1920x1080@120, borderless, exclusive, desktop, 2560x1440@100: every change reached the monitor within
0.5 s, the picture came back each time (screen shots), the frame rate settled at the new rate within one 4 s window
(60.00 / 120.00 / 100.00 frames/s with `fps = "auto"`), and 2 s after the end the desktop had its mode back.
Under Vulkan the driver reports the swapchain out of date after each mode change; the renderer makes it anew at once
(`FCP_RENDER_RECREATE_LOST=0`: only on a resize, as before). Not tested: a second monitor, HDR, G-Sync/FreeSync.

## Display scaling (DPI)

The program is per-monitor DPI aware (v2): window sizes, video modes and render resolutions are real pixels at every
Windows display scale.

- The built-in window size (`[display] width = 0`) is 1280x720 at 100 % and grows with the scale of the monitor it
  opens on (125 %: 1600x900, 150 %: 1920x1080, 200 %: 2560x1440; at most the monitor's work area, never below
  1280x720). A size set in `predator.toml` or chosen in the F10 menu stays exact pixels.
- A window moved to a monitor with another scale keeps its size on the screen (its pixel size changes by the ratio
  of the two scales), as Windows does it for its own windows. Fullscreen and maximized windows take the monitor's
  size.
- The F10 menu is laid out in points of the display scale and its text is drawn at the display's pixel density, so
  it keeps its size on the screen and stays sharp at 125, 150 and 200 %.

Back to the earlier behaviour: `FCP_DPI_WINDOW=0` (always 1280x720 pixels, the pixel size kept on a scale change),
`REX_IMGUI_DPI_RASTER=0` (menu text stretched from the point size), `FCP_DPI_EARLY=0` (DPI awareness declared only when
SDL starts). See [DEBUG-SWITCHES.md](DEBUG-SWITCHES.md).

Measured on a 3440x1440 monitor at 125 %: the built-in window opens at 1600x900 and the menu's 18-point text is drawn
at 22.5 pixels. 100, 150 and 200 % and the move to another monitor were checked with a simulated scale
(`FCP_TEST_DPI`): 1280x720, 1920x1080, 2327x1309 (the work area), and 1600x900 -> 2335x1333 -> 1278x712 -> 1600x900
for a move 125 -> 200 -> 100 -> 125 %; the menu followed each time and stayed sharp. A second monitor with another
scale was not available: the real move between monitors is not verified.

## Frame rate

`[display] fps` sets the frame rate limit: 30 (default, as the console), any whole number from 40 to 120, or `auto`
(the monitor's refresh rate, at most 120); with `unsupported_uncapped_fps = true` up to 240 (not supported). In the F10
menu it changes at once. Game time runs at wall-clock speed at
every rate; the places where the game logic depends on the frame count are corrected above 30. Details,
measurements and open points: [FRAMERATE.md](FRAMERATE.md).

## Other files and switches

| Item | Meaning |
|---|---|
| `portable.txt` beside `predator.exe` | keep the user folder in the executable's folder |
| `FCP_USER_ROOT=<folder>` | use this user folder |
| `FCP_USER_MIGRATE=0` | do not copy saves from an older `<exe>\user` folder into a new user folder (done once otherwise; the originals stay) |
| `FCP_SETTINGS_FILE=<path>` or `none` | another settings file, or none (built-in defaults, nothing stored) |
| `rexglue.toml` in the user folder | optional SDK settings, only read when present |
| `renderer.toml` beside the EXE | renderer layout; its `backend` is used by `[graphics] backend = "auto"` |
| `FCP_IMAGE_SEARCH=0` | do not look for the disc image when `[game] image` is empty or does not fit |
| `FCP_SAVE_GUARD=0` | save protection off: no backups, no repair at the start, no wait for a save at the end, a save replaces the old one by deleting it first (the SDK's way); the check at the start is still logged |
| `FCP_SAVE_ATOMIC=0` | only the atomic replacement off (the old save is deleted before the new one is written); backups and repair stay |
| `FCP_SAVE_BACKUPS=<n>` | backup generations kept per saved game in `save_backup\` (default 3, 0 = none) |
| `FCP_SAVE_EXIT_WAIT_MS=<ms>` | how long closing the window or *Exit Game* waits for a save in progress (default 5000) |
| `FCP_DEVICE=deck\|ally\|pc` | force the device detection: Steam Deck OLED defaults, ROG Xbox Ally X defaults, or plain PC defaults (see the README sections on handhelds) |
| `FCP_DECK_FPS45=0` | keep 30 frames/s as the Steam Deck OLED default (from v0.1.2 on it is 45; the LCD model keeps 30) |

Switches for the first start (game code and shader pack build) are listed in the README; switches back to the
original behaviour in [DEBUG-SWITCHES.md](DEBUG-SWITCHES.md).
