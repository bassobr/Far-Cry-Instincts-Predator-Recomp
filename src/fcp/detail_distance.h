// PC level of detail: [graphics] lod_distance / view_distance (src/fcp/detail_distance.cpp).
#pragma once

namespace fcp::detail {

/** At once (settings menu): the factors on the engine's console values; 1 = as the console. Clamped to 0.5..8. */
void SetLodDistance(float factor);
void SetViewDistance(float factor);

}  // namespace fcp::detail
