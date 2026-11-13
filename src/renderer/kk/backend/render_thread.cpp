// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/backend/render_thread.cpp),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
#include "kk/backend/render_thread.h"

#include <algorithm>
#include <cstring>

#include "kk/backend/thread_scheduling.h"

namespace fcp::nr {

namespace {

thread_local const GuestSnapshot* t_snapshot = nullptr;

// How far the game may run ahead of the render thread: max_queued_presents frames, or this many calls.
constexpr size_t kMaxQueuedCommands = 8000;
// Commands kept for reuse at most.
constexpr size_t kMaxFreeCommands = 1024;

uint64_t Micros(std::chrono::steady_clock::time_point from) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - from).count());
}

}  // namespace

void GuestSnapshot::AddObject(const GuestMemory& memory, uint32_t address, uint32_t size) {
  if (!address || !size) return;
  for (const Object& o : objects)
    if (o.address == address && o.size >= size) return;
  const uint8_t* p = memory.Virtual(address);
  if (!p) return;
  Object o{address, size, uint32_t(bytes.size())};
  bytes.insert(bytes.end(), p, p + size);
  objects.push_back(o);
}

const uint8_t* GuestSnapshot::Find(uint32_t address) const {
  if (device && address >= device_address && address - device_address < device_size) return device + (address - device_address);
  for (const Object& o : objects)
    if (address >= o.address && address - o.address < o.size) return bytes.data() + o.offset + (address - o.address);
  return nullptr;
}

const uint8_t* SnapshotGuestMemory::Virtual(uint32_t address) const {
  if (const GuestSnapshot* s = t_snapshot)
    if (const uint8_t* p = s->Find(address)) return p;
  return live_.Virtual(address);
}

void SnapshotGuestMemory::SetCurrent(const GuestSnapshot* snapshot) { t_snapshot = snapshot; }

ThreadedSink::ThreadedSink(DrawSink& inner, const GuestMemory& memory, const GuestLayout& layout)
    : inner_(inner), memory_(memory), layout_(layout), sent_device_(layout.device_size, 0), device_image_(layout.device_size, 0) {
  for (const auto& [a, b] : layout.snapshot_ranges)
    for (uint32_t o = a; o < b; o += 64) blocks_.emplace_back(o, std::min<uint32_t>(64, b - o));
  thread_ = std::thread([this] { Run(); });
}

ThreadedSink::~ThreadedSink() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  room_cv_.notify_all();
  done_cv_.notify_all();
  thread_.join();
}

std::unique_ptr<ThreadedSink::Command> ThreadedSink::NewCommand(Kind kind) {
  std::unique_ptr<Command> c;
  {
    std::lock_guard lock(mutex_);
    if (!free_.empty()) {
      c = std::move(free_.back());
      free_.pop_back();
    }
  }
  if (!c) c = std::make_unique<Command>();
  c->kind = kind;
  c->up_data.clear();
  c->snapshot.device = nullptr;
  c->snapshot.device_size = 0;
  c->snapshot.device_address = 0;
  c->snapshot.bytes.clear();
  c->snapshot.objects.clear();
  c->device_address = 0;
  c->device_blocks.clear();
  c->device_bytes.clear();
  return c;
}

bool ThreadedSink::CaptureDevice(Command& c, uint32_t device) {
  const uint8_t* live = device ? memory_.Virtual(device) : nullptr;
  if (!live || sent_device_.empty()) return false;
  // Only the parts the renderer reads: the library's dirty bits and counters change at every call elsewhere.
  const bool all = sent_device_address_ != device;
  uint8_t* sent = sent_device_.data();
  for (size_t i = 0; i < blocks_.size(); ++i) {
    const auto [offset, size] = blocks_[i];
    const bool same = !all && (size == 64 ? SameBytes64(live + offset, sent + offset) : std::memcmp(live + offset, sent + offset, size) == 0);
    if (same) continue;
    std::memcpy(sent + offset, live + offset, size);
    c.device_blocks.push_back(uint16_t(i));
    c.device_bytes.insert(c.device_bytes.end(), live + offset, live + offset + size);
  }
  sent_device_address_ = device;
  c.device_address = device;
  return true;
}

uint32_t ThreadedSink::SentDeviceWord(DeviceField field, uint32_t index) const {
  const FieldLayout& f = layout_.field(field);
  if (f.status == FieldStatus::kMissing) return 0;
  const uint64_t offset = uint64_t(f.offset) + uint64_t(f.stride) * index;
  return offset + 4 <= sent_device_.size() ? LoadBE32(sent_device_.data() + offset) : 0;
}

