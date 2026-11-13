# rexglue/: frozen copy of the ReXGlue SDK

This directory is our own frozen copy of the ReXGlue SDK. It is maintained in this repository from now on;
upstream changes are only taken over as targeted, listed patches.

## Origin

| | |
|---|---|
| Upstream | https://github.com/rexglue/rexglue-sdk |
| Branch / tag | `development`, tag `nightly-20261111-351bc94d` (tip of `development` on 2026-11-12) |
| Commit | `351bc94d8080917509cce83ff23c4a8e881d7dac` |
| Commit date | 2026-11-11 17:05:25 -0500 ("build: set a project up with find_package and rexglue_project") |
| Version | CMake floor 0.11.0; the copy reports `0.11.0-dev.g351bc94d` (set by the including project via `REXGLUE_VERSION_OVERRIDE`, see change 5) |
| Distance | 328 commits after `v0.10.1` (`bd833a2`) |
| License | BSD 3-Clause, `LICENSE` (Copyright (c) 2026 Tom Clay); large parts derive from Xenia (BSD 3-Clause, notices in the files) and XenonRecomp (MIT). See `../THIRD-PARTY-NOTICES.md`. |
| Taken | `git archive 351bc94d -- . ':(exclude)docs' ':(exclude)tools' ':(exclude).github' ':(exclude)scripts' ':(exclude).gitignore' ':(exclude).gitmodules'` |

Not taken: `tools/binutils` (136 MB of prebuilt binutils binaries for the PPC test programs), `docs/`
(documentation site), `scripts/` (lint, IDA and PowerShell helpers; only used when the SDK is the top level
project), `.github/` (CI), `.gitignore`, `.gitmodules`.

The first commit of this directory (`rexglue: unveraenderte Kopie ...`) is the unmodified export; every later
change is a separate commit and is listed below.

## Former git submodules

The submodules are not part of the copy. `tools/get_deps.ps1` fetches them at the commits below into
`FCP_DEPS_DIR` (default `<repo>/third_party/rexglue-deps`, gitignored); the SDK reads that location from
`REXGLUE_DEPS_DIR` (change 1). The pins are the gitlinks of commit `351bc94d`.

| Name | URL | Commit | Needed by our build |
|---|---|---|---|
| cli11 | https://github.com/CLIUtils/CLI11 | `bfffd37e1f804ca4fae1caae106935791696b6a9` | yes (rexglue.exe) |
| libmspack | https://github.com/kyz/libmspack.git | `305907723a4e7ab2018e58040059ffb5e77db837` | yes (LZX) |
| excrypt | https://github.com/emoose/ExCrypt.git | `088094585aee1694cdeef9a9330d29d14aef9203` | yes |
| FFmpeg | https://github.com/wmarti/FFmpeg.git (branch xenia-ffmpeg-canary-full) | `0604b464c7cb4ebc94940cf1f324a3b26b87717c` | yes (XMA) |
| tomlplusplus | https://github.com/marzer/tomlplusplus.git | `30172438cee64926dc41fdd9c11fb3ba5b2ba9de` | yes |
| simde | https://github.com/simd-everywhere/simde.git | `71fd833d9666141edcd1d3c109a80e228303d8d7` | yes |
| xxHash | https://github.com/Cyan4973/xxHash.git | `e626a72bc2321cd320e953a0ccf1584cad60f363` | yes |
| spdlog | https://github.com/gabime/spdlog.git | `79524ddd08a4ec981b7fea76afd08ee05f83755d` | yes |
| imgui | https://github.com/ocornut/imgui.git | `6d910d5487d11ca567b61c7824b0c78c569d62f0` | yes |
| sdl3 | https://github.com/libsdl-org/SDL (branch release-3.4.x) | `147a8ee32dbf9ac02f3794964490687b6bbda1bc` | yes |
| inja | https://github.com/pantor/inja.git | `7d1b4600b68595085a949743331c2e5673f511ea` | yes (codegen templates) |
| tlsf | https://github.com/mattconte/tlsf.git | `deff9ab509341f264addbd3c8ada533678591905` | yes |
| tracy | https://github.com/wolfpld/tracy.git | `05cceee0df3b8d7c6fa87e9638af311dbabc63cb` | only with `FCP_TRACY=ON` (fetched anyway) |
| glslang | https://github.com/KhronosGroup/glslang.git | `f4f1d8a352ca1908943aea2ad8c54b39b4879080` | only with `FCP_XENOS_GPU=ON` (`get_deps.ps1 -Optional`) |
| catch2 | https://github.com/catchorg/Catch2.git | `88abf9bf325c798c33f54f6b9220ef885b267f4f` | only for the SDK unit tests (`-Optional`) |

