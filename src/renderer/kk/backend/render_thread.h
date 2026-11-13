// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/backend/render_thread.h),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md (adapted to our DrawSink records, device ranges and object sizes from the
// configuration instead of literals, AVX2 path, GPU loss and long-frame report removed).
//
// ThreadedSink: the renderer's work on a thread of its own. The hooks only copy what each call needs and queue it:
//
// - The D3D device struct, because the game changes it right after the call. Only the parts the renderer reads
//   (GuestLayout::snapshot_ranges, config/predator/renderer.toml), and of those only the 64-byte blocks that changed
//   since the previous call: the render thread keeps one copy of the device and brings it up to date with each
//   call's changes before running it.
// - The small guest objects a call names (render target and depth surfaces, the vertex declaration, the vertex
//   buffer objects of the 16 streams, a resolve's destination texture): their headers can be rewritten for the
//   next call. Sizes come from GuestLayout's object layouts.
// - DrawVerticesUP data (the pointer is valid during the call only), and a shader container (OnShaderCreated runs
//   on the calling thread anyway).
// - Not the data (vertices, indices, textures): the renderer reads guest memory when it runs the call. The game
//   rewrites such memory only after the GPU passed a fence; the ring skimmer (M1) writes a fence only after the
//   render thread ran every call queued before it (WaitDone). For Predator this is NOT checked yet (design
//   section 7).
// - Queries: GetData is answered on the calling thread from the inner sink's thread-safe result table
//   (OnQuery of the inner sink must then be callable from any thread and must not block); Issue is queued.
//
// The renderer's guest reads go through SnapshotGuestMemory: on the render thread, an address inside a copied block
// reads the copy; everything else, and every other thread, reads live memory.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "kk/backend/guest_memory.h"
#include "renderer/draw_sink.h"
#include "renderer/guest_device.h"

namespace fcp::nr {

// Guest bytes copied at a call on the game's thread.
struct GuestSnapshot {
  // The device struct as the call saw it (the render thread's copy; null: none).
  const uint8_t* device = nullptr;
  uint32_t device_size = 0;
  uint32_t device_address = 0;
  std::vector<uint8_t> bytes;  // the objects, back to back
  struct Object {
    uint32_t address = 0, size = 0, offset = 0;
  };
  std::vector<Object> objects;

  // Copies `size` bytes at a guest virtual address (nothing when unmapped or already copied).
  void AddObject(const GuestMemory& memory, uint32_t address, uint32_t size);
  // The copy of the byte at `address`, or null when no block holds it.
  const uint8_t* Find(uint32_t address) const;
};

// GuestMemory that reads the render thread's current snapshot first.
class SnapshotGuestMemory final : public GuestMemory {
 public:
  explicit SnapshotGuestMemory(const GuestMemory& live) : live_(live) {}
  const uint8_t* Virtual(uint32_t address) const override;
  const uint8_t* Physical(uint32_t address) const override { return live_.Physical(address); }
  bool CopyVirtual(uint32_t address, void* output, uint32_t bytes) const override {
    return live_.CopyVirtual(address, output, bytes);
  }
  // The snapshot of the call the calling thread is running (null: live memory).
  static void SetCurrent(const GuestSnapshot* snapshot);

 private:
  const GuestMemory& live_;
};

class ThreadedSink final : public DrawSink {
 public:
  struct Stats {
    uint64_t commands = 0;
    uint64_t device_copies = 0;      // calls whose device differed from the previous call's
    uint64_t device_bytes = 0;       // device bytes copied in all
    uint64_t enqueue_us = 0;         // the game's thread copying and queueing
    uint64_t backpressure_us = 0;    // the game's thread waiting for room in the queue
    uint64_t fence_waits = 0;        // WaitDone calls that had to wait
    uint64_t fence_wait_us = 0;
    uint64_t max_backlog = 0;        // commands
    uint64_t presents = 0;
    uint64_t render_idle_us = 0;
  };

