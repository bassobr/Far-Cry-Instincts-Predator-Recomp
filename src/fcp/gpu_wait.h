// GPU wait loops of the D3D library sleep instead of spinning (FCP_GPU_WAIT, see gpu_wait.cpp).
#pragma once

namespace fcp::gpuwait {

/** Counters for the shutdown summary. */
void WriteSummary();

}  // namespace fcp::gpuwait
