// What the title receives from XamInputGetState and sends with XamInputSetState (measurement only,
// nothing is changed). Always on, cheap: counters per user, a log line on each change of the buttons,
// sticks or triggers of a connected user (first FCP_INPUT_TRACE_LINES, default 200), each change of the
// vibration values (same limit), and a summary at shutdown. FCP_INPUT_TRACE=0 turns it off.
#pragma once

#include <cstdint>

namespace fcp::input_trace {

void Configure();
bool Enabled();

/** After XamInputGetState(user, flags, state*) returned result (state already written by the SDK). */
void OnGetState(uint32_t user, uint32_t result, uint8_t* base, uint32_t state_address);

/** After XamInputSetState(user, unk, vibration*) returned result. */
void OnSetState(uint32_t user, uint32_t result, uint8_t* base, uint32_t vibration_address);

void WriteSummary();

}  // namespace fcp::input_trace
