# Frame rate and game logic

The game was written for 30 frames per second. The port can run it at 30 (default) or at any rate from 40 to 120,
or at the monitor's refresh rate (`auto`, at most 120). Up to 240 is possible as an unsupported option
([Above 120](#above-120-unsupported)). This page lists how the frame rate is set, which parts of the game logic depend
on the frame count, what was corrected, what was measured and what is still open.

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

Every other rate (40-59, 61-120) runs like 120: no vblank wait, host limiter at the rate; up to 60 the host
present waits for the monitor, above 60 it does not. `auto` takes the refresh rate of the monitor the window is on
(59 and 119 Hz, as Windows reports 59.94 and 119.88, count as 60 and 120), at most 120; under 40 Hz it gives 30. It
looks at the monitor again every 2 s, so moving the window to another monitor changes the rate. On the 90 Hz
display of a Steam Deck OLED `auto` means 90 without display pacing; measured in `21_dunea` it ran at 73-77
frames/s, unevenly. 45 (the Deck's default, paced on the display) is smoother there.

**Changing the rate while playing.** In the F10 menu frame rate, VSync and frame pacing apply at once: the game side
(`r_vsynch`, limiter, the target the corrections look at) from the next frame, the host present mode (Vulkan: the
swapchain is made anew with FIFO or mailbox; Direct3D 12: the sync interval) and display pacing from the next
present. The `framestat:` window in progress ends early at the change, so every line belongs to one rate.
`FCP_FPS_LIVE=0` keeps the change for the next start. Measured in `21_dunea` on a 120 Hz monitor, one run per API,
changed through the menu while playing: Vulkan 30 → 45 → 60 → 90 → 120 → auto → 30 → 40 gave 45.00, 60.00, 90.00,
120.00, 120.00, 30.00 and 40.00 frames/s from the first full 4 s window after each change (game time against wall
time 1.000; the swapchain went to mailbox at 90 and back to FIFO at 30); Direct3D 12 30 → 60 → 120 → 45 → 30 → 75
gave 60.00, 120.00, 45.00, 30.00, 75.00. Not measured: the switch of display pacing on a Steam Deck (on the
Windows test PC the renderer did not measure the 120 Hz refresh from a window, so display pacing stayed off there;
the new target was taken, logged and the game paced itself at it).

Diagnostic values (not for play): `FCP_FPS=<n>` (limiter at n, any n; above 120 not verified, logged),
`FCP_FPS=unlimited`, `FCP_FPS_PACING=limiter` (60 through the limiter instead of a vblank),
`FCP_PRESENT_INTERVAL=1|2|immediate` (`r_vsynch` directly, wins over `FCP_FPS`), `FCP_FPS_LIMIT` (older name of
`FCP_FPS`), `FCP_FPS_AUTO_HZ=<hz>` (`auto` as on a monitor with that refresh rate).

### Which frame rates

`[display] fps` and the F10 menu accept 30, 40 to 120 and `auto`, nothing else. The reason is in the corrections
below (image, `src/fcp/frame_coupling.cpp`): each one is written in frame time, not for a particular rate, and acts
only when the target is not 30 **and** the frame is shorter than 30 ms. A target of 31-39 frames/s gives frames of
26-32 ms, close to or over that limit: some frames would be corrected and some not, and the uncorrected ones still
count frames (the fire test rounds a 100 ms interval up to four frames of 31 ms). From 40 frames/s (25 ms) the
limiter's jitter stays clear of it. Above 120 only with `[display] unsupported_uncapped_fps = true`, up to 240
(one correction assumes at most 240 steps per second); see [Above 120](#above-120-unsupported).

| Rate | Game logic |
|---|---|
| 30 | the console's; every correction is idle (measured: the same values as without them) |
| 60, 120 | corrected; the audit runs of the table below (measured) |
| 45 | corrected; fire rate, swimming and landing checked once each (measured, Steam Deck pacing round); further checks at 45 are under way |
| 40-119 others | the same corrections act (image); not measured one by one |
| 121-240 | only with `unsupported_uncapped_fps = true`; not supported, audited at 144 and 240 (below) |
| 31-39, over 240 | not accepted by the settings (only `FCP_FPS=<n>` for diagnostics) |

## Display pacing

A steady frame rate is not yet a smooth picture: every frame has to stay on the screen equally long. On the Steam
Deck OLED (89.6 Hz measured) 60 frames/s cannot do that: frames alternate between one and two refreshes (11 and
22 ms), measured 50 % / 45 % with about 45 cadence changes per second even while the game holds 60.0. And the
game's own clock (the null GPU driver's 60 Hz vblank, or the host limiter) runs apart from the display's, so even 30
and 45 drift: measured 10-31 % (30) and 9-21 % (45) of the frames one refresh too short or too long.

`[display] pacing = "display"` (`FCP_FPS_PACING=display`, the default on the Steam Deck) times the game by the
display instead: the Vulkan renderer sees when each image reaches the display (`VK_KHR_present_wait`), measures the
refresh period, and for a frame rate the refresh rate is a multiple of (n = 2 or more refreshes per frame) it
- holds the present of a frame until the (n-1)-th refresh of the previous one has started (FIFO then shows it at
  the n-th), and
- lets the game's render thread continue only once its previous frame is on the display, so the game renders each
  shown frame once and its frame time is even; the game's vblank wait and the host limiter are off meanwhile.

Measured on the Deck (21_dunea, the heaviest level tested; 99-100 % in the three others): 30 frames/s 97-99 % of the frames exactly three refreshes
(the game's own pacing: 84 %), 45 frames/s 98 % exactly two (own pacing: 79 %), at less power (45: 10.4 W instead of
13.7 W). Without display timing (Direct3D 12), until the refresh is measured, at one refresh per frame (60 on a 60 Hz
display, where FIFO alone keeps it even) and above 60 the game paces itself as before. Diagnostics: a
`presentstat:` line every 10 s in the log (refreshes per frame, cadence changes, latency); switches in
[DEBUG-SWITCHES.md](DEBUG-SWITCHES.md).

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
| Fall damage | the landing sends the mean speed of the last physics step in the air, and which step that is depends on where the frames fall | the same jump sends 4.47 / 4.50 / 4.63 / 4.70 instead of 4.08-4.10 m/s at 45 / 60 / 90 / 120 | the console's steps are worked out from the flight: 4.087-4.088 m/s at 45, 60, 90 and 120; falls of 2.5-12 m do the same damage as at 30 | `FCP_FRAME_FALL=1`; `FCP_FALL_MODEL=linear` for the earlier estimate |
| Drowning damage | the script timer deals damage per call times the frame time | 2.9 instead of 7.3-7.9 HP/s; death after 33.7 instead of 11.2 s (12 ms frames) | 7.14 HP/s and death after 13.9 s at 30, 45, 60 and 120 frames/s (measured, 2 runs each) | `FCP_FRAME_TIMERS=1` |
| View bob in water | the bob angle is summed per frame | tilt up to 13.8 instead of 3.3-5.0 degrees (12 ms frames) | 4.8 degrees | `FCP_FRAME_BOB=1` |
| Boats (computer-controlled and the player's) | the water model subtracts drag per call, one call per physics step | patrol boat 5.46 instead of 11.74-11.84 m/s; player's boat 5.34 instead of 5.81-5.83 m/s 3 s after full throttle (at 15.8 ms frames) | the console's model rate (at most 50 calls per second of game time): 11.89 m/s; player's boat as at 30 | `FCP_FRAME_WATER=1` |
| Swimming: buoyancy and currents | applied per frame | after the jump from the helicopter in `13_black_forest` the player rose at 0.003 instead of 0.37 m/s, stayed under water and drowned | 0.367 m/s at 30, 60 and 120; surface after 3.8-4.7 s | `FCP_FRAME_SWIM=1` |
| First landing after take-off | with steps shorter than a console frame the flight can end in its first step | flight over a stair edge in `05_mines` 0.02 instead of 0.60 s | 0.68 s | `FCP_FRAME_LANDING=1` |
| Entering water | swim mode starts one step after crossing the water line, so the depth depends on the step length | 0.14-0.28 instead of 0.55-1.1 m below the line; surfaced after 30.3-30.6 instead of 31.4-32.8 s | depth and time in the range of 30 | `FCP_FRAME_DIVE=1` |
| Stick turn ramp | the turn rate ramp is summed per frame | 1 s of full stick: 253.0-253.2 instead of 249.6-249.8 degrees | 249.5-250.3 degrees at 45, 60, 90 and 120 (frames that cross a console frame mix the rates of both) | `FCP_FRAME_TURN=1`; `FCP_TURN_MODEL=floor` |
| Even frame time (camera turns, issue #2) | the frame timer's clock gives whole milliseconds: at 60 frames/s the world moves by 15-18 ms per frame while frames reach the display every 16.7 ms | turning moved the view 4 % more or less from frame to frame (stick 400 deg/s: 6.0-7.2 instead of 6.67 deg per frame; sd 3.7-4.1 %) | sd 0.3 % (6.62-6.73 deg); the sum over many frames stays the clock's, 1 s of full stick 250.1 degrees | `FCP_FRAME_DT_SMOOTH=0` |
| Path following of computer-controlled characters | the look-ahead is one frame's movement | 3-4 cm instead of 13-15 cm | 13-15 cm, as at 30 | `FCP_FRAME_PATH=1` |
| Wheeled vehicles (cars, quads, trucks; the player's and computer-controlled) | the wheel impulses (engine, brakes, suspension, tyre grip) are applied once per physics step; the console's physics runs steps of exactly 0.02 s (50 per second), above 50 frames/s the step is the frame | Humvee (`10_dead_marshes`, full throttle from rest until an obstacle after about 1.8 s): 19.1-19.2 instead of 17.3-17.4 m/s, 20 m in 1.73-1.74 instead of 1.89-1.93 s (120, 144, 240); quad (`06_cliffs`): 3 % slower at 240 (30 m in 3.26 instead of 3.17-3.20 s) | the wheel impulses run at most 50 times per second, over the time since the last one (as the author's earlier *Far Cry Instincts* port): quad as at 30 at 60-240 (+1/+2/+3 s: 8.25-8.51 / 11.84-11.97 / 13.25-13.55 m/s against 7.97-8.21 / 11.73-11.93 / 12.98-13.55; 30 m in 3.17-3.19 against 3.17-3.23 s); Humvee 17.5-17.9 m/s, 20 m in 1.78-1.87 s at 120 and 240, 1.97-1.99 s at 144 (still up to 5 % off, see [Still open](#still-open)) | `FCP_FRAME_VEHICLE=1` |
| Fire rate | the fire test runs once per frame, so the interval is rounded up to whole frames | assault rifle 9.62-9.64 instead of 8.97-9.07 shots/s; after reloading 75 instead of 100 ms per shot | the test runs on the console's 1/30 s grid: 8.97 shots/s at 120, 8.61-9.15 at 60; 100 ms after reloading | `FCP_FRAME_FIRE=1` |

Notes:

- **Even frame time and the other corrections:** the even frame time changes only what the game moves the world and
  the camera by. The fire rate correction (together with the game's own fire test) and the entity timer correction
  (drowning) model the console's whole-millisecond frame clock and therefore read the clock's game time and frame time
  before the evening; with the even values they gave 2-8 % slower fire and 8-10 % more drowning damage at 60 and
  120 frames/s. `FCP_FRAME_DT_RAW_CLOCK=0` lets them read the even values again (for comparison only).
- **Fall damage:** measured as health lost for falls of 2.5, 4, 6, 9 and 12 m (teleport in `m01_mercycreek`): 0 / 0 /
  11.5 / 37.8 / 50.7 at 30, 45, 60, 90 and 120. The earlier estimate (a fixed age of 2.4 frames) was 3 % high for a
  jump at 45 frames/s and gave 9.1 / 36.9 / 54.0 for these falls at 45; it still serves where the step model does not
  apply (`FCP_FALL_SAMPLE_FRAMES=<k>`).
- **45 frames/s:** all corrections were checked at 45 against 30 (PC, two to six runs each). Besides fall damage and
  the turn ramp (corrected), the assault rifle fires 1.7 % faster than the PC at 30 (no frame falls on the 100 ms
  interval; the correction alternates 89 and 111 ms), and entering water ends 0.23 m higher than in the PC's 30 runs,
  inside the range the console gives depending on where its frames fall. Neither is corrected.
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
- **Humvee acceleration.** With the wheel impulse correction the quad accelerates as at 30, the Humvee in
  `10_dead_marshes` not quite: 20 m from rest take 1.78-1.87 s at 120 and 240 frames/s (1.84-1.86 at 60, 1.97-1.99 at
  144) against 1.89-1.93 s at 30, the speed before the obstacle is 1-3 % higher (17.5-17.9 against 17.3-17.4 m/s;
  original 19.1-19.2). The body still takes all physics steps between two impulse steps; FCI saw the same. A variant that
  also gave the impulse step the wheel contacts of the first collected step made the Humvee 5 % slower and the quad
  10 % faster and was dropped. Computer-controlled vehicles keep their track (`21_dunea`, 240 against 30: the same
  path, 0.4 s ahead, the known earlier start of the scripted drive).
- **Reloading** is about 0.05 s shorter at 60 and 120 (one frame of rounding in the reload sequence, not the fire
  test).
- **At 30 itself** the fire interval depends on the jitter of the millisecond clock (5-9 of 30 intervals take four
  instead of three frames). How much a real console jitters is not known.

- **Ladder ends.** At the ladder at (834.4, 807.2) in `06_cliffs`, climbed with the stick held, the player got over
  the top in 1 of 3 runs at 30, 0 of 1 at 120 and 3 of 3 at 60, 144 and 240; in the other runs he climbed to the same
  height (187.3-187.7) and went back down, and at 30 the climb started 0-4 s late. The outcome varies from run to
  run at 30 itself, a frame rate dependence is not shown. A second ladder (856.9, 808.5) was climbed and left the
  same way at 30, 144 and 240 (top at 17.3-17.5 s). Tapped input (FCI: over the edge at 30, back at 60-120) was not
  tested.

Not examined yet: vine ends, slope edges, particles and HUD animations, semi-automatic weapons tapped faster than
their interval, other ways of entering water (cliff, boat).

## Above 120 (unsupported)

`[display] unsupported_uncapped_fps = true` (`FCP_UNSUPPORTED_UNCAPPED_FPS=1`) lets `[display] fps` and `auto` go up to
240, and the F10 menu then lists 144, 165 and 240 marked "unsupported". This is **not supported**: it was audited at
144 and 240 on one PC (below), not played through, and what still differs is listed here. Without the key the settings
stay at 30 and 40-120 as above.

Measured on a PC (Vulkan, 640x360 window, 144.0 and 240.0 frames/s held; one or two runs per rate, against the 30
runs of this page and of the same build):

| Area | 30 | 144 | 240 | Verdict |
|---|---|---|---|---|
| Landing after a jump (sent speed, m/s; raw 4.73 / 4.76) | 4.084-4.096 | 4.087-4.088 | 4.087-4.088 | as 30 |
| Falls 2.5 / 4 / 6 / 9 / 12 m: health lost | 0 / 0 / 11.5 / 37.8 / 50.7 | the same | the same | as 30 |
| Stick turn ramp, 1 s full stick (degrees; original at 240: 253.9) | 249.6-250.1 | 250.20 | 250.16 | as 30 |
| Swimming after the helicopter jump (`13_black_forest`): rise, least breath | 0.367 m/s, 87-88 | 0.367 m/s, 91 | 0.367 m/s, 89 | as 60/120 (surfaces earlier, see above) |
| Entering water: depth of the first swimming step (z) | 13.50-13.54 | 14.05 | 13.85 | inside the console's range |
| First landing on the `05_mines` stairs (flight) | 0.60-0.63 s | 0.674 s | 0.675 s | as 60/120 |
| Player's boat (`03_river`): +3 s / top (m/s) | 5.81-5.83 / 7.05 | 5.82 / 7.05 | 5.83 / 7.05 | as 30 |
| Patrol boat (`01_beach`, median 60-75 s) | 11.71-11.84 m/s | - | 11.90 m/s | as 60/120 |
| Drowning (`01_beach`): damage, death after breath 0 | 7.14-7.19 HP/s, 13.9 s | - | 7.36 HP/s, 13.4 s | as 60 |
| View bob under water (pitch span) | 4.78 degrees | - | 4.78 | as 30 |
| Assault rifle, 29 shots held (`05_mines`) | 8.70-8.79 /s | **8.09 / 9.82 /s** (165: 9.37) | 8.80-8.89 /s | 240 as 30; **144 and 165 off by 7-12 %, either way** |
| Wheeled vehicles: Humvee 20 m / quad 30 m from rest | 1.89-1.93 s / 3.17-3.23 s | 1.97-1.99 s / 3.18 s | 1.81-1.87 s / 3.17-3.18 s | quad as 30, Humvee up to 5 % off |
| Computer-controlled character (`05_mines`): second path, then | from 20.25 s, moves on | - | from 19.83 s, stands still | as 60/90/120 (known) |

**Fire rate at 144 and 165.** The fire correction lets the game's own test decide on a frame within 2.5 ms of a
console frame. That fits rates that are a multiple of 30 (60, 90, 120, 240: the frames lie on the console's grid up to
the clock's jitter). At 144 (frames of 6.9 ms) the frame nearest to the rifle's 100 ms comes after 97 ms, and each
next interval counts from there; a frame 1-2.5 ms before the grid fails the game's test and fires a console frame later. Which of the two wins depends on where the frames fall: 24 of 29 intervals 97 ms (9.82 shots/s) in one run, 22 of 29 at 132 ms (8.09 shots/s) in another, 9.37 at 165, against 8.70-8.79 at 30. Two other rules for
such rates were tried and dropped (always the console model: 10.3 shots/s at 144; the console's grid kept across
shots: 8.1-8.2 at 144, 9.3 at 165, and 45 got worse). Not corrected; at 240 the rifle fires as at 30, and 45 stays as measured before (9.26 shots/s in this build, 9.00-9.25 earlier).

**Fixed assumptions.** One correction assumes at most 240 physics steps per second (the look-ahead of computer-
controlled characters on the first console frame of a walk, and the experimental step edge test); that is why the
option ends at 240. The even frame time ([Corrections](#corrections)) keeps 240 frames/s at 4.17 ms per frame from
the 4/5 ms clock.

Not measured above 120: the mounted machine gun, the reload time, the path release of computer-controlled characters, particles, HUD animations,
the frame rate switch in the F10 menu itself (only the settings file was used), Direct3D 12.

## Method

Runs use the development start (`FCP_START_LEVEL`), scripted input on the level clock (`FCP_INPUT_SCRIPT_CLOCK=level`,
so the script is the same at every frame rate) and the state trace (`FCP_TRACE_STATE`, one CSV line per frame:
position, angles, speed, health, water state, ammunition). `tools\audit\run_series.ps1` runs a series of rates and
repeats and counts other game processes before and after each run. Some runs were made while other processes loaded
the machine; the tables therefore use the measured mean frame time rather than the target. See
[TESTING.md](TESTING.md) for the switches.
