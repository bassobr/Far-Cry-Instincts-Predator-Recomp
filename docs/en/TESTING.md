# Test tools

Switches for reproducible test runs, traces and measurements. All of them are environment variables read once at
start; all are off unless stated. They are meant for contributors, not for play: some of them change what the game
does (marked "changes the game").

The run scripts set useful defaults: `tools\boot\run.ps1` starts one `predator.exe` with `FCP_NO_DIALOGS=1`,
`FCP_MUTE=1`, `FCP_EXIT_AFTER=<Seconds>` and a 640x360 window, passes `-Env "A=1;B=2"` on, and ends only the process
it started. `tools\boot\run_chain.ps1` does the same for a chain of relaunched processes (every level change starts
a new process; generation 0 is the first one). See [BUILDING.md](BUILDING.md) for the sweep and transition scripts.

Logs go to `predator.log` in the user folder (generation n: `predator.gen<n>.log`), CSV files and dumps to `logs\`.
Dumps and traces are derived from the game: keep them out of the repository and delete them after use.

## Starting and controlling a run

| Switch | Meaning |
|---|---|
| `FCP_START_LEVEL=<level>[:<mission>[:<players>]]` | start a level without the menu, the way the game restarts a level (launch data with `-map <level>`); only generation 0. Level names as on the disc, e.g. `00_training`, `21_dunea` |
| `FCP_START_PORT=<0..3>` | controller port of that start |
| `FCP_START_SAVE_PROFILE=0` | the development start does not save the profile (it does by default, as the menu path does before the first level) |
| `FCP_TEST_COMMAND=<s>:<command>[\|...]` | push a game command at s seconds, e.g. `ReturnToMenu`, `FadeOutRestartStartLevel level=00_training`; `FCP_TEST_COMMAND_GENS=<g,...>\|all` (default 0). Changes the game |
| `FCP_TEST_TELEPORT=[<level>@]<s>:<x>,<y>,<z>[\|...]` | put the local player at a position at level time s; with `<level>` in every generation that has this level, else in `FCP_TEST_TELEPORT_GENS` (default 0). Repeats every 5 s if the game does not take it (vehicle, sequence). Changes the game |
| `FCP_TEST_PATCH=<old>\|<new>` (also `FCP_TEST_PATCH2` ... `9`) | same-length replacement in every decompressed archive entry (e.g. a spawn position in a level file); `FCP_TEST_PATCH_RAW=1` also in raw file reads. Changes the game |
| `FCP_KEYBOARD_TEXT=<text>[\|<text>...]` | default text of the on-screen keyboard (call n takes entry n, `{n}` = call number), e.g. a save name |
| `FCP_EXIT_AFTER=<s>` | clean shutdown after s seconds; a thread dump if the process still lives 15 s later |
| `FCP_NO_DIALOGS=1` | no message boxes; system dialogs answer at once |
| `FCP_MUTE=1` | no sound output |
| `FCP_USER_ROOT=<folder>` | own user folder for the run (settings, saves, logs) |

## Scripted input

| Switch | Meaning |
|---|---|
| `FCP_INPUT_SCRIPT=<entry>,...` | pad input written into the game's input after the SDK: `<t>[-<t_end>]:<control>[+<control>...][:<value>][@<player>]`. Controls: `a b x y start back up down left right lb rb ls rs`, sticks `lup ldown lleft lright rup rdown rleft rright`, triggers `lt rt`, own value `name=<0..1>`. Without `<t_end>` held for `FCP_INPUT_HOLD_MS` (150). Example: `34:start,38:start,42:a`, `10-30:lup`, `8-9:lup=1+rright=0.3+rt` |
| `FCP_INPUT_SCRIPT_CLOCK=wall\|level` | `wall` (default): seconds since the title launch; `level`: game time since the level start (stops while paused, the same at every frame rate) |
| `FCP_INPUT_SCRIPT_GENS=<g,...>\|all` | relaunch generations that run the script (default 0) |
| `FCP_KEY_SCRIPT=<entry>,...` | keyboard and mouse through the real binding path: `<t>[-<t_end>]:<key>[+<key>...]` holds keys (names as in `[bindings]`), `<t>[-<t_end>]:mx=<n>` / `my=<n>` moves the mouse by n counts spread over the interval; same clock as `FCP_INPUT_SCRIPT_CLOCK`, generations `FCP_KEY_SCRIPT_GENS` |
| `FCP_VIRTUAL_PADS=<n>` | n virtual SDL 3 controllers (1-4), so the whole path SDL -> SDK -> `XamInputGetState` is tested; `FCP_VIRTUAL_PAD_SCRIPT=<s>:<step>[+<step>],...` with buttons, `lx=`/`ly=`/`rx=`/`ry=`/`lt=`/`rt=` values, `unplug`/`plug`, prefix `p<k>.` for pad k; `FCP_VIRTUAL_PAD_ID=<vid>:<pid>`, `FCP_VIRTUAL_PAD_HOLD_MS` |
| `FCP_TEST_MENU=<t>:<action>,...` | drive the F10 menu: `open`, `close`, `toggle`, `tab=<name>`, `dump=<file.png>`, `set=<section>.<key>=<value>`, `input=<section>.<key>=<literal>`, `post=<key>`, `pad=<buttons>`; `FCP_TEST_MENU_GENS` |
| `FCP_TEST_FOCUS=<t>:<s>,...` | simulated focus loss at t for s seconds |
| `FCP_TEST_ALT_ENTER=<s>[,<s>...]` | Alt+Enter posted to the own window (arrives only with focus); `FCP_TEST_ALT_ENTER_GENS`; `FCP_TEST_ALT_ENTER_DIRECT=1` runs the Alt+Enter action on the UI thread instead (no focus needed, skips the key path) |

## Traces

| Switch | Output |
|---|---|
| `FCP_TRACE_STATE=<n>` | every n-th frame one line in `logs\state[.gen<n>].csv`: frame, wall/game/level time, frame time, position, angles, speed, health, armour, water state, breath, firing/reloading, clip counters, shots |
| `FCP_TRACE_COMMANDS=1` | `logs\commands[.gen<n>].csv`: every queued and dispatched game command, level loaded |
| `FCP_TRACE_HITCHES=<ms>` | frames over the threshold (`1` = 50 ms) with file reads, file opens, run-time shader compiles, allocations and the frame's commands |
| `FCP_TRACE_LAND=1`, `FCP_TRACE_TIMERS=1`, `FCP_TRACE_AI=1` | landing speed per landing; corrected (and held) script timers; count of thinking turns of computer-controlled characters |
| `FCP_TRACE_BOAT=1`, `FCP_TRACE_WHEELS=1`, `FCP_TRACE_DROWNING=1` | boat physics steps and speed; wheel force calls per vehicle; every "under water" answer |
| `FCP_TRACE_LIVING=<x>,<y>\|player` | one line per physics step for the first living entity within 2 m of (x, y), or the local player (needs `FCP_TRACE_STATE`) |
| `FCP_TRACE_PATH=1`, `FCP_TRACE_FIRE=1`, `FCP_TRACE_EVENTS=<from>-<to>` | path following of the traced entity; every fire test of the local player; every script event in a level time window with the guest call chain |
| `FCP_TRACE_LOOK=<n>`, `FCP_TRACE_RUMBLE=<n>`, `FCP_TRACE_CAMERA=1`, `FCP_TRACE_WIDE=1` | look input (stick, game values, mouse, zoom); rumble; camera setup; each new kind of 2D draw over a wide 3D picture |
| `FCP_TRACE_IMPORTS=1\|2`, `FCP_TRACE_FILES=1` | kernel/XAM imports (2 = every call, very much); every file opened by the game |
| `FCP_TRACE_D3D=<frame>[,<count>]` | every D3D hook call of these frames with registers and decoded arguments |
| `FCP_TRACE_DRAWS=1`, `FCP_RENDER_TRACE=<first>[,<count>]` | renderer draw log |
| `FCP_INPUT_TRACE=0` | turns off the input trace that is on by default (`FCP_INPUT_TRACE_LINES`, default 200) |
| `FCP_TRACE_SOUND_READS=1` | reads of the sound archive |

## Dumps

| Switch | Output |
|---|---|
| `FCP_DUMP_FRAMES=<file.png>` | pattern for frame dumps (`%d` = frame, `%g` = relaunch generation), relative to the log folder; which frames: `FCP_DUMP_FRAME_AT=<n>[,<n>...]` or `FCP_DUMP_FRAME_EVERY=<n>` |
| `FCP_DUMP_PRESENT=1` | additionally the presented image (swap chain, with SMAA and bars) as `<name>_front.png` |
| `FCP_DUMP_RESOLVES=<n>` | every RGBA8 resolve target of frame n |
| `FCP_DUMP_DEVICE=<n>[,<n>...]` | the guest D3D device structure at swap n (`logs\device_<n>.bin`) |
| `FCP_DUMP_SHADERS=1` | the game's shader containers to `logs\shaders\` |
| `FCP_PROMPTS_DUMP=1`, `FCP_PROMPTS_LOG=1` | every redrawn button prompt page as PNG; every checked texture with its key |
| `FCP_RECORD_AUDIO=<file.wav>` | the summed mix as 48 kHz float WAV plus a gap list (`<file.wav>.gaps.tsv`) |
| `FCP_TEST_GUARDED_STRING=1` | self-test of the guarded guest string read (copies across an uncommitted page and logs the result); `FCP_GUEST_STRING_INLINE_TRY=1` restores the old, unprotected form |
| `FCP_WATCHDOG_TEST=<s>` | one thread dump at s seconds (the watchdog itself dumps all host and guest stacks after `FCP_WATCHDOG_SECONDS` without a swap, default 5, at most 3 times; `FCP_WATCHDOG=0` turns it off) |

## Measurements

| Switch | Output |
|---|---|
| `FCP_FPS_STATS=<s>` | a `framestat:` line every s seconds (default 10, 0 = off): frames/s, mean, p50, p99, max, 1 % low, game time against wall time, level, histogram; `framestat total` at the end |
| `FCP_CPU_STATS=<s>` | a `cpu:` line every s seconds: process CPU per wall second (100 % = one core), user/kernel, frames/s, and every thread with at least 2 % of a core; totals at shutdown |
| `FCP_PROFILE=<ms>` | sampling profile of all guest threads every ms: CPU share, where the samples are (game code, SDK, system, driver), hottest guest functions and host callers. `FCP_PROFILE_WINDOW=<s>` (default 10), `FCP_PROFILE_TOP=<n>`, `FCP_PROFILE_DEPTH=<n>`; `tools\profile_symbolize.py` resolves host addresses |
| `FCP_GAMETIME_STATS=0` | turns off the `clock:` line (every 5 s: frames/s, game time against wall time, frame time range, level) |
| `FCP_RENDER_STATS=<n>` | renderer statistics every n frames (default 150) |
| `FCP_AUDIO_STATS=0` | turns off the audio and XMA statistics lines (every 10 s of audio) |
| `FCP_FP_STATS=1` | counts the slow paths of the PowerPC floating point rules per guest address, written at shutdown |
| `FCP_STATS=0` | turns off the `stat` lines |

`tools\perf_measure.ps1` runs a level several times with `FCP_CPU_STATS` and `FCP_FPS_STATS` and summarises them
(`-Affinity` limits the process to a set of logical processors, e.g. 4 cores / 8 threads). `tools\audit\run_series.ps1`
runs frame rate series (30/60/120, several repeats) for the [frame rate audit](FRAMERATE.md).

**Other processes falsify measurements.** Measure with no other game process running; the series scripts count
other game processes before and after each run and log them. Repeat runs that had company.

## Picture comparison with a reference

`tools\compare\` compares frame dumps against reference images taken with the Xenia Canary emulator (BSD-3):
`canary_capture.py` starts one Canary process with your disc image, sends keys only to its own window and takes
screenshots at fixed times; `compare.py pair|match|series` reports per image region the mean absolute deviation, the
brightness deviation and the correlation of edge images, plus PSNR. Pixel equality is not expected (water, video,
timing). Reference images are derived from the game: keep them under `work\` and never commit them.