## Local changes

Every local change carries a comment starting with `FCP` at the changed place.

1. **Dependencies outside the source tree** (`CMakeLists.txt`, `thirdparty/CMakeLists.txt`,
   `src/base/CMakeLists.txt`, `src/codegen/CMakeLists.txt`, `src/rexglue/CMakeLists.txt`,
   `tests/unit/CMakeLists.txt`, `cmake/rexglue_install.cmake`): new variable `REXGLUE_DEPS_DIR` (default
   `thirdparty/`, the upstream layout). Every path into a former submodule goes through it; vendored
   subprojects get an explicit binary directory in `add_subdirectory`.
2. **Xenos host drivers optional** (`CMakeLists.txt`, `thirdparty/CMakeLists.txt`,
   `src/gpu/xenos/CMakeLists.txt`, `cmake/rexglue_export_targets.cmake`): new option
   `REXGLUE_BUILD_XENOS_HOST_DRIVERS` (default ON = upstream). OFF builds neither `rexgpu-xenos-vulkan` nor
   `rexgpu-xenos-d3d12` and drops `find_package(Vulkan)` (Vulkan SDK), VMA and glslang. `rexgpu`,
   `rexgpu-xenos-base` and the null driver `rexgpu-xenos-null` stay. The SDK unit tests need ON.
3. **Output directory overridable** (`CMakeLists.txt`): `REX_OUTPUT_DIR` keeps a value set by the including
   project; upstream always wrote the binaries to `rexglue/out/<platform>` inside the source tree.
4. **Codegen leaves an unchanged manifest alone** (`src/codegen/manifest.cpp`,
   `ManifestConfig::WriteSDKVersionStamp`): upstream rewrote the manifest after every run, after
   `codegen.build.stamp`. The manifest is an input in `codegen.d`, so every build reran codegen (13 s). Now the
   file is only written when the `sdk_version` line changes. Bug fix, candidate for upstream.
5. **Version without git** (`CMakeLists.txt`): `REXGLUE_VERSION_OVERRIDE`, if set, replaces the git based
   version (the copy has no `.git`; without it the version would be `0.11.0.0-dev.unknown`).
6. **Disc image as game root** (`src/filesystem/file_system.cpp` `FileSystem::ConfigureStandardMounts`,
   `src/app/title_app.cpp` `TitleApp::LaunchTitle`): `title.game_root` may name a GDFX disc image file
   (XGD2/XGD3); it is mounted read only with `ProviderType::kDiscImage` under the game partition and linked
   as `game:` and `d:`. Upstream only accepted a host folder (`LaunchTitle` refused a file,
   `ConfigureStandardMounts` always mounted `kHostDirectory`). Folders behave as before. Candidate for
   upstream.
7. **`KeTimeStampBundle` time fields** (`src/kernel/xboxkrnl/xboxkrnl_module.cpp`): the 1 ms timer now also
   keeps `system_time` (guest FILETIME, `Clock::QueryGuestSystemTime`) and `interrupt_time` (100 ns since
   the guest time base) current; upstream left both at 0 and only updated `tick_count`. All three come from
   the same guest clock as `KeQuerySystemTime` and the tick count. `FCP_NO_TIMESTAMP_BUNDLE=1` restores the
   upstream behaviour.
