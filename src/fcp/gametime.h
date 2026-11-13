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
//   FCP_TRACE_HITCHES=<ms>     frames longer than ms (1 = 50 ms) with what happened in them
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
/** True after the first level start of this process. */
bool InLevel();
/** Game time since the level start (0 before). */
double LevelSeconds();
/** Name of the level of the last StartLevel command ("" before). */
std::string LevelName();

}  // namespace fcp::gametime
