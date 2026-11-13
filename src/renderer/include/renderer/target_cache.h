// TargetCache: guest surface description -> host render target.
//
// Key as in King Kong (depth yes/no, EDRAM base, pitch, format) plus MSAA type and the size from the
// tiling rectangle (not from the surface: on the console the EDRAM surface can be smaller than the image).
// No EDRAM aliasing: the same base with a different format is a different target. Render scaling: host size =
// round(guest x scale). Covers key, format choice (kk/resources/render_targets.h) and creation; resolve,
// clear values and pass tracking live elsewhere.
#pragma once

#include <cstdint>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

namespace fcp::nr {

struct SurfaceKey {
  bool depth = false;
  uint32_t edram_base = 0;  // tiles
  uint32_t pitch = 0;       // pixels
  uint32_t format = 0;      // ColorRenderTargetFormat or DepthRenderTargetFormat (0 D24S8, 1 D24FS8)
  uint32_t msaa = 0;        // 0 = 1x
  uint32_t width = 0, height = 0;  // guest pixels (from the tiling rectangle or the surface)
  bool operator==(const SurfaceKey&) const = default;
};
struct SurfaceKeyHash {
  size_t operator()(const SurfaceKey& k) const;
};

class TargetCache {
 public:
  explicit TargetCache(nvrhi::IDevice* device, float scale = 1.0f) : device_(device), scale_(scale) {}
  // Host target for the key; created on first use. null if the format has no host equivalent.
  nvrhi::ITexture* Get(const SurfaceKey& key);
  size_t size() const { return targets_.size(); }
  uint32_t HostSize(uint32_t guest) const;

 private:
  nvrhi::IDevice* device_;
  float scale_;
  std::unordered_map<SurfaceKey, nvrhi::TextureHandle, SurfaceKeyHash> targets_;
};

}  // namespace fcp::nr
