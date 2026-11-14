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

ShaderLibrary::~ShaderLibrary() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_jobs_.notify_all();
  for (std::thread& t : workers_)
    if (t.joinable()) t.join();
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
  if (options.translate_missing && dxc_ && workers_.empty())
    for (unsigned i = 0; i < options.translate_threads; ++i) workers_.emplace_back([this] { Worker(); });
  if (note) *note = notes;
  return true;
}

bool ShaderLibrary::Translate(const uint8_t* container, size_t size, fcshaders::PackEntry& pe, std::string* error) {
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
  if (!r.ok) {
    *error = r.stage + ": " + r.error;
    return false;
  }
  pe.kind = r.reflection.kind;
  pe.code = std::move(o.dxil ? r.dxil : r.spirv);
  pe.reflection = std::move(r.reflection);
  return true;
}

void ShaderLibrary::Worker() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      cv_jobs_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    ShaderEntry& e = *job.entry;
    const auto t0 = std::chrono::steady_clock::now();
    fcshaders::PackEntry pe;
    pe.containerHash = e.container_hash;
    std::string error;
    if (Translate(job.container.data(), job.container.size(), pe, &error)) {
      e.kind = pe.kind;
      e.reflection = pe.reflection;
      e.code = pe.code;
      e.source = ShaderSource::kTranslated;
      if (cache_.isOpen()) cache_.put(pe);
    } else {
      e.source = ShaderSource::kFailed;
      e.error = error;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    {
      std::lock_guard lock(mutex_);
      if (e.source == ShaderSource::kTranslated) ++stats_.translated;
      else ++stats_.failed;
      e.pending.store(false, std::memory_order_release);
    }
    cv_ready_.notify_all();
    if (options_.on_translated) options_.on_translated(e, ms);
  }
}

const ShaderEntry* ShaderLibrary::WaitReady(const ShaderEntry* e) const {
  if (!e || !e->pending.load(std::memory_order_acquire)) return e;
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock lock(mutex_);
  cv_ready_.wait(lock, [&] { return !e->pending.load(std::memory_order_acquire) || stop_; });
  stats_.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return e;
}

const ShaderEntry* ShaderLibrary::Resolve(uint64_t hash, const uint8_t* container, size_t size, bool may_translate) {
  {
    std::lock_guard lock(mutex_);
    auto it = by_hash_.find(hash);
    if (it != by_hash_.end()) {
      ++stats_.memory_hits;
      return it->second.get();  // possibly still pending (callers that read it wait)
    }
  }
  auto entry = std::make_unique<ShaderEntry>();
  entry->container_hash = hash;
  fcshaders::PackEntry pe;
  if (pack_.isOpen() && pack_.find(hash, pe)) {
    entry->source = ShaderSource::kPack;
  } else if (cache_.isOpen() && cache_.get(hash, pe)) {
    entry->source = ShaderSource::kDiskCache;
  } else if (may_translate && container && options_.translate_missing && dxc_ && !workers_.empty()) {
    // On a worker: copy the container (guest memory may change), publish the pending entry.
    Job job;
    job.container.assign(container, container + size);
    entry->source = ShaderSource::kTranslated;
    entry->pending.store(true, std::memory_order_relaxed);
    std::lock_guard lock(mutex_);
    auto [it, inserted] = by_hash_.emplace(hash, std::move(entry));
    if (!inserted) return it->second.get();  // another thread published it first
    job.entry = it->second.get();
    ++stats_.queued;
    jobs_.push_back(std::move(job));
    cv_jobs_.notify_one();
    return it->second.get();
  } else if (may_translate && container && options_.translate_missing && dxc_) {
    // Translate outside the lock: other shaders may be looked up meanwhile.
    std::string error;
    pe.containerHash = hash;
    if (Translate(container, size, pe, &error)) {
      entry->source = ShaderSource::kTranslated;
      if (cache_.isOpen()) cache_.put(pe);
    } else {
      entry->source = ShaderSource::kFailed;
      entry->error = error;
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
  const ShaderEntry* e = nullptr;
  {
    std::lock_guard lock(mutex_);
    auto it = by_object_.find(object);
    if (it != by_object_.end()) e = it->second;
  }
  return WaitReady(e);
}

const ShaderEntry* ShaderLibrary::FindByHash(uint64_t hash) { return WaitReady(Resolve(hash, nullptr, 0, false)); }

ShaderLibrary::Stats ShaderLibrary::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

}  // namespace fcp::nr
