// ShaderLibrary (renderer/shader_library.h). This project's own code.
#include "renderer/shader_library.h"

#include <chrono>

namespace fcp::nr {

const char* ShaderSourceName(ShaderSource source) {
  switch (source) {
    case ShaderSource::kPack: return "pack";
    case ShaderSource::kDiskCache: return "cache";
    case ShaderSource::kTranslated: return "translated";
    case ShaderSource::kFailed: return "failed";
  }
  return "?";
}

bool ShaderLibrary::Open(const Options& options, std::string* error, std::string* note) {
  options_ = options;
  std::string message;
  if (options.translate_missing || !options.cache_dir.empty()) {
    dxc_ = fcshaders::loadDxc(options.dxc_path, &message);
    if (!dxc_ && options.translate_missing) {
      if (error) *error = "DXC: " + message;
      return false;
    }
  }
  // Without DXC there is no stamp: a pack is then not checked and the cache is not used.
  const uint64_t stamp = dxc_ ? fcshaders::buildStamp() : 0;
  std::string notes;
  if (!options.pack_path.empty()) {
    if (!pack_.open(options.pack_path, stamp, &message)) {
      notes += "pack not loaded: " + message + ". ";
    } else if (pack_.target() != options.target) {
      pack_.close();
      notes += "pack has a different target. ";
    } else if (pack_.stale() && !options.accept_stale_pack) {
      pack_.close();
      notes += "pack outdated (different translator or DXC), not used. ";
    }
  }
  if (!options.cache_dir.empty() && dxc_ && !cache_.open(options.cache_dir, options.target, stamp, &message))
    notes += "disk cache off: " + message + ". ";
  if (note) *note = notes;
  return true;
}

const ShaderEntry* ShaderLibrary::Resolve(uint64_t hash, const uint8_t* container, size_t size, bool may_translate) {
  {
    std::lock_guard lock(mutex_);
    auto it = by_hash_.find(hash);
    if (it != by_hash_.end()) {
      ++stats_.memory_hits;
      return it->second.get();
    }
  }
  auto entry = std::make_unique<ShaderEntry>();
  entry->container_hash = hash;
  fcshaders::PackEntry pe;
  if (pack_.isOpen() && pack_.find(hash, pe)) {
    entry->source = ShaderSource::kPack;
  } else if (cache_.isOpen() && cache_.get(hash, pe)) {
    entry->source = ShaderSource::kDiskCache;
  } else if (may_translate && container && options_.translate_missing && dxc_) {
    // Translate outside the lock: other shaders may be looked up meanwhile.
    thread_local std::unique_ptr<fcshaders::Compiler> compiler;
    if (!compiler) compiler = std::make_unique<fcshaders::Compiler>();
    fcshaders::BuildOptions o;
    o.dxil = options_.target == fcshaders::PackTarget::Dxil;
    o.spirv = !o.dxil;
    const auto t0 = std::chrono::steady_clock::now();
    fcshaders::BuildResult r = fcshaders::buildShader(container, size, compiler.get(), o);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
      std::lock_guard lock(mutex_);
      stats_.translate_ms += ms;
    }
    if (r.ok) {
      pe.containerHash = hash;
      pe.kind = r.reflection.kind;
      pe.code = std::move(o.dxil ? r.dxil : r.spirv);
      pe.reflection = std::move(r.reflection);
      entry->source = ShaderSource::kTranslated;
      if (cache_.isOpen()) cache_.put(pe);
    } else {
      entry->source = ShaderSource::kFailed;
      entry->error = r.stage + ": " + r.error;
    }
  } else {
    if (!may_translate) return nullptr;
    entry->source = ShaderSource::kFailed;
    entry->error = "not in pack/cache, translation off";
  }
  if (entry->source != ShaderSource::kFailed) {
    entry->kind = pe.kind;
    entry->code = std::move(pe.code);
    entry->reflection = std::move(pe.reflection);
  }
  std::lock_guard lock(mutex_);
  switch (entry->source) {
    case ShaderSource::kPack: ++stats_.pack_hits; break;
    case ShaderSource::kDiskCache: ++stats_.cache_hits; break;
    case ShaderSource::kTranslated: ++stats_.translated; break;
    case ShaderSource::kFailed: ++stats_.failed; break;
  }
  auto [it, inserted] = by_hash_.emplace(hash, std::move(entry));  // a second thread may finish at the same time
  return it->second.get();
}

const ShaderEntry* ShaderLibrary::OnShaderCreated(uint32_t object, const uint8_t* container, size_t max_size) {
  {
    std::lock_guard lock(mutex_);
    ++stats_.created;
  }
  const size_t size = fcshaders::containerSize(container, max_size);
  if (!size) return nullptr;
  const ShaderEntry* e = Resolve(fcshaders::containerHash(container, size), container, size, true);
  std::lock_guard lock(mutex_);
  if (object) by_object_[object] = e;
  return e;
}

void ShaderLibrary::OnShaderReleased(uint32_t object) {
  std::lock_guard lock(mutex_);
  by_object_.erase(object);
}

const ShaderEntry* ShaderLibrary::Find(uint32_t object) const {
  std::lock_guard lock(mutex_);
  auto it = by_object_.find(object);
  return it == by_object_.end() ? nullptr : it->second;
}

const ShaderEntry* ShaderLibrary::FindByHash(uint64_t hash) { return Resolve(hash, nullptr, 0, false); }

ShaderLibrary::Stats ShaderLibrary::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

}  // namespace fcp::nr
