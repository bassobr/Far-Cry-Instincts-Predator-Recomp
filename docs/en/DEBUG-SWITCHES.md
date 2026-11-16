# Switches back to the original

Every place where the port deviates from the original game, from the SDK's behaviour or from an earlier state of
the port has an environment switch that turns the change off. They exist for comparison and for finding the cause
of a problem; none of them is needed for play. The tables list only switches that exist in the code. Switches for
tests, traces and dumps are in [TESTING.md](TESTING.md), settings in [CONFIGURATION.md](CONFIGURATION.md).

A flag counts as set for any value other than empty, `0`, `false` or `off`. All switches are read once at start.

## Game logic at high frame rates

All of these corrections are inactive at 30 frames/s. Details and measurements: [FRAMERATE.md](FRAMERATE.md).

| Switch | Effect |
|---|---|
| `FCP_FPS=30` | the console's frame rate (the default); every frame rate correction is then inactive |
| `FCP_FRAME_FALL=1` | landing speed for fall damage as in the original (taken frames before the impact) |
| `FCP_FRAME_TIMERS=1` | `_frametime` of script timers as in the original (drowning damage per frame) |
| `FCP_FRAME_TIMER_SLACK_MS=0` | a repeating script timer's measured interval only counts from its period on (before: a 50 ms timer measured at 49 ms on the whole-millisecond clock took the fallback of whole frames, 64 ms at 60 frames/s: 7.3-7.7 instead of 7.14 HP/s drowning damage). Default 3 (ms below the period) |
| `FCP_FRAME_BOB=1` | view bob in water summed per frame |
| `FCP_FRAME_WATER=1` | boat water model once per physics step |
| `FCP_FRAME_SWIM=1` | buoyancy and currents per frame |
| `FCP_FRAME_LANDING=1` | first landing after take-off without the console's minimum flight |
| `FCP_FRAME_DIVE=1` | entering water without the console's remaining fall |
| `FCP_FRAME_TURN=1` | stick turn ramp summed per frame |
| `FCP_FRAME_PATH=1` | path following with one frame's look-ahead; `FCP_FRAME_PATH_RELEASE=1` keeps the look-ahead correction but turns off the release from corners |
| `FCP_FRAME_FIRE=1` | fire test once per frame |
| `FCP_FRAME_VEHICLE=1` | wheel impulses of cars, quads and trucks once per physics step (above 50 frames/s more often than the console's 50 per second) |
| `FCP_FALL_MODEL=linear` | fall correction without the console step model: only the older fixed-age estimate below (it was 3 % high at 45 frames/s) |
| `FCP_FALL_SAMPLE_FRAMES=<k>` | age of the sampled landing speed in frames for the older estimate (default 2.4; used where the step model does not apply) |
| `FCP_TURN_MODEL=floor` | stick turn ramp: a frame that spans two console frames takes the rate of the first only (0.4 % less turn at 45 frames/s) |
| `FCP_FRAME_DT_SMOOTH=0` | the game's frame time in whole milliseconds (15-18 ms at 60 frames/s, the view turned 4 % more or less from frame to frame: issue #2); by default above 30 frames/s the frame time handed to the game is the target frame time plus a tenth of the time still owed, the sum stays the clock's. The fire rate correction (with the game's own fire test) and the entity timer correction (drowning) keep using the clock's whole-millisecond game time and frame time |
| `FCP_FRAME_DT_RAW_CLOCK=0` | the fire rate and entity timer corrections read the even frame time too (as in the first version of the even frame time: 2-8 % slower fire and 8-10 % more drowning damage at 60 and 120 frames/s) |

Experiments, off by default (the value turns them **on**): `FCP_FRAME_EDGE=0` (step edge test),
`FCP_FRAME_TIMER_TICK=0` (entity timer ticks on console frames), `FCP_AI_CONSOLE_TICK=1` (thinking rate of
computer-controlled characters at the console's rate).

## Generated code

| Switch | When | Effect |
|---|---|---|
| `FCP_CODEGEN_ORIGINAL=<names>\|all` | code generation | emits the SDK's original code for the listed fixes: `fctid_sat` (`fctid`/`fctidz` at 2^63), `stcx_so` (CR0[SO] of `stwcx.`/`stdcx.`), `mfxer_u64` (`mfxer` sign extension), `fp_single_bits` (`lfs`/`stfs` bit rules), `fp_nan` (PowerPC NaN rules of scalar arithmetic), `vmx_nan` (NaN rules of vector arithmetic, `vmaxfp`/`vminfp`), `fmadds_single` (single rounding of `fmadds`). Read by the code generator; verified for the development build |
| `[fp_rules]` in `config/predator/default.toml` | code generation | `nan = "host"`, `vmx_nan = "host"`, `fmadds_rounding = "double"` give the SDK's emission |
| `[vector_math]` in `config/predator/default.toml` | code generation | removing the block gives the SDK's `fast` approximations |
| CMake `-DFCP_MARCH=x86-64-v2` | build | the earlier ISA level (no AVX2) |
| CMake `-DFCP_GAME_CODE_DLL=OFF` | build | game code linked into `predator.exe`, no first-start build |
| `FCP_GAME_CODE_LTO=0\|1` | first start / game code build | ThinLTO of the game code; default on, on the Steam Deck off (no measurable gain there, shorter build) |
| `FCP_GAME_CODE_CAPTURE_USES=<n>` | first start / game code build (with `FCP_GAME_CODE_REBUILD=1` for an existing DLL) | LLVM's capture tracking limit for the game code (default 10000; `1000000` = the earlier default, `0` = LLVM's default of 100, about 3 % more main thread time) |

Each of the codegen fixes is checked by the semantic test; see `rexglue/UPSTREAM.md` (changes 9 and 70).

## SDK runtime

Local changes of the frozen SDK copy (`rexglue/UPSTREAM.md`):

| Switch | Effect |
|---|---|
| `FCP_NO_TIMESTAMP_BUNDLE=1` | the kernel's time stamp bundle keeps only the tick count current (upstream); system and interrupt time stay 0 |
| `FCP_AUDIO_UNREGISTER_NOWAIT=1` | unregistering an audio render client does not wait for its running callback (upstream order; crashed in 3 of 4 test runs of the map editor launcher with a widened window) |
| `FCP_NET_IDLE_WAIT_MS=0` | a `select` poll with timeout 0 returns at once every time (upstream); otherwise, after `FCP_NET_IDLE_POLLS` empty polls in a row (default 64), it waits up to 1 ms for readiness. Measured with a failed system link socket: menu 209.6 % against 25.9 % process CPU |
| `FCP_PROFILE_WRITE_TYPE_FROM_DATA=1` | profile settings take their data type from the game's type byte (upstream); the game leaves it uninitialised and a save's title setting was dropped |
| `FCP_PROFILE_TITLE_TYPE_FROM_ID=0` | the game's own profile settings no longer always take their type from the setting ID; a type byte of 255 or 1..5/7 drops the save again |
| `FCP_FIND_STAR_DOT_STRICT=1` | a file pattern like `0*.*` needs a literal dot again; "Load a Map" and the multiplayer map list stay empty |
| `FCP_DEV_OVERLAYS=1` | registers the SDK's developer overlays (debug F3, console backtick, settings F4, achievements F7), which are off by default |
| `FCP_RELAUNCH_KEEP_WINDOW=0` | the relaunched process gets only the window size; position, maximised and fullscreen state are set later (earlier hand-over) |
| `FCP_GPU_WAIT=0` | the D3D library's GPU wait loops spin instead of sleeping on the command processor's progress; `FCP_GPU_WAIT_TIMEOUT_MS=<ms>` (default 1) bounds one sleep |
| `REX_HID_SDL_SYNC=1` | SDL pads as upstream: `XamInputGetState`/`GetCapabilities`/`SetState` call SDL on the game's thread and vibration is written synchronously (on a Steam Deck 2-18 ms in a few percent of the frames, rarely up to 200 ms); otherwise state and capabilities are cached and vibration is sent from the UI thread |
| CMake `-DREXGLUE_FFMPEG_SHARED=OFF` | FFmpeg linked statically into the kernel library instead of `rexffmpeg.dll` |

## Title layer

| Switch | Effect |
|---|---|
| `FCP_STARTUP_SYS_UI=1` | the SDK's start-up "system UI on" notification reaches the game unchanged (a development start or level restart then begins in the pause menu) |
| `FCP_TITLE_EXIT_CRT=1` | after the game quits its main loop the original C runtime exit runs (it crashed in a heap free; now the title ends through the SDK) |
| `FCP_NO_RELAUNCH=1` | a relaunch request of the game only ends the title (SDK behaviour); no new process for the next level |
| `FCP_RELAUNCH_TIMEOUT=<s>` | time limit for the relaunched process to show its first frame (default 120) |
| `FCP_DASHBOARD_PROMPT=1` | a request to return to the dashboard shows the SDK's dialog instead of ending cleanly |
| `FCP_GUEST_READABLE_VQ=1\|verify` | guest memory checks use `VirtualQuery` every time (earlier, slow) instead of the SDK's page table; `verify` does both and logs differences |
| `FCP_GUEST_STRING_UNGUARDED=1` | diagnostic reads of guest strings without the fault guard |
| `FCP_IMPORT_SHARED_COUNT=1` | one shared atomic counter for every import call (earlier, slower) |
| `FCP_IDLE_PAD=1` | the SDK's idle pad: player 1 is always connected even without any controller, keyboard or mouse |
| `FCP_PAD_BACKGROUND=1` | controller events also without window focus |
| `FCP_PAD_EXTRA=0` | paddles, misc and touchpad folded by the SDK as before (paddles on A/B/X/Y), not bindable |
| `FCP_PAD_REMAP=0` | no controller remapping (`[pad] pad_bind_*` ignored), extra buttons as with `FCP_PAD_EXTRA=0` |
| `FCP_USER_MIGRATE=0` | no one-time copy of saves from `<exe>\user` into a new user folder |
| `FCP_SAVE_GUARD=0` | save protection off (no atomic replacement, backups, repair or wait at the end; the start check is still logged) |
| `FCP_SAVE_ATOMIC=0` | SDK change 330 off: a replaced save is deleted first and written in place (upstream) |
| `FCP_IMAGE_SEARCH=0` | no search for the disc image when `[game] image` is empty or does not fit |
| `FCP_TOML_LITERAL=0` | a value with a backslash (the path the image search found, a path set in the F10 menu) goes into `predator.toml` as `"..."` as before, not as the literal string `'...'` (a Windows path in `"..."` is no valid TOML; the game reads both) |
| `FCP_TEST_SAVE_END=<n>:<kill\|close\|menu\|restart>[:<ms>]` | test: end the program `<ms>` after the n-th save of the process started (kill = as a crash, close = window closed, menu = F10 menu opened, then Exit Game, restart = F10 Apply); `FCP_TEST_SAVE_END_GENS` selects relaunch generations |
| `FCP_LOG_DEBUGGER=1` | SDK log output also to an attached debugger (off in this port) |
| `FCP_GPU_DRIVER=<name>` | another SDK GPU driver instead of the null driver (not used by the port) |

## Renderer

| Switch | Effect |
|---|---|
| `FCP_RENDERER=0` | no native renderer: the D3D hooks run only the originals, nothing is presented |
| `FCP_D3D_HOOK_<Name>=0` | this D3D hook passes straight to the original, without counting or logging |
| `FCP_D3D_CHECK_READABLE=1` | `VirtualQuery` checks of guest pointers in the D3D hooks (earlier; cost most of the render thread's time) |
| `FCP_RENDER_TILING_CLEAR=0` | the clear inside `BeginTiling` is not forwarded (earlier state) |
| `FCP_RENDER_DEPTH_RESOLVE=0` | depth resolves are skipped |
| `FCP_RENDER_QUERY=original\|visible` | occlusion queries answer with the null driver's value (`original`) or a fixed "visible" value (`FCP_RENDER_QUERY_SAMPLES`) instead of host queries |
| `FCP_RENDER_LOCK_READBACK=0\|sync` | resolve targets are not written back to guest memory when the CPU locks them (`0`), or written back synchronously (`sync`) |
| `FCP_RENDER_TEXTURE_WATCH=0`, `FCP_RENDER_BUFFER_WATCH=0` | guest textures and buffers are hashed instead of watched for writes |
| `FCP_RENDER_TEXTURE_CHECK=0` | guest textures are read only once |
| `FCP_RENDER_ELEMENT_ENDIAN=0` | vertex data endianness from the buffer instead of from the declaration element |
| `FCP_RENDER_PRESENT_UNLOCKED=0` | present waits for the GPU while holding the renderer lock (earlier); `FCP_RENDER_FRAMES_AHEAD=<0..8>` (default 2) bounds the swaps in flight otherwise |
| `FCP_RENDER_FAST_CONSTANTS=0` | shader constants converted and compared per draw (earlier) |
| `FCP_RENDER_INDEX_CACHE=0`, `FCP_RENDER_RING_BATCH=0` | indices and ring copies per draw (earlier) |
| `FCP_RENDER_CB_VERSIONS=<n>` | versions per volatile constant buffer under Vulkan (default 8192) |
| `FCP_RENDER_SLOT_RECYCLE=0` | textures and vertex buffers keep their bindless slot for good (earlier); by default entries not bound for a while are evicted when a table fills up and their slots are reused, so that a long session in one level does not run out of slots (issue #1: missing or flat, untextured objects) |
| `FCP_RENDER_ALLOC_EVICT=0` | no eviction when device allocations run short (issue #1); by default a texture or buffer the driver refuses is logged (`refused by the device`), tried again later, and entries not bound for a while (textures, vertex and index buffers) are evicted after a refusal or once the estimated allocation count reaches 3/4 of the driver's limit |
| `FCP_RENDER_VK_TEXTURE_POOL=0\|1` | Vulkan: one memory allocation per guest texture (`0`) or textures in shared 64 MiB blocks (`1`); default: blocks when the driver allows at most 65536 allocations (`[vulkan] memory:` line) |
| `FCP_PIPELINE_PREWARM=0` | no pipeline creation ahead of time from the stored pipeline list |
| `FCP_RENDER_PIPELINE_SHIPPED=0` | without the shipped pipeline list (`pipelines.fcpl` next to the EXE); `=<file>` uses another list |
| `FCP_RENDER_PREWARM_LOW_PRIORITY=0` | prewarm jobs at normal thread priority (default: below normal) |
| `FCP_PIPELINE_COLLECT=<folder>` | every pipeline record a draw uses goes to `<folder>\<group>.bin` (`menu` or the level name), input of `tools\pipelines_build.ps1` |
| `FCP_RENDER_PIPELINE_ASYNC=0` | missing pipelines are created on the game's render thread one after another (earlier); by default they are made on worker threads while the rest of the command list is recorded and replayed in order (same image); `FCP_RENDER_PIPELINE_THREADS=<n>` sets the worker count (default: CPUs of the process minus 2, 1..8) |
| `FCP_RENDER_PIPELINE_MAX_WAIT_MS=<ms>` | **changes the image**: draws whose pipeline is not ready after this wait are left out of that frame (pop-in instead of a hitch); off by default |
| `FCP_RENDER_SHADER_ASYNC=0` | shaders missing from pack and cache are translated in the game's create call (earlier) instead of on worker threads |
| `FCP_RENDER_CONST_SPAN=0` | the whole 4 KB constant block is converted on every change (earlier) instead of the changed span |
| `FCP_RENDER_MSAA_RESOLVE_ONCE=0` | an MSAA target is resolved again for every resolve rectangle, even when unchanged (earlier) |
| `FCP_RENDER_DEFER_ALL=1` | diagnostics: every command list goes through the deferred (recorded and replayed) list |
| `FCP_VK_NO_GPL=1` | Vulkan without the graphics pipeline library; `FCP_VK_GPL_ANY_VENDOR=1` also uses it on GPUs other than NVIDIA (only tested there) |
| `FCP_SHADER_PACK_BUILD=0` | no shader pack on the first start; shaders are translated while playing |
| `FCP_SHADER_COMPILE_CACHE=0\|verify\|memory` | cache of the game's own run-time shader compiles: off, checked against a fresh compile, or in memory only (default: on disk, `cache\hlsl`) |
| `FCP_RENDER_VSYNC=0\|1` | host present without / with waiting for the monitor (default: on up to 60 frames/s) |
| `FCP_FPS_PACING=game` | the game's own pacing (vblank wait / host limiter) also where `display` is the default (Steam Deck); see [FRAMERATE.md](FRAMERATE.md#display-pacing) |
| `FCP_PRESENT_WAIT=0` | Vulkan without `VK_KHR_present_id`/`present_wait`: no display timing, no display pacing, `presentstat:` with present intervals only |
| `FCP_PRESENT_STATS=<s>` | `presentstat:` line every s seconds (default: `FCP_FPS_STATS`, 10; 0 = off) |
| `FCP_PACE_HOLD=<f>` | display pacing: present released f refreshes after the (n-1)-th one (default 0; 0.3 left 9-15 %, 0.6 70 % of the frames one refresh longer on the Deck, measured) |
| `FCP_PACE_QUEUE=<1..3>`, `FCP_PACE_EARLY=<ms>`, `FCP_PACE_SPIN=<ms>`, `FCP_PACE_LIMIT=<f>`, `FCP_PACE_SINGLE=1` | display pacing experiments: swaps allowed ahead of the display (2 drops frames), release the game earlier, poll instead of sleep, host limiter at f x the display frame period, pace also at one refresh per frame; none improved the measured cadence |
| `FCP_PACE_TRACE=1` | a `pace late:` line (time line of swap, present, display) for each frame that stayed too long |
| `FCP_FPS_AUTO_HZ=<hz>` | test: `fps = auto` takes this as the monitor's refresh rate |
| `FCP_TEST_DPI=<percent>[,<t>:<percent>...]` | test: stands for the monitor's display scale (50-400 %) from the start, and at t seconds after the title launch as if the window had been moved to a monitor with that scale (window size, F10 menu); sets `REX_TEST_CONTENT_SCALE=<factor>`, which the SDK's ImGui scale reads at its next resize or DPI event. In fullscreen a timed change reaches only the window logic, not the menu |

Diagnostics only: `FCP_RENDER_CONDITIONAL=1` (skip conditionally rendered draws), `FCP_RENDER_SKIP_PS=<hash>,...`
(skip draws with these pixel shaders), `FCP_RENDER_SOLID_PS`/`_VS`/`_COLOR`, `FCP_GPU_DEBUG=1` (Direct3D 12 debug
layer, Vulkan validation), `FCP_GPU_WARP=1` (Direct3D 12 software device), `FCP_SHADER_PACK_STALE_OK=1` (use an
outdated shader pack), `FCP_RENDER_SCALE_MIN_WIDTH=<px>` (width from which a target counts as screen-sized,
default 640), `FCP_RENDER_TABLE_SLOTS=<2D>,<3D>,<cube>,<array>,<samplers>,<buffers>` (capacities of the bindless
tables, 0 = default 4096,64,256,64,256,4096; small values reproduce a full table quickly). The renderer statistics
line `renderer: tables ...` shows slots in use per table, requests refused because a table was full, and the
number of draws with the guest's alpha-to-mask bit (`RB_COLORCONTROL` bit 4, not emulated; the first ones are logged)
and the number of texture fetches at the base address of a resolve target with a different size (the resolve target
is bound instead of the guest texture; the first ones are logged). `renderer: allocations: ...` shows the estimated
device allocations against the driver's limit and the refusals; `renderer: resolves per target` lists each resolve
target with its resolves and binds in the interval. Allocation diagnostics: `FCP_RENDER_TEST_ALLOCS=<n>` (n extra
allocations held from the start), `FCP_RENDER_ALLOC_LIMIT=<n>` (evict as if the driver allowed n),
`FCP_RENDER_TEST_ALLOC_FAIL=<n>` (guest textures and buffers are refused while the estimate is at least n, like a
full device). `FCP_TRACE_CAMERA_DUMP=<n>`: the first n updates of screen cameras as raw words (the view camera holds
position at +0x04 and angles in degrees at +0x10).

## PC features back to the console

| Switch | Effect |
|---|---|
| `FCP_RENDER_SCALE=1`, `FCP_RENDER_SHADOW_SCALE=1`, `FCP_RENDER_ANISO=1`, `FCP_RENDER_FOV=1` | the console's resolution, shadow map, filtering and field of view (the defaults) |
| `FCP_RENDER_RESOLUTION=scale` | the render resolution from `FCP_RENDER_SCALE` (the default; `native` and `WxH` replace it) |
| `FCP_RENDER_VERTPLUS=0` | outputs narrower than 16:9 (16:10, 4:3) show the 16:9 picture with bars as before (Vert+ off, Hor+ stays) |
| `FCP_RENDER_ASPECT=16:9` | the console's 16:9 picture with bars on wide and narrow outputs (the whole Hor+ / Vert+ path off); `FCP_FOV_RESTORE=1` keeps Hor+ but writes the game's camera values back after each camera update (water then covers only the 16:9 middle) |
| `FCP_RENDER_HUD_ASPECT=16:9` | the whole HUD in the 16:9 band on wide and narrow outputs (the default; `auto` or a shape moves the edge elements outwards, `[graphics] hud_aspect`) |
| `FCP_RENDER_SCREEN_MASK=0` | binocular and scope masks on wide and narrow outputs classified as before (frame and bars stretched over the whole picture, lenses and circle in the band, the world visible beside them) |
| `FCP_RENDER_SPLIT_VIEW=0` | split screen on wide and narrow outputs as before: every player's HUD narrowed about the centre of the whole screen, `hud_aspect` moves it towards the screen's edges, and masks are recognised only over the whole screen |
| `FCP_LOD_DISTANCE=1`, `FCP_VIEW_DISTANCE=1` | the engine's level of detail variables as registered (the defaults; nothing is written then). `FCP_TRACE_LOD=1` logs the three variables every 5 s and whether anything else changed them |
| `FCP_SHADOW_DISTANCE=1` | the console's shadow radius (the default; the hook then writes nothing). `FCP_SHADOW_DISTANCE_FLIP=<s>` (test) alternates the factor between 1 and the set value every s seconds, the map keeping the larger size |
| `FCP_RENDER_SHADOW_FILTER=console` | the game's own 2x2 shadow filter (the default; the translated shaders then never take the soft filter's branch) |
| `FCP_RENDER_AO=off` | no ambient occlusion (the default; no pass is made, nothing is created, the camera hook passes nothing on). Diagnostics with it on: `FCP_AO_DEBUG=1` shows the occlusion instead of the scene (white = none), `2` the reconstructed view depth (red to 2 m, green to 20 m, blue to 200 m), `3` the occlusion on the right half only; `FCP_AO_TIMING=1` logs the GPU time of the pass (average of 300); `FCP_TRACE_AO=1` logs every insertion point and every reason it was left out; `FCP_RENDER_AO_RESOLUTION=full\|half` (default: half above 720 lines), `FCP_RENDER_AO_STRENGTH=<0..4>` (exponent, default 1) |
| `FCP_RENDER_MSAA=console`, `FCP_RENDER_SMAA=0`, `FCP_RENDER_GAMMA=1` | the console's multisampling, no SMAA, the game's gamma ramp (the defaults) |
| `FCP_TEXTURE_DUMP=0`, `FCP_TEXTURE_REPLACE=0` | no texture dumps, no replacement images (the defaults; no texture pack code runs). `FCP_TEXTURE_DIR=<folder>` moves `dump\` and `replace\` there (tests), `FCP_TEXTURE_UPLOAD_MB=<n>` sets the replacement uploads per frame (default 16, the rest follows in the next frames) |
| `FCP_HDR=off` | SDR output as before (the default; no HDR10 swapchain, no HDR10 pass, the SDR path draws straight into the swapchain). Diagnostics with HDR10 on: `FCP_HDR_TEST_PATTERN=1` shows eight bars of known SDR values (white, 75/50/25 % grey, black, red, green, blue) through the HDR10 mapping instead of the picture; `FCP_HDR_GAMMA=<1..3>` the SDR gamma of the mapping (default 2.2); `FCP_DUMP_PRESENT=1` with `FCP_DUMP_FRAMES`/`FCP_DUMP_FRAME_AT` writes per dump the SDR picture (`<name>.png`), the presented HDR10 image as raw words (`<name>_hdr10.raw`) and the parameters (`<name>_hdr10.txt`); `python tools\hdr_check.py [--pfm] <folder>` decodes them (PQ to nits, paper white, greys, back to SDR, Delta E ITP; `--pfm` also writes linear light as `.pfm` and a preview `.png`). The log line `renderer: display HDR ...` shows what Windows reports for the monitor |
| `FCP_BUTTON_PROMPTS=xbox` | the game's own button glyphs; `FCP_PROMPTS=0` turns the prompt module off completely |
| `FCP_MENU=0` | no F10 settings menu |
| `FCP_MENU_PAUSE=0` | the open settings menu does not pause the game |
| `FCP_MENU_DEFAULTS=0` | no "Defaults" button and no default marks in the settings menu |
| `FCP_MENU_SHORT_MARK=0` | a closed choice field always shows its full text with the default mark ("- Steam Deck OLED default"), also when it does not fit; otherwise it then shows "- default", drops the mark or ends in "..." |
| `FCP_MENU_WRAP_HINTS=0` | the grey hint lines of the Controller page stay on one line (cut at a narrow window) instead of wrapping |
| `FCP_FPS_LIVE=0` | frame rate, VSync and frame pacing changed in the settings menu take effect at the next start (as before), not at once |
| `FCP_F11=0` | F11 does not switch window/fullscreen (Alt+Enter still does) |
| `FCP_DPI_EARLY=0` | the process declares its DPI awareness only when SDL starts its video (as before); by default it declares per-monitor v2 as its first step, before the first start's progress window |
| `FCP_DPI_WINDOW=0` | the built-in window is 1280x720 pixels at every display scale, and a window moved to a monitor with another scale keeps its size in pixels (as before); by default the built-in size is meant at 100 % and the window keeps its size on the screen |
| `REX_IMGUI_DPI_RASTER=0` | the F10 menu's text is rasterized at the point size and stretched to the display scale (as before, blurry at 125 % and more) instead of at the window's pixel density |
| `FCP_RENDER_RECREATE_LOST=0` | Vulkan: a swapchain the driver reports out of date is only made anew on a resize event (as before), not at once at the window's size |
| `FCP_EXCLUSIVE_BACKGROUND_MINIMIZE=0` | an exclusive fullscreen window that never got the focus (another program was clicked while the game loaded) keeps the video mode behind that program (as before); by default it is minimized after 1.5 s and the desktop gets its mode back |
| `FCP_EXCLUSIVE_MODE=desktop` | exclusive fullscreen in the monitor's desktop mode (the default of `[display] exclusive_mode`) |
| `FCP_RENDER_FRAMES_AHEAD=2` | two game frames ahead on the GPU, as before (the default of `[display] frame_queue`; the switch takes 0 = no waiting to 8) |
| `FCP_OVERLAY=0` | the SDK's ImGui overlay is not drawn |
| `FCP_KEYBOARD=0`, `FCP_MOUSE=0` | no keyboard, no mouse (the look hook then only runs the original) |
| `FCP_SKIP_INTRO=0`, `FCP_PAUSE_ON_FOCUS_LOSS=0` | intro videos and focus behaviour as on the console (the defaults) |
| `FCP_RENDER_COLOUR_BLIND=off` | the final blit as before (the default) |
| `FCP_SPEECH_VOLUME=100` | speech at the console's level (the default; the XMA decoder's output stays bit for bit as before); `FCP_SPEECH_TRACK=0` also stops following the sound archive reads |
| `FCP_AUDIO_LIMITER=0` | no output limiter: the downmix is clamped at full scale as before, without the 63-sample delay (`apu.limiter`) |

Measurement switches of the audio path: `FCP_RECORD_AUDIO_OUT=<file.wav>` records the output after the limiter
(device channels, before mute; with the limiter on), next to `FCP_RECORD_AUDIO` (the 6 channel mix before gain and
downmix); `tools\audio\limiter_check.py <mix.wav> <out.wav>` compares both. The log line `fcp limiter:` (every 10 s
of audio and a total) gives peaks and samples above full scale before and after the limiter; the second `fcp xma`
line counts decoded frames by their source (unknown/sound/speech) with their output level. `FCP_TRACE_SPEECH=1` logs
every tracked read of the sound archive with its memory address.
