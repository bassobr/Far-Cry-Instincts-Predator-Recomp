// ShaderLibrary: guest shader object -> translated shader (code for one target + reflection).
//
// Sources in this order: the disc pack (fcshaders/pack.h, built by the user from their disc), the disk cache
// (shaders translated later, e.g. the ≈ 86 runtime shaders), and finally translation in the create hook
// (fcshaders + DXC), whose result goes into the disk cache. A second launch therefore translates nothing.
// The key is the container hash (fcshaders::containerHash).
//
// Threads: OnShaderCreated comes from any game thread; Find comes from the draw path. Both are thread-safe;
// entries are never moved or freed while the library lives (pointers stay valid).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "fcshaders/library.h"
#include "fcshaders/pack.h"

namespace fcp::nr {

enum class ShaderSource : uint8_t { kPack, kDiskCache, kTranslated, kFailed };
const char* ShaderSourceName(ShaderSource source);

struct ShaderEntry {
  uint64_t container_hash = 0;
  fcshaders::ShaderKind kind = fcshaders::ShaderKind::Pixel;
  std::vector<uint8_t> code;         // DXIL or SPIR-V (the library's target)
  fcshaders::Reflection reflection;
  ShaderSource source = ShaderSource::kFailed;
  std::string error;                 // for kFailed: why (the draw gets the fallback shader in the signal color)
};

class ShaderLibrary {
 public:
  struct Options {
    fcshaders::PackTarget target = fcshaders::PackTarget::Spirv;
    std::string pack_path;     // empty: no pack
    std::string cache_dir;     // empty: no disk cache
    std::string dxc_path;      // directory or dxcompiler.dll; empty: default search
    bool translate_missing = true;
    bool accept_stale_pack = false;  // FCP_SHADER_PACK_STALE_OK=1: accept a pack with a different stamp anyway
  };
  struct Stats {
    uint64_t created = 0;      // OnShaderCreated calls
    uint64_t pack_hits = 0, cache_hits = 0, translated = 0, failed = 0, memory_hits = 0;
    double translate_ms = 0;   // total translation + DXC time
  };

  ShaderLibrary() = default;
  ShaderLibrary(const ShaderLibrary&) = delete;
  ShaderLibrary& operator=(const ShaderLibrary&) = delete;

  // False with a message if DXC is needed and missing or the pack is unreadable. A missing pack is
  // not an error (note says so).
  bool Open(const Options& options, std::string* error, std::string* note = nullptr);
  const Options& options() const { return options_; }
  const fcshaders::PackReader& pack() const { return pack_; }

  // The create hook: container (host pointer, at most max_size bytes) for guest object `object`.
  const ShaderEntry* OnShaderCreated(uint32_t object, const uint8_t* container, size_t max_size);
  void OnShaderReleased(uint32_t object);
  // Draw path: guest object -> entry (null: unknown).
  const ShaderEntry* Find(uint32_t object) const;
  // By container hash (pack, cache, already seen). Loads from pack/cache on demand but does not translate.
  const ShaderEntry* FindByHash(uint64_t container_hash);
  Stats stats() const;

 private:
  const ShaderEntry* Resolve(uint64_t hash, const uint8_t* container, size_t size, bool may_translate);
  Options options_;
  fcshaders::PackReader pack_;
  fcshaders::DiskCache cache_;
  bool dxc_ = false;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> by_hash_;
  std::unordered_map<uint32_t, const ShaderEntry*> by_object_;
  Stats stats_;
};

}  // namespace fcp::nr
