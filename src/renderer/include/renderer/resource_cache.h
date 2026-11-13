// ResourceCache: guest resource -> descriptor index in the bindless tables.
//   fetch constant (6 words) -> texture index (table by dimension)
//   (physical address, size) -> index of a raw buffer with the vertex data as it lies in the guest (big-endian)
//
// The conversion is King Kong's (kk/resources: untiling, endian, mip tail, format table, ConvertTexture).
// This implementation uploads on every new key and keeps everything; invalidation (write protection, content
// hash), residency and release are not handled here (King Kong's texture_cache.h, buffers.h are already in
// kk/resources). Not thread-safe: runs on the renderer's execution thread.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include "kk/resources/guest_texture.h"
#include "kk/resources/texture_convert.h"
#include "renderer/translated_layout.h"

namespace fcp::nr {

class ResourceCache {
 public:
  struct Stats {
    uint64_t textures = 0, texture_bytes = 0, buffers = 0, buffer_bytes = 0, hits = 0, failures = 0;
  };
  ResourceCache(nvrhi::IDevice* device, TranslatedLayout& layout) : device_(device), layout_(layout) {}

  // Texture for a fetch constant (host order of the 6 words) via `memory`. 0 = fallback (error: why).
  uint32_t Texture(const uint32_t fetch[6], const res::GuestMemory& memory, nvrhi::ICommandList* cl, std::string* why = nullptr);
  // Upload already converted host data (tests, probe); 0 on failure.
  uint32_t Upload(const res::HostTextureData& data, nvrhi::ICommandList* cl, const char* name);
  // Raw buffer with `bytes` bytes from `data` (guest memory, unchanged); key (physical, bytes).
  uint32_t Buffer(uint32_t physical, const uint8_t* data, uint32_t bytes, nvrhi::ICommandList* cl);
  const Stats& stats() const { return stats_; }

 private:
  nvrhi::IDevice* device_;
  TranslatedLayout& layout_;
  std::unordered_map<std::string, uint32_t> textures_;   // key: the 6 words as bytes
  std::unordered_map<uint64_t, uint32_t> buffers_;
  std::vector<nvrhi::TextureHandle> keep_textures_;
  std::vector<nvrhi::BufferHandle> keep_buffers_;
  Stats stats_;
};

}  // namespace fcp::nr
