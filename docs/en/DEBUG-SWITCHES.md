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
| `FCP_FRAME_BOB=1` | view bob in water summed per frame |
| `FCP_FRAME_WATER=1` | boat water model once per physics step |
| `FCP_FRAME_SWIM=1` | buoyancy and currents per frame |
| `FCP_FRAME_LANDING=1` | first landing after take-off without the console's minimum flight |
| `FCP_FRAME_DIVE=1` | entering water without the console's remaining fall |
| `FCP_FRAME_TURN=1` | stick turn ramp summed per frame |
| `FCP_FRAME_PATH=1` | path following with one frame's look-ahead; `FCP_FRAME_PATH_RELEASE=1` keeps the look-ahead correction but turns off the release from corners |
| `FCP_FRAME_FIRE=1` | fire test once per frame |
| `FCP_FALL_SAMPLE_FRAMES=<k>` | age of the sampled landing speed in frames for the fall correction (default 2.4, measured) |

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

Each of the codegen fixes is checked by the semantic test; see `rexglue/UPSTREAM.md` (changes 9 and 70).

## SDK runtime

Local changes of the frozen SDK copy (`rexglue/UPSTREAM.md`):

| Switch | Effect |
|---|---|
| `FCP_NO_TIMESTAMP_BUNDLE=1` | the kernel's time stamp bundle keeps only the tick count current (upstream); system and interrupt time stay 0 |
| `FCP_AUDIO_UNREGISTER_NOWAIT=1` | unregistering an audio render client does not wait for its running callback (upstream order; crashed in 3 of 4 test runs of the map editor launcher with a widened window) |
| `FCP_NET_IDLE_WAIT_MS=0` | a `select` poll with timeout 0 returns at once every time (upstream); otherwise, after `FCP_NET_IDLE_POLLS` empty polls in a row (default 64), it waits up to 1 ms for readiness. Measured with a failed system link socket: menu 209.6 % against 25.9 % process CPU |
| `FCP_PROFILE_WRITE_TYPE_FROM_DATA=1` | profile settings take their data type from the game's type byte (upstream); the game leaves it uninitialised and a save's title setting was dropped |
| `FCP_DEV_OVERLAYS=1` | registers the SDK's developer overlays (debug F3, console backtick, settings F4, achievements F7), which are off by default |
| `FCP_RELAUNCH_KEEP_WINDOW=0` | the relaunched process gets only the window size; position, maximised and fullscreen state are set later (earlier hand-over) |
| `FCP_GPU_WAIT=0` | the D3D library's GPU wait loops spin instead of sleeping on the command processor's progress; `FCP_GPU_WAIT_TIMEOUT_MS=<ms>` (default 1) bounds one sleep |
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
| `FCP_USER_MIGRATE=0` | no one-time copy of saves from `<exe>\user` into a new user folder |
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
| `FCP_PIPELINE_PREWARM=0` | no pipeline creation ahead of time from the stored pipeline list |
| `FCP_VK_NO_GPL=1` | Vulkan without the graphics pipeline library; `FCP_VK_GPL_ANY_VENDOR=1` also uses it on GPUs other than NVIDIA (only tested there) |
| `FCP_SHADER_PACK_BUILD=0` | no shader pack on the first start; shaders are translated while playing |
| `FCP_SHADER_COMPILE_CACHE=0\|verify\|memory` | cache of the game's own run-time shader compiles: off, checked against a fresh compile, or in memory only (default: on disk, `cache\hlsl`) |
| `FCP_RENDER_VSYNC=0\|1` | host present without / with waiting for the monitor (default: on up to 60 frames/s) |

Diagnostics only: `FCP_RENDER_CONDITIONAL=1` (skip conditionally rendered draws), `FCP_RENDER_SKIP_PS=<hash>,...`
(skip draws with these pixel shaders), `FCP_RENDER_SOLID_PS`/`_VS`/`_COLOR`, `FCP_GPU_DEBUG=1` (Direct3D 12 debug
layer, Vulkan validation), `FCP_GPU_WARP=1` (Direct3D 12 software device), `FCP_SHADER_PACK_STALE_OK=1` (use an
outdated shader pack), `FCP_RENDER_SCALE_MIN_WIDTH=<px>` (width from which a target counts as screen-sized,
default 640).

## PC features back to the console

| Switch | Effect |
|---|---|
| `FCP_RENDER_SCALE=1`, `FCP_RENDER_SHADOW_SCALE=1`, `FCP_RENDER_ANISO=1`, `FCP_RENDER_FOV=1` | the console's resolution, shadow map, filtering and field of view (the defaults) |
| `FCP_RENDER_ASPECT=16:9` | the console's 16:9 picture with bars on wide outputs (the whole Hor+ path off); `FCP_FOV_RESTORE=1` keeps Hor+ but writes the game's camera values back after each camera update (water then covers only the 16:9 middle) |
| `FCP_RENDER_MSAA=console`, `FCP_RENDER_SMAA=0`, `FCP_RENDER_GAMMA=1` | the console's multisampling, no SMAA, the game's gamma ramp (the defaults) |
| `FCP_BUTTON_PROMPTS=xbox` | the game's own button glyphs; `FCP_PROMPTS=0` turns the prompt module off completely |
| `FCP_MENU=0` | no F10 settings menu |
| `FCP_MENU_PAUSE=0` | the open settings menu does not pause the game |
| `FCP_OVERLAY=0` | the SDK's ImGui overlay is not drawn |
| `FCP_KEYBOARD=0`, `FCP_MOUSE=0` | no keyboard, no mouse (the look hook then only runs the original) |
| `FCP_SKIP_INTRO=0`, `FCP_PAUSE_ON_FOCUS_LOSS=0` | intro videos and focus behaviour as on the console (the defaults) |
