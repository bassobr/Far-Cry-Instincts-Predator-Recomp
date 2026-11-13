// ResourceCache (renderer/resource_cache.h), basic version. Own code; the conversion is kk/resources.
#include "renderer/resource_cache.h"

#include <cstring>

#include "kk/resources/nvrhi_format.h"

namespace fcp::nr {

uint32_t ResourceCache::Upload(const res::HostTextureData& data, nvrhi::ICommandList* cl, const char* name) {
  const res::HostTexturePlan& p = data.plan;
  nvrhi::TextureDesc d;
  d.width = p.width;
  d.height = p.height;
  d.mipLevels = p.levels;
  d.format = res::ToNvrhi(p.format);
  Table table = Table::kTex2D;
  switch (p.dimension) {
    case res::Dimension::k3D:
      d.dimension = nvrhi::TextureDimension::Texture3D, d.depth = p.depth, table = Table::kTex3D;
      break;
    case res::Dimension::kCube:
      d.dimension = nvrhi::TextureDimension::TextureCube, d.arraySize = 6, table = Table::kTexCube;
      break;
    default:
      if (p.layers > 1) d.dimension = nvrhi::TextureDimension::Texture2DArray, d.arraySize = p.layers, table = Table::kTex2DArray;
      else d.dimension = nvrhi::TextureDimension::Texture2D;
      break;
  }
  d.initialState = nvrhi::ResourceStates::ShaderResource;
  d.keepInitialState = true;
  d.debugName = name ? name : "guest texture";
  if (d.format == nvrhi::Format::UNKNOWN) return ++stats_.failures, 0;
  nvrhi::TextureHandle t = device_->createTexture(d);
  if (!t) return ++stats_.failures, 0;
  for (const res::HostSubresource& s : data.subresources)
    cl->writeTexture(t, s.layer, s.level, data.bytes.data() + s.offset, s.row_pitch, s.depth_pitch);
  // The fetch's component mapping (view_swizzle) belongs in the SRV; King Kong's NVRHI version uses a
  // separate texture view. This path binds the texture without swizzle (probe: formats with XYZW).
  const uint32_t slot = layout_.AddTexture(table, t);
  if (!slot) return ++stats_.failures, 0;
  keep_textures_.push_back(t);
  ++stats_.textures;
  stats_.texture_bytes += data.bytes.size();
  return slot;
}

uint32_t ResourceCache::Texture(const uint32_t fetch[6], const res::GuestMemory& memory, nvrhi::ICommandList* cl,
                                std::string* why) {
  std::string key(reinterpret_cast<const char*>(fetch), 24);
  if (auto it = textures_.find(key); it != textures_.end()) return ++stats_.hits, it->second;
  res::HostTextureData data;
  if (!res::ConvertTexture(res::TextureFetch::FromWords(fetch), memory, data, why)) return ++stats_.failures, 0;
  const uint32_t slot = Upload(data, cl, "guest texture");
  if (slot) textures_.emplace(std::move(key), slot);
  return slot;
}

uint32_t ResourceCache::Buffer(uint32_t physical, const uint8_t* data, uint32_t bytes, nvrhi::ICommandList* cl) {
  const uint64_t key = (uint64_t(physical) << 32) | bytes;
  if (auto it = buffers_.find(key); it != buffers_.end()) return ++stats_.hits, it->second;
  nvrhi::BufferDesc d;
  d.byteSize = (bytes + 3) & ~3u;
  d.canHaveRawViews = true;
  d.initialState = nvrhi::ResourceStates::ShaderResource;
  d.keepInitialState = true;
  d.debugName = "guest vertex data";
  nvrhi::BufferHandle b = device_->createBuffer(d);
  if (!b) return ++stats_.failures, 0;
  std::vector<uint8_t> padded(d.byteSize, 0);
  std::memcpy(padded.data(), data, bytes);
  cl->writeBuffer(b, padded.data(), padded.size());
  const uint32_t slot = layout_.AddBuffer(b);
  if (!slot) return ++stats_.failures, 0;
  keep_buffers_.push_back(b);
  buffers_.emplace(key, slot);
  ++stats_.buffers;
  stats_.buffer_bytes += bytes;
  return slot;
}

}  // namespace fcp::nr