  // `inner` runs the calls on the render thread; `memory` is live guest memory, read on the game's thread for the
  // copies; `layout` gives the device ranges and object sizes.
  ThreadedSink(DrawSink& inner, const GuestMemory& memory, const GuestLayout& layout);
  ~ThreadedSink() override;  // runs what is queued, then stops

  void OnDeviceCreated(uint32_t device) override;
  void OnDraw(const DrawRecord& r) override;
  void OnClear(const ClearRecord& r) override;
  void OnResolve(const ResolveRecord& r) override;
  void OnTiling(const TilingRecord& r) override;
  void OnPredication(const PredicationRecord& r) override;
  uint32_t OnQuery(const QueryRecord& r, uint32_t* samples) override;
  // Shader creation stays on the calling thread (the shader library has its own lock).
  void OnShaderCreated(const ShaderCreatedRecord& r) override { inner_.OnShaderCreated(r); }
  void OnShaderReleased(uint32_t kind, uint32_t object) override;
  void OnSwap(const SwapRecord& r) override;

  // The device's 64-byte blocks the snapshot compares (from the layout's snapshot ranges).
  const std::vector<std::pair<uint32_t, uint32_t>>& DeviceBlocks() const { return blocks_; }

  // Frames the game may queue before its Swap waits for the render thread (set before the first call).
  uint32_t max_queued_presents = 1;
  // Calls queued so far (a ticket), and a wait until the render thread has run that many.
  uint64_t Ticket() const { return queued_.load(std::memory_order_acquire); }
  void WaitDone(uint64_t ticket);
  Stats stats() const;

 private:
  enum class Kind : uint8_t { kDevice, kDraw, kClear, kResolve, kTiling, kPredication, kQuery, kRelease, kSwap };
  // Commands are reused (their vectors keep their room): no allocation per call once warm.
  struct Command {
    Kind kind = Kind::kDraw;
    DrawRecord draw{};
    ClearRecord clear{};
    ResolveRecord resolve{};
    TilingRecord tiling{};
    PredicationRecord predication{};
    QueryRecord query{};
    SwapRecord swap{};
    uint32_t value0 = 0, value1 = 0;  // kDevice: device; kRelease: kind, object
    std::vector<uint8_t> up_data;     // DrawVerticesUP
    GuestSnapshot snapshot;
    uint32_t device_address = 0;
    std::vector<uint16_t> device_blocks;
    std::vector<uint8_t> device_bytes;
  };
  std::unique_ptr<Command> NewCommand(Kind kind);
  bool CaptureDevice(Command& c, uint32_t device);
  uint32_t SentDeviceWord(DeviceField field, uint32_t index = 0) const;
  void CaptureObject(GuestSnapshot& s, uint32_t address, const ObjectLayout& layout);
  void CaptureBoundSurfaces(Command& c);
  void Enqueue(std::unique_ptr<Command> command, std::chrono::steady_clock::time_point start);
  void Run();

  DrawSink& inner_;
  const GuestMemory& memory_;
  const GuestLayout& layout_;
  std::vector<std::pair<uint32_t, uint32_t>> blocks_;  // offset, size
  std::vector<uint8_t> sent_device_;
  uint32_t sent_device_address_ = 0;
  std::vector<uint8_t> device_image_;

  mutable std::mutex mutex_;
  std::condition_variable work_cv_, room_cv_, done_cv_;
  std::deque<std::unique_ptr<Command>> queue_;
  std::vector<std::unique_ptr<Command>> free_;
  uint32_t queued_presents_ = 0;
  std::atomic<uint64_t> queued_{0};
  uint64_t done_ = 0;
  uint64_t done_wake_at_ = UINT64_MAX;
  bool room_wanted_ = false;
  bool stop_ = false;
  Stats stats_;
  std::thread thread_;
};

}  // namespace fcp::nr
