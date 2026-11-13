# Frame rate and game logic

The game was written for 30 frames per second. The port can run it at 30 (default), 60 or 120. This page lists how
the frame rate is set, which parts of the game logic depend on the frame count, what was corrected, what was
measured and what is still open.

Everything marked "measured" was seen in test runs (state traces on game time, logs, frame dumps). "Image" means
read from the disassembled game code. "Not verified" means neither.

## How the frame rate is set

The original paces itself: once per frame its render thread reads the variable `r_vsynch` (start value 2) and sets
the device's present interval; the D3D library then writes a GPU wait for that many vblanks of the 60 Hz video
mode into the command ring (image, measured). `FCP_FPS` (`[display] fps`) sets the value the game itself reads:

| `FCP_FPS` | Game side | Host side |
|---|---|---|
| `30` (default) | original: `r_vsynch` = 2 (two vblanks at 60 Hz) | present with VSync |
| `60` | `r_vsynch` = 1 (one vblank) | present with VSync |
| `120` | `r_vsynch` = 0 (no vblank wait) plus a host limiter: at least 1/120 s between two frames (at most one frame of credit, re-anchored after loading pauses) | present without waiting for the monitor (Vulkan mailbox; Direct3D 12 `Present(0, 0)`) |

The vblank rate of the SDK's null GPU driver stays 60 Hz, so the video mode the game queries is unchanged. Over
60 frames/s the host present must not wait for the monitor: measured at 120 frames/s on a 120 Hz monitor, a FIFO
present held the renderer lock and made every third frame 16 ms long.

Diagnostic values (not for play): `FCP_FPS=<n>` (limiter at n; above 120 not verified, logged), `FCP_FPS=unlimited`,
`FCP_FPS_PACING=limiter` (60 through the limiter instead of a vblank), `FCP_PRESENT_INTERVAL=1|2|immediate`
(`r_vsynch` directly, wins over `FCP_FPS`), `FCP_FPS_LIMIT` (older name of `FCP_FPS`). The settings file only
accepts 30, 60 and 120.

**Game time.** The game's frame timer measures real frame time (rounded to whole milliseconds, clamped to 0.1 s).
Measured: game time against wall time 1.000 at 30, 60 and 120 frames/s. The game does not run "too fast"; what
remains are places that count frames instead of seconds.

## Corrections

Each correction is active only when the target is not 30 **and** the frame (or physics step) is shorter than
30 ms. At the default of 30 the game runs unchanged; measured: the 30 frames/s runs with the corrections give the
same values as without them. Values the corrections change are changed only for the duration of the call and
written back afterwards; nothing is written into the game's code or constants.

Most places were measured with three runs per rate (30, 60, 120) and one or more runs with the correction off at
120 ("Original").

