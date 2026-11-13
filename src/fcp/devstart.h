// Developer start path and test scenarios.
//
// FCP_START_LEVEL=<level>[:<mission>[:<players>]]
//   Starts a level without the menu, the way the game itself restarts a level: launch data in the
//   game's relaunch format (magic 'FCI1' at +0x3A0, command "-map <level>[ -lpc<n>]" at +0x3B2,
//   controller port at +0x3F2 from FCP_START_PORT, default 0). Game_ReadLaunchData then sets the port,
//   maps the pad and selects the profile of that user (ProfileMgr_SelectUser), as after a relaunch.
//   The profile's 12 option bytes the relaunch carries over (+0x180) do not exist on a fresh start:
//   right before Profile_ReadLaunchData the hook fills them from the freshly selected profile with the
//   game's own Profile_WriteLaunchData, so the read is an identity (options, difficulty and profile
//   state stay as loaded). The game's -map path uses the level name as mission; another mission is
//   refused with a log line. <players> becomes -lpc<n>.
// FCP_LAUNCH_DATA=<text>
//   Raw: the same launch data with <text> as command and nothing else (no profile carry-over).
// FCP_TEST_COMMAND=<s>:<command>[|<s>:<command>...]
//   Pushes game commands (Game_PushCommand, main thread, before the main loop body) s seconds after
//   title launch, e.g. "ReturnToMenu" or "FadeOutRestartStartLevel level=00_training".
//   FCP_TEST_COMMAND_GENS=<g>[,<g>...]|all: relaunch generations it applies to (default 0).
// FCP_TEST_MAPEDITOR=<s>
//   Calls the menu's map editor launcher (Menu_LaunchMapEditor) at s seconds (generation 0 only).
// FCP_TEST_DASHBOARD=1
//   Launch data with a wrong magic: Game_Run exits to the dashboard (XLaunchNewImage(NULL)).
// FCP_INPUT_SCRIPT_GENS=<g>[,<g>...]|all: generations FCP_INPUT_SCRIPT applies to (default 0).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace rex::kernel {
class TitleRuntime;
}

namespace fcp::devstart {

/** Reads the switches once (after fcp::relaunch::Configure). */
void Configure();

/**
 * Fills a launch data buffer for XamLoaderGetLaunchData when the title has none. Returns false when
 * neither FCP_START_LEVEL nor FCP_LAUNCH_DATA asks for it.
 */
bool FillLaunchData(uint8_t* buffer, uint32_t size);

/** Main thread, at the start of every Game_MainLoop call (r3 = game object). */
void OnMainLoop(PPCContext& ctx, uint8_t* base);

/** After Game_ReadLaunchData (game = its r3): FCP_START_LEVEL creates the game session like the menu. */
void AfterReadLaunchData(PPCContext& ctx, uint8_t* base, uint32_t game);

/**
 * After Game_Run returned (main thread). The title only returns when "Quit-Yes" ended the main loop (console
 * command "quit"); the CRT exit after it crashes in a static destructor of the audio library (testwerkzeuge.md,
 * Fehler B). Ends the process cleanly instead, as the dashboard exit; FCP_TITLE_EXIT_CRT=1 runs the original
 * CRT exit.
 */
void OnGameRunReturned(PPCContext& ctx, uint8_t* base);

/** The title runtime (title launch), for the title exit above. */
void SetRuntime(rex::kernel::TitleRuntime& runtime);

/** The title runtime set above, or null (pc_input: XN_SYS_UI of the focus pause). */
rex::kernel::TitleRuntime* Runtime();

/** True after the title quit through Game_Run (the app ends without the "title exited" dialog). */
bool TitleQuit();

/** True when a switch with a generation list (default "0") applies to this process. */
bool ActiveInGeneration(const char* gens_variable);

}  // namespace fcp::devstart