void ThreadedSink::CaptureObject(GuestSnapshot& s, uint32_t address, const ObjectLayout& layout) {
  if (layout.size) s.AddObject(memory_, address, layout.size);
}

void ThreadedSink::CaptureBoundSurfaces(Command& c) {
  for (uint32_t i = 0; i < 4; ++i) CaptureObject(c.snapshot, SentDeviceWord(DeviceField::render_targets, i), layout_.surface);
  CaptureObject(c.snapshot, SentDeviceWord(DeviceField::depth_stencil), layout_.surface);
}

void ThreadedSink::OnDeviceCreated(uint32_t device) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kDevice);
  c->value0 = device;
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnDraw(const DrawRecord& r) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kDraw);
  c->draw = r;
  if (r.kind == kDrawVerticesUP && r.up_data && r.up_bytes) {
    c->up_data.assign(r.up_data, r.up_data + r.up_bytes);
    c->draw.up_data = nullptr;  // set to the copy when it runs
  }
  if (CaptureDevice(*c, r.device)) {
    CaptureBoundSurfaces(*c);
    const uint32_t decl = SentDeviceWord(DeviceField::vertex_declaration);
    if (const uint8_t* d = decl ? memory_.Virtual(decl) : nullptr) {
      const uint32_t count = std::min(LoadBE32(d + layout_.declaration.get("count")), layout_.declaration.get("max_elements", 64));
      c->snapshot.AddObject(memory_, decl, layout_.declaration.get("elements") + layout_.declaration.get("element_size") * count);
    }
    for (uint32_t s = 0; s < 16; ++s) CaptureObject(c->snapshot, SentDeviceWord(DeviceField::stream_buffer, s), layout_.vertex_buffer);
    CaptureObject(c->snapshot, SentDeviceWord(DeviceField::index_buffer), layout_.index_buffer);
    for (uint32_t t = 0; t < 26; ++t) CaptureObject(c->snapshot, SentDeviceWord(DeviceField::textures, t), layout_.texture);
  }
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnClear(const ClearRecord& r) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kClear);
  c->clear = r;
  if (CaptureDevice(*c, r.device)) CaptureBoundSurfaces(*c);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnResolve(const ResolveRecord& r) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kResolve);
  c->resolve = r;
  if (CaptureDevice(*c, r.device)) CaptureBoundSurfaces(*c);
  CaptureObject(c->snapshot, r.dest_texture, layout_.texture);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnTiling(const TilingRecord& r) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kTiling);
  c->tiling = r;
  if (CaptureDevice(*c, r.device)) CaptureBoundSurfaces(*c);
  if (r.rects && r.rect_count) c->snapshot.AddObject(memory_, r.rects, 16 * std::min<uint32_t>(r.rect_count, 64));
  CaptureObject(c->snapshot, r.dest_texture, layout_.texture);
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnPredication(const PredicationRecord& r) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kPredication);
  c->predication = r;
  Enqueue(std::move(c), start);
}

uint32_t ThreadedSink::OnQuery(const QueryRecord& r, uint32_t* samples) {
  // GetData: answered at once from the inner sink's results (never waits for the render thread).
  if (r.op == kQueryGetData) return inner_.OnQuery(r, samples);
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kQuery);
  c->query = r;
  if (r.op == kQueryIssueBegin || r.op == kQueryIssueEnd || r.op == kSurveyBegin || r.op == kSurveyEnd ||
      r.op == kConditionalBegin || r.op == kConditionalEnd)
    CaptureDevice(*c, r.device);
  Enqueue(std::move(c), start);
  return kQueryNotReady;
}

void ThreadedSink::OnShaderReleased(uint32_t kind, uint32_t object) {
  // After the draws queued before it (they may still draw with the shader).
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kRelease);
  c->value0 = kind;
  c->value1 = object;
  Enqueue(std::move(c), start);
}

void ThreadedSink::OnSwap(const SwapRecord& r) {
  const auto start = std::chrono::steady_clock::now();
  auto c = NewCommand(Kind::kSwap);
  c->swap = r;
  if (CaptureDevice(*c, r.device)) {
    CaptureObject(c->snapshot, SentDeviceWord(DeviceField::front_buffer), layout_.texture);
    CaptureObject(c->snapshot, SentDeviceWord(DeviceField::back_buffer), layout_.surface);
  }
  CaptureObject(c->snapshot, r.front_buffer, layout_.texture);
  Enqueue(std::move(c), start);
}