8. **Codegen table `[function_names]`** (`src/codegen/config.h`, `src/codegen/config.cpp`,
   `src/codegen/config_keys.cpp`, `src/codegen/codegen_writer.cpp`): `0xADDRESS = "Name"` renames the
   function that starts at the address (symbol `Name`/`__imp__Name` instead of `sub_<address>`) without
   changing its boundaries or authority. A `[functions]` entry with only `name` would have pinned the
   boundaries (authority `kConfig`, size 0). Applied after analysis, next to the `[shim]` names; an address
   without a function, or a shim, is warned about and skipped. Our build feeds it from
   `config/predator/d3d_symbols.toml` (generated, `cmake/fcp_d3d_names.cmake`) and from
   `config/predator/default.toml`, so hooks in `src/` name functions and carry no addresses. Measured: with
   392 names the codegen registers the same 22,832 functions as without.
9. **Codegen fixes found by the semantic test** (`src/codegen/builder/context.cpp`, `builder_context.h`,
   `floating_point.cpp`, `memory.cpp`, `system.cpp`; checked by `tests/semantics`). Each fix can be
   switched back to the upstream emission at codegen time with the environment variable
   `FCP_CODEGEN_ORIGINAL` (`all` or a comma separated list of the names below), read by `FcpOriginal()`.
   - `fctid_sat`: `fctid`/`fctidz` compared `> f64(LLONG_MAX)`, but that double is 2^63, so exactly 2^63
     converted to 0x8000000000000000 instead of saturating to 0x7FFFFFFFFFFFFFFF.
   - `stcx_so`: `stwcx.`/`stdcx.` left CR0[SO] unchanged; the ISA copies XER[SO] into it.
   - `mfxer_u64`: `(xer.so << 31)` promoted the u8 to int and sign extended into the high word of RT.
   - `fp_single_bits`: `lfs`/`lfsx`/`lfsu`/`lfsux` and `stfs`/`stfsx`/`stfsu`/`stfsux` used the host
     conversions, which quiet a signalling NaN and make `stfs` round a value that is not a single. They now
     follow the bit rules of the ISA (Book I 4.6.2/4.6.3); below the single normal range `stfs` still
     converts on the host. The SDK test `tests/ppc/asm/seq_stack_forward.s` (`test_stack_forward_stfs_lfs`)
     expected the host rounding of 1.1 and now expects the truncated value; it is marked `FCP` there.
10. **Host presenter for the null Xenos driver** (new `include/rex/gpu/host_presenter.h`,
    `src/gpu/xenos/null/null_xenos_graphics_driver.{h,cpp}`, `src/gpu/xenos/null/null_xenos_provider.cpp`):
    new interface `rex::gpu::IHostPresenter` (version 1). At `Connect` the null provider looks it up among the
    connected modules; when one serves it, the null driver hands it the window (`SetSurface`) and the present
    calls of `GraphicsSystem` (`ResizeSurface`, `BeginPresent`, `RenderOverlays`, `EndPresent`,
    `DetachTitle`). Ring buffer, fences, interrupts, vblank and `WaitForFrame` stay with the null driver. This
    is the place where a renderer outside the SDK (our native D3D renderer, `src/renderer`) shows its frames;
    it owns device and swap chain itself, so the Vulkan device-feature requests that King Kong Recompiled
    patches into the SDK provider (its patches 0001-0003) are not needed. Without a module serving the
    interface the null driver presents nothing, as upstream. The app serves it from a code module
    (`App::OnRegisterModules`); `FCP_RENDERER=0` leaves it out.

