// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/resources/include/kknr/resource_work.h),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
// Opt-in CPU wall-time attribution for resource work. No clock reads when disabled.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>

namespace fcp::res {

struct ResourceWork {
  enum Stage { TextureConvert, TextureCreate, TextureStage, BufferCreate, BufferConvert,
               BufferStage, Hash, Watch, CacheBind, Invalidate, Descriptor, Submit, Snapshot, PrepareWall, Count };
  std::array<double, Count> ms{};
  std::array<uint64_t, Count> calls{};
};

class ResourceWorkTimer {
 public:
  ResourceWorkTimer(ResourceWork* work, ResourceWork::Stage stage) : work_(work), stage_(stage) {
    if (work_) { ++work_->calls[stage_]; start_ = std::chrono::steady_clock::now(); }
  }
  ~ResourceWorkTimer() {
    if (work_) work_->ms[stage_] += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start_).count();
  }
 private:
  ResourceWork* work_;
  ResourceWork::Stage stage_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace fcp::res
