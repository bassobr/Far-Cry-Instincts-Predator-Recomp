// Frame-rate coupling: measurements and corrections of game logic that depends on the number of frames instead
// of the time. Names of the hooked functions in [function_names]
// (config/predator/default.toml), no addresses here.
//
// Switches (read once at title launch):
//   FCP_TRACE_LAND=1   one log line per landing event of a player or AI (Player_Land): the vertical speed the
//                      event sends (fEntitySpeed = -[player +0xB24]), the frame time, the level time
//   FCP_TRACE_AI=1     counts the per-AI updates of the AI system (AI_UpdateEntity) by their flag; framestat
//                      shows them per window
#pragma once

#include <cstdint>

namespace fcp::frame_coupling {

/** Reads the switches. Called at title launch. */
void Configure();

/** AI_UpdateEntity calls so far with flag 0 (the AI's turn in the system's 4-frame rotation) and in total. */
uint64_t AiTurns();
uint64_t AiUpdates();

}  // namespace fcp::frame_coupling