12. **Title memory for the host presenter** (`include/rex/gpu/host_presenter.h`,
    `src/gpu/xenos/null/null_xenos_graphics_driver.cpp`): `IHostPresenter::AttachTitleMemory(IPhysicalMemory&,
    IPhysicalWriteWatch&)`, an optional virtual (empty by default), is called by the null driver when it
    creates its command processor during `AttachTitle`. The presenter (the project's renderer) watches the
    guest ranges of cached textures with the SDK's write watch instead of hashing them every frame. Without a
    presenter, or one that ignores the call, nothing changes.
20. **`App::OnShutdownBegin`** (`include/rex/app/app.h`, `src/app/app.cpp`): a new virtual hook, called as
    the first step of `App::Shutdown` (after a successful `OnInit`), before the UI stops and before `OnStop`
    unloads the title, while the window still exists. `predator.exe` cancels a pending relaunch there (the
    new process is terminated) when the window is closed during the hand-over (`src/fcp/relaunch.cpp`).
    Default: empty, so upstream behaviour is unchanged.
    Not changed: the SDK's own `RequestRestart`/`platform::Relaunch` (`src/base/relaunch_win.cpp`). It starts
    the new process only after the old one has shut down completely, without launch data, hand-over event,
    first-frame wait or failure report, so the relaunch of `predator.exe` does not use it.

30. **Measurement tap on the mix** (`src/apu/fcp_audio_tap.{h,cpp}` new, `src/apu/audio_system.cpp`,
    `src/apu/CMakeLists.txt`): every mix period hands the summed 6 channel frame (after the streams, before
    master gain, mute and the device down/up mix) and what the mix saw (open streams, frames taken,
    empty open streams, largest queue) to the tap. `FCP_RECORD_AUDIO=<file.wav>` writes it as 48 kHz
    float WAV (WAVE_FORMAT_EXTENSIBLE, mask 0x3F) from an own writer thread, also with `apu.mute`, plus
    `<file.wav>.gaps.tsv` with every period in which an open stream had no frame; a relaunched process
    (`FCP_RELAUNCH_GEN=n`) writes `<file>.gen<n>.wav`. A statistics line every 10 s of audio time
    (`fcp audio: …`: periods, device or timer, frames, underruns, queue, silence, peak, samples above
    1.0, RMS per channel); `FCP_AUDIO_STATS=0` turns it off. `AudioSystem::Start` logs the channel count of
    the opened device (`audio device open: 2 channels, 48000 Hz`). Nothing played changes.
31. **Counters of the XMA decoder and the render clients** (`src/kernel/xma/fcp_xma_stats.h` new,
    `src/kernel/xma/decoder.cpp`, `src/kernel/xma/context.cpp`, `src/kernel/apu.{h,cpp}`): contexts
    created/released/live, kicks, decodes, frames decoded/skipped, FFmpeg errors, stream errors
    (error_status 4, bad packet index, no bits), loops, output blocks, codec opens by rate index and
    channel count; render clients registered/unregistered, callbacks. The audio worker logs them every
    10 s and at shutdown (`fcp xma: …`), registrations and unregistrations once each.
    `FCP_AUDIO_STATS=0` turns the lines off. Only counted.
32. **Unregistering a render client waits for its running callback** (`src/kernel/apu.{h,cpp}`):
    upstream `APU::UnregisterClient` freed the slot and the wrapped callback argument while the
    "Audio Worker" could already have taken the callback and be about to run it (or be inside it). The
    title then tears its voices down under a running callback. Measured with the map editor launcher of
    Predator (the only path seen that unregisters, `XAudioUnregisterRenderDriverClient` 1×) and the test
    switch `FCP_TEST_AUDIO_CALLBACK_DELAY_MS=<ms>` (sleeps before every callback to widen the window,
    tests only): upstream order crashed in 3 of 4 runs (`unhandled title write of 0x000000E0` on the
    Audio Worker in `sub_8275F998`, 0xC0000005). Now the worker marks the slot whose callback runs and
    signals when it returns; `UnregisterClient` (not on the worker itself) waits for it, bounded to
    250 ms with a warning, so a callback that waits for a lock the caller holds cannot deadlock. The
    wrapped argument lives per slot until `APU::Shutdown`, so a late callback never reads freed memory.
    Counters `callbacks_overlapping_unregister` and `unregister_waits` (change 31).
    `FCP_AUDIO_UNREGISTER_NOWAIT=1` restores the upstream order (the per slot argument stays).
33. **FFmpeg as its own DLL** (`thirdparty/CMakeLists.txt`, `src/kernel/CMakeLists.txt`,
    `tests/unit/CMakeLists.txt`, `cmake/rexglue_export_targets.cmake`): new option `REXGLUE_FFMPEG_SHARED`
    (Windows, default ON). `libavutil` and `libavcodec` become object libraries and form one shared library
    `rexffmpeg` (`WINDOWS_EXPORT_ALL_SYMBOLS`, links `bcrypt`); `rexkernel` and the unit tests link
    `REXGLUE_FFMPEG_TARGETS` (`rexffmpeg`, or `libavcodec;libavutil` when OFF); `rexffmpeg` joins the
    install targets. `libavcodec/file_open.c` is left out of the shared build: it only includes
    `libavutil/file_open.c` again and would define `ff_open`, `ff_tempfile`, `av_fopen_utf8` twice in the
    one DLL. FFmpeg sources and configuration are unchanged (fork's `config_windows_x86_64.h`, LGPL, no
    GPL/nonfree parts). Measured: `rexkernelrd.dll` 2,784,256 → 2,209,792 bytes, `rexffmpegrd.dll`
    957,952 bytes with 1,004 exports; `rexkernel` imports 12 FFmpeg functions; the XMA decoder decodes as
    before (21_dunea, 90 s: 67,694 frames, 0 errors). Other platforms keep the static libraries (the hidden
    visibility preset would hide the API in a shared object). `-DREXGLUE_FFMPEG_SHARED=OFF` restores
    upstream. Licence notes in `../THIRD-PARTY-NOTICES.md`.
70. **`[fp_rules]`: PowerPC rules for NaN results and the single rounding of `fmadds`** (new
    `include/rex/ppc/fcp_fp_rules.h`, included at the end of `include/rex/ppc/fpu.h`;
    `src/codegen/config.{h,cpp}`, `src/codegen/config_keys.cpp`, `src/codegen/builder/floating_point.cpp`,
    `src/codegen/builder/vector.cpp`). A new title configuration table
    `[fp_rules]` with three keys, all defaulting to the upstream emission (with the defaults the generated
    game code is byte identical, measured: no generated file changed after the codegen rerun):
    - `nan = "ppc"` (default `"host"`): the 18 scalar arithmetic forms `fadd(s)`, `fsub(s)`, `fmul(s)`,
      `fdiv(s)`, `fmadd(s)`, `fmsub(s)`, `fnmadd(s)`, `fnmsub(s)`, `fsqrt(s)` wrap their host result in
      `Nan1/2/3`: if it is a NaN, an out-of-line path returns the first NaN operand in the order frA, frB,
      frC, quieted, else the default QNaN 0x7FF8000000000000, sign never changed (ISA Book I 4.3.2, 4.6.6;
      single forms keep the high 23 fraction bits). The host gives 0xFFF8… for a new NaN, picks the NaN
      by its own operand order and flips the sign of a propagated NaN in `fnmadd`/`fnmsub`/`fmsub`.
      56,433 sites in the game.
    - `vmx_nan = "ppc"` (default `"host"`): `vaddfp(128)`, `vsubfp(128)`, `vmulfp128` per lane after the
      PEM rule (vA, vB, quieted, else 0x7FC00000; 1,008 sites); `vmaxfp(128)`/`vminfp(128)` with the PEM
      rules for NaN operands, max(+0,-0) = +0, min(+0,-0) = -0 and NJ denormal operands instead of
      `maxps`/`minps` (12 sites).
    - `fmadds_rounding = "single"` (default `"double"`): `fmadds`, `fmsubs`, `fnmadds`, `fnmsubs` round
      once. The fused result rounded to double is only wrong after the second rounding when it lies exactly
      halfway between two singles in round-to-nearest; only then (inline test of the low 29 bits, plus
      results below the single normal range) an out-of-line path decides the direction with the exact sign
      of frA*frC+frB minus that result (error free transforms; exact unless the product leaves the double
      normal range). `fnmadds`/`fnmsubs` now negate after the rounding, as the ISA defines. 13,513 sites.
    Each correction switches back at codegen time with `FCP_CODEGEN_ORIGINAL=fp_nan`, `vmx_nan`,
    `fmadds_single` (or `all`), even when the table enables it. The slow paths count per guest address
    when `FCP_FP_STATS=1` is set at run time (`src/fcp/fp_stats.cpp` writes the counters at shutdown).
    `predator_semtest` builds a third runner with `tests/semantics/fp_rules.toml` (all three on) and
    fails the build when it gets any case wrong that the plain emission gets right; `gen.py` adds a NaN
    matrix of 2,986 cases (`nan:<form>`). Measured: plain 1,563 failures in 47 forms, with `[fp_rules]` 1
    (the ISA-undefined `stfs` case), 0 worse. Cost and in-game counts in the report. The game keeps all
    three keys at their defaults for now (experiment, `config/predator/default.toml`).
80. **Progress counter of the command processor** (`include/rex/gpu_progress.h` new, `src/base/gpu_progress.cpp`
    new, `src/base/CMakeLists.txt`, `src/gpu/xenos/command_processor.cpp`): `rex::GpuProgress`,
    `rex::NotifyGpuProgress`, `rex::WaitGpuProgress(seen, timeout_ms)` in `rexbase` (an atomic counter plus
    `WaitOnAddress`/`WakeByAddressAll`, links `Synchronization`). The command processor bumps it after every
    batch (after the read-pointer write-back), before the first round of every `WAIT_REG_MEM` that does not
    match and when it goes idle. Nothing in the SDK waits on it; the title layer uses it to let the D3D
    library's GPU wait loops sleep instead of spinning (`src/fcp/gpu_wait.cpp`, `FCP_GPU_WAIT=0` keeps the
    spin). Without a waiter the cost is one atomic increment per batch.
