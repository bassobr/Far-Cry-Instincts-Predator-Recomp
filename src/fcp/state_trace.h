// FCP_TRACE_STATE=<n>: state of the local player every n-th frame (1 = every frame) as CSV on the game
// clock (logs/state[.gen<n>].csv): position, angles, speed (from the position over game time), health,
// armor, in water / under water, breath, firing/reloading/jumping flags, clip counters and shots of the local
// player. The roles were read from the image (layout in state_trace.cpp).
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace fcp::state {

void Configure();

/** Once per frame from Game_MainLoop (main thread, before the original). */
void OnFrame(PPCContext& ctx, uint8_t* base);

/** Weapon_Fire entry (one bullet; r4 = shot parameters, +0x1C = shooter). Counts the local player's bullets. */
void OnWeaponFire(uint8_t* base, uint32_t shot);

/** Player object (entity +0x254) of the local player in the last traced frame, 0 without FCP_TRACE_STATE. */
uint32_t PlayerObject();

/** Position of the local player in the last traced frame (false without FCP_TRACE_STATE or before the first). */
bool PlayerPosition(float out[3]);

void Shutdown();

}  // namespace fcp::state
