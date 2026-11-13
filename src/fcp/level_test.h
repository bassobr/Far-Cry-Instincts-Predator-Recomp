// Level end and level transition tests. Test only, all off by default;
// they change the game (player position) and must never be set for normal play.
//
// FCP_TEST_TELEPORT=<entry>[|<entry>...], entry = [<level>@]<s>:<x>,<y>,<z>
//   Sets the local player's entity position (entity virtual +0x14 = SetPos, as the Lua binding
//   Entity:SetPos 0x82320740) to x,y,z when the level clock (game time since the level start, see
//   fcp/gametime.h) reaches s. With <level> (case-insensitive, as in the StartLevel command) the entry
//   applies in every relaunch generation whose current level has that name; without, only in the
//   generations of FCP_TEST_TELEPORT_GENS (default 0). Used to put the player into the EndMapTrigger of a
//   level: the trigger's own Lua (Triggers\EndMapTrigger.lua) then runs Game:LoadNextMap(NextMap), the
//   real end-of-level path. Each entry runs once per process; when the position does not take (player bound
//   to a vehicle or a scripted sequence) it is repeated every 5 s, at most 36 times.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace fcp::level_test {

/** Reads the switches once (title launch, after fcp::relaunch::Configure). */
void Configure();

/** Main thread, at the start of every Game_MainLoop call. */
void OnMainLoop(PPCContext& ctx, uint8_t* base);

}  // namespace fcp::level_test
