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
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
  // Being translated on a worker (Options::translate_threads): only container_hash may be read until Find /
  // FindByHash / WaitReady returned it (they wait).
  std::atomic<bool> pending{false};
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
    // Translation of shaders missing from pack and cache on this many worker threads (0: in the create hook, as
    // before). The create hook then only copies the container; the first draw with the shader waits for it.
    unsigned translate_threads = 0;
    // Called on the worker after an asynchronous translation (log), with the time of the translation.
    std::function<void(const ShaderEntry&, double ms)> on_translated;
  };
  struct Stats {
    uint64_t created = 0;      // OnShaderCreated calls
    uint64_t pack_hits = 0, cache_hits = 0, translated = 0, failed = 0, memory_hits = 0;
    double translate_ms = 0;   // total translation + DXC time
    uint64_t queued = 0;       // asynchronous translations started
    double wait_ms = 0;        // time readers waited for asynchronous translations
  };

  ShaderLibrary() = default;
  ~ShaderLibrary();
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
  // Waits until an asynchronously translated entry is complete (returns e).
  const ShaderEntry* WaitReady(const ShaderEntry* e) const;
  Stats stats() const;

 private:
  struct Job {
    ShaderEntry* entry = nullptr;
    std::vector<uint8_t> container;
  };
  const ShaderEntry* Resolve(uint64_t hash, const uint8_t* container, size_t size, bool may_translate);
  // Translation + DXC into pe; false: error text in *error.
  bool Translate(const uint8_t* container, size_t size, fcshaders::PackEntry& pe, std::string* error);
  void Worker();
  std::vector<std::thread> workers_;
  std::deque<Job> jobs_;
  std::condition_variable cv_jobs_;
  mutable std::condition_variable cv_ready_;
  bool stop_ = false;
  Options options_;
  fcshaders::PackReader pack_;
  fcshaders::DiskCache cache_;
  bool dxc_ = false;
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::unique_ptr<ShaderEntry>> by_hash_;
  std::unordered_map<uint32_t, const ShaderEntry*> by_object_;
  mutable Stats stats_;
};

}  // namespace fcp::nr