81. **Idle wait for `select` polls with timeout 0** (`src/xam/xam_net.cpp` `NetDll_select_entry`): a thread
    whose `select` with a zero timeout found no ready socket `FCP_NET_IDLE_POLLS` times in a row (default
    64) waits up to `FCP_NET_IDLE_WAIT_MS` (default 1) for readiness instead of returning at once; it
    returns as soon as a socket gets ready, and the next ready result resets the count. A `select` on a
    closed socket (`WSAENOTSOCK`) sleeps after the same number of failures before it returns the error: the
    requested timeout, at most `FCP_NET_IDLE_WAIT_MS`. Predator's network thread (entry 0x8239AF78) keeps
    selecting on its system-link socket after `bind` on UDP 1000 failed (a second instance holds the port)
    and then cost a whole host core, about 1.4 million rounds per second (measured: menu 209.6 % -> 25.9 %
    process CPU). With its own port the thread blocks in `select` as it should and nothing changes.
    Results, error codes and fd sets are unchanged; only the timing of idle or failing polls.
    `FCP_NET_IDLE_WAIT_MS=0` restores upstream.
90. **Profile settings: data type from the setting ID** (`src/xam/xam_user.cpp`
    `XamUserWriteProfileSettings`): the setting ID carries the data type in bits 28-31 (`0x63E83FFF` = 6,
    binary). Far Cry Instincts Predator leaves the type byte of its title settings uninitialised; measured
    0, 64, 112 and 130 in one level sweep. A byte outside 0..7 hit the stub branch and the write was
    dropped (warning `unimplemented: XamUserWriteProfileSettings data type 112`), so the 904-byte title
    setting `0x63E83FFF` of the first save after a level start was lost until the next write. Such a byte now
    takes the type from the ID. Measured: dev start `01_beach`, write at 0.7 s with type byte 112 for
    `0x63E83FFF`, no warning, file written. `FCP_PROFILE_WRITE_TYPE_FROM_DATA=1` restores upstream
    (warning comes back).
