// Logging hooks on engine anchors (src/fcp/engine_hooks.cpp).
#pragma once

#include <cstdint>

namespace fcp::engine {

/** Writes the call counts of the engine hooks into the log. */
void WriteSummary();

/** Calls of an engine hook so far, by name (0 for an unknown name). */
uint64_t CountOf(const char* name);

}  // namespace fcp::engine
