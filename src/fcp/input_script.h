// FCP_INPUT_SCRIPT: deterministic pad input for tests (reproducible runs), applied in the import wrapper of
// XamInputGetState after the SDK answered. Only the script part; real pads stay with the SDK input system.
//
//   FCP_INPUT_SCRIPT=<entry>[,<entry>...]
//   entry    = <t>[-<t_end>]:<control>[+<control>...][:<value>][@<player>]
//   control  = a b x y start back up down left right lb rb ls rs   (buttons)
//            | lup ldown lleft lright rup rdown rleft rright      (stick directions, value 0..1)
//            | lt rt                                                (triggers, value 0..1)
//            | <analog>=<value>                                    (own value inside a combination)
//   <t>      seconds on the script clock; held FCP_INPUT_HOLD_MS (150) or until <t_end>
//   <value>  analog value for every analog control without its own (default 1)
//   @<player> 1..4 (default 1). A player named in the script is a connected pad for the whole run (neutral
//            state when the SDK has no pad for that user).
//
//   FCP_INPUT_SCRIPT_CLOCK=wall (default: seconds since title launch) | level (game time since the level
//            start, see fcp/gametime.h: equal at 30 and 120 frames/s, stops while paused)
//   FCP_INPUT_SCRIPT_GENS=<g,...>|all  relaunch generations that run the script (default 0)
//
// Examples: "34:start,38:start,42:a"  "2-6:lup"  "7:rright:0.15"  "8-9:lup=1+rright=0.3+rt"  "10:a@2"
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace fcp::input_script {

/** Reads the switches (title launch). */
void Configure();

/** True when a script is active in this process. */
bool Active();

/** After XamInputGetState(user, flags, state) returned (ctx.r3 = result). */
void OnGetState(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state);

/** After XamInputGetCapabilities(user, flags, caps) returned. */
void OnGetCapabilities(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t caps);

/** Called by the game clock when a level starts. */
void OnLevelStart();

}  // namespace fcp::input_script