110. **Window position and maximized state at creation** (`include/rex/ui/ui_system.h`, `include/rex/ui/window.h`,
    `src/ui/ui_system.cpp`, `src/ui/sdl3/sdl3_ui_driver.cpp`): two startup settings, `ui.window_position`
    (`"x,y"`, the client area origin in screen coordinates, empty = as before: centered or `ui.monitor`) and
    `ui.window_maximized` (bool, default false), go into `WindowConfig` (`has_position`, `x`, `y`, `maximized`).
    The SDL3 driver creates the window with `SDL_WINDOW_MAXIMIZED` when asked (not in fullscreen) and calls
    `SDL_SetWindowPosition` after `CenterOnMonitor` and before the fullscreen switch and `SDL_ShowWindow`, so a
    window opens where it belongs instead of moving after it shows. Used by the title's relaunch (the new process
    opens at the old window's place, maximized, or fullscreen on the old monitor; `src/fcp/relaunch.cpp`). Measured:
    moved, maximized and fullscreen windows of generation 0 reappear in generation 1 at the same rectangle, already
    at title launch, no move. With both settings at their defaults nothing changes. The title switch
    `FCP_RELAUNCH_KEEP_WINDOW=0` restores the old hand-over (size only, placement at title launch).

120. **Diagnosis line when an indirect buffer fails** (`src/gpu/xenos/command_processor.cpp`
    `ExecuteIndirectBuffer`): on a failed packet the null driver now also logs the buffer address and length,
    the dword index of the failing packet, the first four dwords of the buffer and of the packet. Error path
    only, no behaviour change. Used to classify the boot-time `ExecutePacketType0 overflow` (one buffer at the
    first `Swap`) and the `WAIT_REG_MEM` failures at shutdown as harmless.