| Area | Cause (image, measured) | Original at high frame rates (measured, 120 unless noted) | Now (measured) | Back to original |
|---|---|---|---|---|
| Fall damage | the landing speed is taken about 2.4 frames before the impact | the same jump sends 4.60 instead of 4.09 m/s (11 ms frames), so more damage | 4.03-4.21 m/s at 15-28 ms frames | `FCP_FRAME_FALL=1` |
| Drowning damage | the script timer deals damage per call times the frame time | 2.9 instead of 7.3-7.9 HP/s; death after 33.7 instead of 11.2 s (12 ms frames) | 7.2-8.6 HP/s | `FCP_FRAME_TIMERS=1` |
| View bob in water | the bob angle is summed per frame | tilt up to 13.8 instead of 3.3-5.0 degrees (12 ms frames) | 4.8 degrees | `FCP_FRAME_BOB=1` |
| Boats (computer-controlled and the player's) | the water model subtracts drag per call, one call per physics step | patrol boat 5.46 instead of 11.74-11.84 m/s; player's boat 5.34 instead of 5.81-5.83 m/s 3 s after full throttle (at 15.8 ms frames) | the console's model rate (at most 50 calls per second of game time): 11.89 m/s; player's boat as at 30 | `FCP_FRAME_WATER=1` |
| Swimming: buoyancy and currents | applied per frame | after the jump from the helicopter in `13_black_forest` the player rose at 0.003 instead of 0.37 m/s, stayed under water and drowned | 0.367 m/s at 30, 60 and 120; surface after 3.8-4.7 s | `FCP_FRAME_SWIM=1` |
| First landing after take-off | with steps shorter than a console frame the flight can end in its first step | flight over a stair edge in `05_mines` 0.02 instead of 0.60 s | 0.68 s | `FCP_FRAME_LANDING=1` |
| Entering water | swim mode starts one step after crossing the water line, so the depth depends on the step length | 0.14-0.28 instead of 0.55-1.1 m below the line; surfaced after 30.3-30.6 instead of 31.4-32.8 s | depth and time in the range of 30 | `FCP_FRAME_DIVE=1` |
| Stick turn ramp | the turn rate ramp is summed per frame | 1 s of full stick: 253.0-253.2 instead of 249.6-249.8 degrees | 249.6-250.2 degrees | `FCP_FRAME_TURN=1` |
| Path following of computer-controlled characters | the look-ahead is one frame's movement | 3-4 cm instead of 13-15 cm | 13-15 cm, as at 30 | `FCP_FRAME_PATH=1` |
| Fire rate | the fire test runs once per frame, so the interval is rounded up to whole frames | assault rifle 9.62-9.64 instead of 8.97-9.07 shots/s; after reloading 75 instead of 100 ms per shot | the test runs on the console's 1/30 s grid: 8.97 shots/s at 120, 8.61-9.15 at 60; 100 ms after reloading | `FCP_FRAME_FIRE=1` |

Notes:

- **Fall damage:** only the speed sent to the landing was measured; the damage itself was not. The factor 2.4 can be
  changed with `FCP_FALL_SAMPLE_FRAMES=<k>`.
- **Drowning timer:** the correction scales `_frametime` for every entity script timer, not only drowning; in the
  game's scripts `_frametime` is read inside a timer only there (image).
- **Path following:** the correction also contains a release from corners (`FCP_FRAME_PATH_RELEASE=1` turns only
  that off). Its constants come from the author's earlier *Far Cry Instincts* port and were not re-measured here. In
  the measured scenario the behaviour did not change with or without the correction.
- **Fire rate:** for pistol and shotgun (0.2 s interval, tapped) no difference was measurable. The mounted machine
  gun was measured once at 120 with the correction (7.50 shots/s); the full series is open.

## Measured without a dependency on the frame rate

| Area | Measurement |
|---|---|
| Walking speed | 5.15 m/s at 33, 17 and 10 ms frames |
| Jump height | 1.107 / 1.107 / 1.108 m |
| Breath under water | 2.000 per second |
| Mounted machine gun (`21_dunea`) | 30 / 29 / 31 shots in 4 s |
| Video playback | 300 video frames in 10 s at 30, 43 and 119.5 frames/s (the video player paces by time) |
| Menu key repeat | first repeat after 0.83-0.90 s, then 9.2-9.7 lines/s at 30, 60 and 120 |
| Game time against wall time | 1.000 at 30, 60 and 120 |

## Experiments (off by default)

| Switch | What it does | Why it stays off |
|---|---|---|
| `FCP_FRAME_EDGE=0` | step edge test with the angle scaled to a console step (through the `tan` call of the movement code, without writing to game constants) | no coupling was measurable on the stairs of `05_mines` (player and computer-controlled character) |
| `FCP_FRAME_TIMER_TICK=0` | holds entity timer ticks until the console would have fired them (a 50 ms timer ticks every 50-58 ms at 120, every 66.7 ms at 30) | no effect on the measured scenarios; one side effect of a held tick is not understood |
| `FCP_AI_CONSOLE_TICK=1` | computer-controlled characters think at the console's rate (7.5 times per second) | it did not move the measured timelines (see below) |

## Still open

Measured, not corrected:

- **Thinking rate of computer-controlled characters.** Each one thinks every fourth frame: 7.5 times per second at
  30, 30 times at 120 (image, measured). Not changed, because no effect on play was measured.
- **Timelines of computer-controlled characters.** In `05_mines` a character reacts 0.12 s earlier at 120 and starts
  its second path 0.44 s earlier (3 of 3 runs against 3 of 3). Part of it is explained: two latencies of three frames
  each between script states (0.075 s at 120 instead of 0.2 s at 30). The mechanism is not located. Neither the
  thinking rate nor the timer rate moves it. After about 25 s the same character stands still at 120 in 8 of 8 runs;
  at 30 it moves on in some runs (3 of 5), so the cause is not known.
- **Scripted drive** in `21_dunea` starts 0.35-0.47 s earlier at high frame rates; not caused by the thinking rate.
- **Wheeled vehicles** reach 1.5-3 % higher top speed at 120 (player's vehicle in `10_dead_marshes`). Not corrected:
  the deviation is small, and skipping wheel force calls would change the body's collision.
- **Reloading** is about 0.05 s shorter at 60 and 120 (one frame of rounding in the reload sequence, not the fire
  test).
- **At 30 itself** the fire interval depends on the jitter of the millisecond clock (5-9 of 30 intervals take four
  instead of three frames). How much a real console jitters is not known.

Not examined yet: ladder and vine ends, slope edges, particles and HUD animations, semi-automatic weapons tapped
faster than their interval, other ways of entering water (cliff, boat).

**At most 120 frames/s.** The settings accept only 30, 60 and 120; the game logic was not checked above 120.

## Method

Runs use the development start (`FCP_START_LEVEL`), scripted input on the level clock (`FCP_INPUT_SCRIPT_CLOCK=level`,
so the script is the same at every frame rate) and the state trace (`FCP_TRACE_STATE`, one CSV line per frame:
position, angles, speed, health, water state, ammunition). `tools\audit\run_series.ps1` runs a series of rates and
repeats and counts other game processes before and after each run. Some runs were made while other processes loaded
the machine; the tables therefore use the measured mean frame time rather than the target. See
[TESTING.md](TESTING.md) for the switches.
