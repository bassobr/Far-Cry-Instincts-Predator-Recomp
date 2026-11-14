// Frame rate switch and frame time statistics.
//
// How the original paces (image, measured): the render thread calls the renderer method 0x823696A0 once
// per frame; it reads r_vsynch (fcp::addr::r_vsynch, start value 2) and sets the device present interval
// (1 -> ONE, 2 -> TWO, other -> IMMEDIATE). SynchronizeToPresentationInterval then writes a GPU wait for
// that many vblanks into the ring, the null driver waits for the 60 Hz vblank counter, and the CPU stalls
// on ring space. Writing the device field from another thread does not help: the render thread
// overwrites it every frame. The switch therefore sets r_vsynch, the value the game itself reads.
//
// Switches (read once):
//   FCP_FPS=30|60|120|unlimited|<n>
//       30 (default) = original (r_vsynch untouched = 2: two vblanks of 60 Hz)
//       60  = r_vsynch 1 (one vblank)
//       120 = r_vsynch 0 (immediate, no vblank wait) plus a host limiter: at least 1/120 s between frames
//       <n> = immediate plus limiter at n (only for audits; values over 120 are not verified)
//       unlimited = immediate, no limiter (only on request)
//   FCP_FPS_PACING=limiter     60 over immediate plus the host limiter instead of one vblank (diagnosis)
//   FCP_FPS_PACING=display     pace on the display's own refresh (renderer/present_pacer.h): every frame stays the
//                              same number of refreshes on the display (45 on 90 Hz: 2, 30: 3, 90: 1). While the
//                              renderer paces: r_vsynch 0, no host limiter, host present with vsync (FIFO). Until the
//                              refresh is measured, without display timing (D3D12) or when the target does not divide
//                              the refresh rate (60 on 90 Hz), the game paces itself as without the switch.
//   FCP_FPS_PACING=vblank|limiter|original  the game's own pacing (default; vblank = as before)
//   FCP_FPS_LIMIT=<n>         older name of FCP_FPS (0 = unlimited); FCP_FPS wins
//   FCP_PRESENT_INTERVAL=1|2|immediate  r_vsynch directly (diagnosis), wins over FCP_FPS
//   FCP_FPS_STATS=<s>          frame time line every s seconds (default 10, 0 = off): mean, p50, p99,
//                              max, 1 % low, game time against wall time
#pragma once

#include <cstdint>

namespace fcp::framerate {

/** Reads the switches. Called at title launch (before the first frame). */
void Configure();

/** Game_MainLoop entry (main thread): limiter, r_vsynch, frame time statistics. */
void OnMainLoop(uint8_t* base, double game_seconds);

/** Final statistics. */
void Shutdown();

/** Target frame rate (0 = unlimited) as configured; 30 when nothing is set. */
int TargetFps();

/** True when the host presenter should not wait for the monitor (frame rates over 60, not with display pacing). */
bool HostPresentImmediate();

/** Display pacing target for the renderer (FCP_FPS_PACING=display): the frame rate, -1 for unlimited (every
 *  refresh), 0 when display pacing is off. Reads the switches itself (the renderer starts before Configure). */
double DisplayPacingTarget();

}  // namespace fcp::framerate