140. **Freestanding compile of the generated code** (`include/rex/ppc/guest_prelude.h`, `include/rex/guest_std/`,
    `resources/templates/codegen/pch_h.inja`, guards `REX_GUEST_FREESTANDING` in `include/rex/assert.h`,
    `chrono.h`, `ppc/image.h`, `ppc/recompiled_code.h`): the codegen PCH header includes the prelude instead of 13
    SDK and 7 standard headers. Hosted builds get the same list as before; with `REX_GUEST_FREESTANDING` only what
    the generated functions use, on own minimal `std` headers (`-nostdlibinc`), so predator_code.dll compiles on a
    PC without Visual Studio or the Windows SDK. `REX_RECOMPILED_CODE()` walks the PE section table inline in that
    mode. Measured: 150 recomp files compiled both ways, 23,385 functions with identical machine code, 36 instances
    (2 templates of `DotProductReference`, the image lambda) differ only in register allocation; semantic test
    41,711 cases with the same single known failure.

141. **Title hooks exported from the executable** (`include/rex/platform.h`): with `REX_TITLE_HOOK_EXPORTS`
    `REX_DETAIL_ENTRY_EXPORT` is `__declspec(dllexport)`, so `REX_HOOK*`, `REX_STUB*`, `REX_DETOUR` in predator.exe
    become exports that predator_code.dll links from the executable's import library. Without the define nothing
    changes.

250. **Write-watch fault counter per host thread** (`src/kernel/memory.cpp`, `include/rex/kernel/heap.h`
    `CopyWatchFaultCounts`): every write-watch fault the address space resolves (`AccessViolationCallback`, the
    physical heap's callbacks fired) counts on its host thread (64 slots, single writer per slot, relaxed). The
    title reads the counts for its diagnostics (`FCP_CPU_STATS`: line `cpu: write-watch faults`). Under Wine each
    such fault is a signal plus a protection change, so the rate tells whether a thread loses time there.
    Measured on the PC (`21_dunea`, 120 frames/s): only the render thread faults, 0.6-0.7 per frame; the main
    thread none. Counting only, no behaviour change.

251. **`select` on a closed socket sleeps the requested timeout** (`src/xam/xam_net.cpp` `NetDll_select_entry`,
    extends change 81): after the idle polls a `select` with a positive timeout on a closed socket now sleeps the
    requested timeout (like a `select` that found nothing), at most `FCP_NET_CLOSED_WAIT_MS` (default 16), before
    it returns `WSAENOTSOCK`; before it slept at most `FCP_NET_IDLE_WAIT_MS` (1 ms). Under Wine/Proton `bind` on
    UDP 1000 fails (ports below 1024), so Predator's network thread (0x8239AF78) selects with a timeout (image 0x823A1158: object +0xC ms x 1000; ~10 ms, PC 93 calls/s) on
    its closed system-link socket for the whole session: measured on the Steam Deck 885 `select` calls/s and
    13.6 % of a core. Measured on the PC with UDP 1000 held by another process (`21_dunea`, 40 s): 38,822 -> 4,026 `select` calls. Zero or missing timeouts keep change 81. `FCP_NET_CLOSED_WAIT_MS=0` restores change 81.

260. **Developer overlays only on request** (`src/ui/ui_system.cpp`): the SDK's standard overlays (debug F3, console
    backtick, settings F4, achievements F7) are registered only with `FCP_DEV_OVERLAYS=1`; otherwise the log says
    they are off. Player builds can no longer open them by accident (decision of the maintainer, 2026-11-13). The
    title's own F10 settings menu is unaffected. `FCP_DEV_OVERLAYS=1` restores upstream.
