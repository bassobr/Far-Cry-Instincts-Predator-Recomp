// Game clock and per-frame test tools.
//
// Frame = one call of Game_MainLoop (the title's main loop body, once per presented frame). Game time =
// the engine's own accumulated frame time (frame timer +0x28, it stops while the game is paused), read
// after every FrameTimer_Update. Level start = the first frame after a "StartLevel" command was
// dispatched (the level loads inside that dispatch); level time counts game time from there.
//
// Switches (all off by default, read once):
//   FCP_FPS, FCP_FPS_LIMIT, FCP_PRESENT_INTERVAL, FCP_FPS_STATS: see fcp/framerate.h
//   FCP_TRACE_COMMANDS=1       command queue as CSV (logs/commands[.gen<n>].csv): push and dispatch
//   FCP_TRACE_HITCHES=<ms>     frames longer than ms (1 = 50 ms) with what happened in them (with the main
//                              thread's CPU time and time inside imports, fcp/imports.h)
//   FCP_TRACE_FRAMES=1         every frame as CSV (logs/frames[.gen<n>].csv): duration, main thread CPU, imports
//   FCP_GAMETIME_STATS=0       no "clock" line every 5 s (frame rate, game time against wall time)
#pragma once

#include <cstdint>
#include <string>

#include <rex/ppc/context.h>

namespace fcp::gametime {

/** Reads the switches. Called at title launch. */
void Configure();

/** Game_MainLoop entry (main thread, before the original). Limiter, present interval, hitches, state. */
void OnMainLoop(PPCContext& ctx, uint8_t* base);

/** After FrameTimer_Update returned. */
void OnFrameTimer(uint8_t* base);

/** Game_PushCommand entry (any thread). */
void OnPushCommand(uint8_t* base, uint32_t command, uint32_t lr);

/** Game_DispatchCommands entry and exit (main thread). */
void OnDispatchBegin(uint8_t* base, uint32_t command);
void OnDispatchEnd();

/** Flushes the CSV files. */
void Shutdown();

/** Seconds since title launch (host clock). */
double WallSeconds();
/** Frames (Game_MainLoop calls) so far. */
uint64_t Frame();
/** Engine game time in seconds (frame timer accumulation), 0 before the first frame. */
double GameSeconds();
/** Engine frame time of the last frame in seconds. */
double FrameDt();
/** Game time and frame time as the frame timer's whole-ms clock gave them, before FCP_FRAME_DT_SMOOTH evened the
 *  frame time (equal to GameSeconds/FrameDt while the evening is off, idle at 30 or FCP_FRAME_DT_RAW_CLOCK=0). For the
 *  corrections that model the console's frame clock (fire rate, entity timers). Main thread. */
double RawGameSeconds();
double RawFrameDt();

/** While it lives, the frame timer holds the unsmoothed game time (+0x28) and frame time (+0x68), so that a game
 *  function called in it (Weapon_TryFire) and the correction around it see the clock as without the evening; the
 *  evened values go back at its end. Does nothing when both are the same. Main thread, between two frame timer
 *  updates. */
class RawClockScope {
 public:
  explicit RawClockScope(uint8_t* base);
  ~RawClockScope();
  RawClockScope(const RawClockScope&) = delete;
  RawClockScope& operator=(const RawClockScope&) = delete;

 private:
  uint8_t* base_;
  double game_ = 0.0, dt_ = 0.0;
  bool active_ = false;
};
/** True after the first level start of this process. */
bool InLevel();
/** Game time since the level start (0 before). */
double LevelSeconds();
/** Name of the level of the last StartLevel command ("" before). */
std::string LevelName();

}  // namespace fcp::gametime