void ThreadedSink::Enqueue(std::unique_ptr<Command> command, std::chrono::steady_clock::time_point start) {
  const bool present = command->kind == Kind::kSwap;
  const uint64_t device_bytes = command->device_bytes.size();
  const bool device_copy = !command->device_blocks.empty();
  std::unique_lock lock(mutex_);
  const auto copied = std::chrono::steady_clock::now();
  while (!stop_ && (queue_.size() >= kMaxQueuedCommands || (present && queued_presents_ >= max_queued_presents))) {
    const auto wait = std::chrono::steady_clock::now();
    room_wanted_ = true;
    room_cv_.wait(lock);
    stats_.backpressure_us += Micros(wait);
  }
  queue_.push_back(std::move(command));
  if (present) ++queued_presents_, ++stats_.presents;
  stats_.max_backlog = std::max<uint64_t>(stats_.max_backlog, queue_.size());
  ++stats_.commands;
  stats_.device_bytes += device_bytes;
  if (device_copy) ++stats_.device_copies;
  queued_.fetch_add(1, std::memory_order_release);
  stats_.enqueue_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(copied - start).count());
  lock.unlock();
  work_cv_.notify_one();
}

void ThreadedSink::WaitDone(uint64_t ticket) {
  std::unique_lock lock(mutex_);
  if (done_ >= ticket) return;
  const auto start = std::chrono::steady_clock::now();
  while (done_ < ticket && !stop_) {
    done_wake_at_ = std::min(done_wake_at_, ticket);
    done_cv_.wait(lock);
  }
  ++stats_.fence_waits;
  stats_.fence_wait_us += Micros(start);
}

ThreadedSink::Stats ThreadedSink::stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void ThreadedSink::Run() {
  ScheduleCriticalThread();
  std::unique_lock lock(mutex_);
  for (;;) {
    if (queue_.empty() && !stop_) {
      const auto idle = std::chrono::steady_clock::now();
      work_cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
      stats_.render_idle_us += Micros(idle);
    }
    if (queue_.empty()) return;  // stopping, and nothing left
    std::unique_ptr<Command> c = std::move(queue_.front());
    queue_.pop_front();
    const bool wake_room = room_wanted_;
    room_wanted_ = false;
    lock.unlock();
    if (wake_room) room_cv_.notify_all();
    // The device as this call saw it: the copy brought up to date with the call's changes.
    if (c->device_address) {
      const uint8_t* bytes = c->device_bytes.data();
      for (uint16_t i : c->device_blocks) {
        std::memcpy(device_image_.data() + blocks_[i].first, bytes, blocks_[i].second);
        bytes += blocks_[i].second;
      }
      c->snapshot.device = device_image_.data();
      c->snapshot.device_size = uint32_t(device_image_.size());
      c->snapshot.device_address = c->device_address;
    }
    SnapshotGuestMemory::SetCurrent(&c->snapshot);
    switch (c->kind) {
      case Kind::kDevice: inner_.OnDeviceCreated(c->value0); break;
      case Kind::kDraw:
        if (!c->up_data.empty()) c->draw.up_data = c->up_data.data();
        inner_.OnDraw(c->draw);
        break;
      case Kind::kClear: inner_.OnClear(c->clear); break;
      case Kind::kResolve: inner_.OnResolve(c->resolve); break;
      case Kind::kTiling: inner_.OnTiling(c->tiling); break;
      case Kind::kPredication: inner_.OnPredication(c->predication); break;
      case Kind::kQuery: inner_.OnQuery(c->query, nullptr); break;
      case Kind::kRelease: inner_.OnShaderReleased(c->value0, c->value1); break;
      case Kind::kSwap: inner_.OnSwap(c->swap); break;
    }
    SnapshotGuestMemory::SetCurrent(nullptr);
    const bool was_present = c->kind == Kind::kSwap;
    lock.lock();
    if (free_.size() < kMaxFreeCommands) free_.push_back(std::move(c));
    if (was_present) {
      --queued_presents_;
      if (room_wanted_) room_cv_.notify_all();
    }
    ++done_;
    if (done_ >= done_wake_at_) {
      done_wake_at_ = UINT64_MAX;
      done_cv_.notify_all();
    }
  }
}

}  // namespace fcp::nr
