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
| `image` | `FCP_GAME_ISO` | `""` | path of the disc image (`.iso`) or of a folder with the extracted disc; relative to the user folder | next start |
| `pause_on_focus_loss` | `FCP_PAUSE_ON_FOCUS_LOSS` | `false` | `true`: while the window has no focus the game opens its pause menu (in a level), the sound is muted and vibration stops; back in the window the game stays in its pause menu | at once |
| `skip_intro` | `FCP_SKIP_INTRO` | `false` | `true`: skip the logo and intro videos at start; story cutscenes stay | at once |
| `fps_counter` | `FCP_FPS_COUNTER` | `false` | `true`: frame rate and frame time display in a corner | at once |
| `language` | `FCP_LANGUAGE` | `"english"` | `english`, `french`, `german`, `spanish`, `italian`, `auto` (the Windows display language; English if it is none of these). Sets the console's system language that the game reads | next start (F10: Apply restarts) |

### `[display]`

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `mode` | `FCP_DISPLAY_MODE` | `"windowed"` | `windowed`, `borderless` (fullscreen window), `exclusive` (fullscreen video mode, not tested). Alt+Enter toggles window/fullscreen and stores the choice here | at once |
| `width`, `height` | `FCP_WINDOW_WIDTH`, `FCP_WINDOW_HEIGHT` | `0` | window client size in pixels; `0` = 1280x720. The game itself renders at 1280x720 unless `[graphics] scale` is set | at once |
| `monitor` | `FCP_MONITOR` | `0` | `0` = the system's choice (primary), `1`..`n` = display number (not tested) | next start |
| `fps` | `FCP_FPS` | `30` | `30` (as the console), `60`, `120`; see [FRAMERATE.md](FRAMERATE.md) | next start |
| `vsync` | `FCP_RENDER_VSYNC` | `"auto"` | `auto` (wait for the monitor up to 60 frames/s, not above), `on`, `off` | next start |

### `[graphics]`

All graphics defaults give the console's picture.

| Key | Variable | Default | Values | Applies |
|---|---|---|---|---|
| `backend` | `FCP_RENDER_BACKEND` | `"auto"` | `auto` (the `backend` of `renderer.toml` beside the EXE, else Vulkan), `vulkan`, `d3d12` | next start |
| `msaa` | `FCP_RENDER_MSAA` | `"console"` | `console` (2x on the screen targets, 4x on the reflection, as the console), `off`, or `2`, `4`, `8` samples on the screen-sized targets (limited to what the GPU supports). Older files: `true` = console, `false` = off | next start |
| `smaa` | `FCP_RENDER_SMAA` | `false` | `true`: SMAA 1x edge smoothing on the finished picture | at once |
| `gamma` | `FCP_RENDER_GAMMA` | `true` | the game's gamma ramp (its brightness setting) | at once |
| `scale` | `FCP_RENDER_SCALE` | `1` | render resolution of the screen-sized targets: `1` = 1280x720 as the console, `auto` = output height / 720 (follows window size changes), or a factor or percentage from 0.5 to 4 (`1.5`, `150%`, `2x`). HUD and text get sharper with it | at once |
| `shadow_scale` | `FCP_RENDER_SHADOW_SCALE` | `1` | shadow map resolution as a multiple of the console's 1344x1344 (0.5 to 4) | next start |
| `aniso` | `FCP_RENDER_ANISO` | `1` | anisotropic filtering 1-16 for linearly filtered samplers; `1` = as the game sets it | next start |
| `fov` | `FCP_RENDER_FOV` | `1` | field of view as a multiple of the game's (0.5 to 2, `1.2` or `120%`); the weapon and hands keep their own projection | next start |
| `aspect` | `FCP_RENDER_ASPECT` | `"auto"` | shape of the 3D picture: `auto` = the output's shape when it is wider than 16:9 (Hor+: the console's vertical field of view, more to the sides), `16:9` (or `off`) = as the console with bars at the sides, or a fixed shape (`21:9`, `32:9`, `2.39`). Outputs of 16:9 or narrower (16:10, 4:3) always show 16:9 with bars | next start |

Which targets are scaled: targets at least 640 pixels wide and wider than high get `scale`, square targets from
1024 get `shadow_scale`, all smaller post-processing targets stay at console size (the game passes their pixel sizes
in shader constants). Measured in `21_dunea` at 120 frames/s: `scale = 1` 119.97-120.00 frames/s, `scale = auto`
(2560x1440 internally) 119.89-120.00 frames/s on a desktop GPU.

**Wide outputs (21:9, 32:9).** With `aspect = "auto"` the 3D picture fills the width; HUD, text, button prompts and the pause menu stay 16:9 in the middle, the main menu, loading screens and videos stay 16:9 with bars. Measured at 1720x720, 2560x1080, 3440x1440 and 32:9: HUD undistorted, 120 frames/s in `21_dunea` at 3440x1440 as with bars. Not verified: binocular and scope masks, subtitles, Direct3D 12, a window resized while playing.

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
| `pad_bind_<button>` | - | the button itself | the controller buttons that act as this button of the Xbox pad, for all players |

Buttons for `pad_bind_<button>` and its values: `a b x y lb rb lt rt ls rs start back up down left right`
(`up`..`right` = D-pad), by their place on an Xbox pad. PlayStation names work too (`cross circle square triangle
l1 r1 l2 r2 l3 r3 options select share create`), as do `south east west north`. Several buttons with commas,
`none` for none. A button that the file gives to another button is removed from the defaults; to swap two buttons,
set both.

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
| Display | display mode, monitor, window size, frame rate, VSync |
| Graphics | graphics API, render scale, shadow resolution, anisotropic filtering, field of view, widescreen (Hor+), MSAA, SMAA, gamma ramp |
| Input | mouse sensitivity (both axes, vertical), invert Y, acceleration and its limit, smoothing, zoomed behaviour and sensitivity, keyboard, mouse, controller driver, dead zone, vibration, button prompts, all 24 key bindings (Change, Clear, Default) |
| Game | pause on focus loss, skip intro videos, frame rate display, language |
| About | version, commit, graphics API, file paths, licence notice |

Settings that need a restart are marked "restart required" after a change. The menu can be navigated with keyboard
or pad.

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

Other keys: **Alt+Enter** switches between window and borderless fullscreen and stores the mode in
`predator.toml` (not when `FCP_DISPLAY_MODE` is set in the environment); the game never sees this Enter.

Not verified with a real keyboard: F10 and Alt+Enter were tested with window messages sent to the game window.

## Frame rate

`[display] fps` selects 30 (default, as the console), 60 or 120 frames per second. Game time runs at wall-clock
speed at every rate; the places where the game logic depends on the frame count are corrected above 30. Details,
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
| `FCP_DEVICE=deck\|pc` | force the device detection (Steam Deck or PC). No device-specific defaults are set at the moment |

Switches for the first start (game code and shader pack build) are listed in the README; switches back to the
original behaviour in [DEBUG-SWITCHES.md](DEBUG-SWITCHES.md).
