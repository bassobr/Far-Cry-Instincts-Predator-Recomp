// TargetCache (renderer/target_cache.h), basic version. Own code. The numbering of the Xenos target formats
// (ColorRenderTargetFormat) follows Xenia/ReXGlue (xenos.h, BSD-3, see src/renderer/kk/LICENSE-Xenia-ReXGlue-BSD3.txt).
#include "renderer/target_cache.h"

#include <algorithm>
#include <cmath>

namespace fcp::nr {

namespace {

nvrhi::Format ColorFormat(uint32_t f) {
  using N = nvrhi::Format;
  switch (f) {
    case 0: case 1: return N::RGBA8_UNORM;          // k_8_8_8_8, _GAMMA
    case 2: case 10: return N::R10G10B10A2_UNORM;   // k_2_10_10_10, _AS_10_10_10_10
    case 3: case 12: return N::RGBA16_FLOAT;        // k_2_10_10_10_FLOAT (7e3), _AS_16_16_16_16
    case 4: return N::RG16_SNORM;                   // k_16_16 (fixed -32..32)
    case 5: return N::RGBA16_SNORM;                 // k_16_16_16_16
    case 6: return N::RG16_FLOAT;
    case 7: return N::RGBA16_FLOAT;
    case 14: return N::R32_FLOAT;
    case 15: return N::RG32_FLOAT;
    default: return N::UNKNOWN;
  }
}

}  // namespace

size_t SurfaceKeyHash::operator()(const SurfaceKey& k) const {
  uint64_t h = 1469598103934665603ull;
  for (uint64_t v : {uint64_t(k.depth), uint64_t(k.edram_base), uint64_t(k.pitch), uint64_t(k.format), uint64_t(k.msaa),
                     uint64_t(k.width), uint64_t(k.height)})
    h = (h ^ v) * 1099511628211ull;
  return size_t(h);
}

uint32_t TargetCache::HostSize(uint32_t guest) const { return std::max(1u, uint32_t(std::lround(guest * scale_))); }

nvrhi::ITexture* TargetCache::Get(const SurfaceKey& key) {
  if (auto it = targets_.find(key); it != targets_.end()) return it->second;
  nvrhi::TextureDesc d;
  d.width = HostSize(key.width);
  d.height = HostSize(key.height);
  // Depth: D24S8 and D24FS8 -> D32 with stencil (King Kong; reverse Z still to be checked).
  d.format = key.depth ? nvrhi::Format::D32S8 : ColorFormat(key.format);
  if (d.format == nvrhi::Format::UNKNOWN) return nullptr;
  d.isRenderTarget = true;
  d.initialState = key.depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
  d.keepInitialState = true;
  d.setClearValue(key.depth ? nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f) : nvrhi::Color(0.0f));
  d.debugName = key.depth ? "guest depth" : "guest color";
  nvrhi::TextureHandle t = device_->createTexture(d);
  if (!t) return nullptr;
  return targets_.emplace(key, t).first->second;
}

}  // namespace fcp::nr
