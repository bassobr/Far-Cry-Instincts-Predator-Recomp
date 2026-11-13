#include "fcp/log.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include <windows.h>

#include <rex/logging/logger.h>

namespace fcp::log {
namespace {

struct Entry {
  int64_t qpc;
  uint32_t tid;
  std::string text;
};

constexpr size_t kMaxQueued = 1u << 20;

struct State {
  std::mutex mutex;
  std::condition_variable wake;
  std::condition_variable drained;
  std::vector<Entry> queue;
  uint64_t queued_seq = 0;   // entries ever queued
  uint64_t written_seq = 0;  // entries ever handed to the file
  bool stop = false;
  std::thread writer;
  FILE* file = nullptr;
  std::filesystem::path folder;
  int64_t qpc_start = 0;
  int64_t qpc_freq = 1;
  std::atomic<uint64_t> dropped{0};
  std::atomic<bool> running{false};
};

State& S() {
  static State* state = new State();  // never destroyed: lines may arrive during static teardown
  return *state;
}

int64_t Qpc() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

void WriterLoop() {
  State& s = S();
  std::vector<Entry> batch;
  SetThreadDescription(GetCurrentThread(), L"fcp log writer");
  for (;;) {
    {
      std::unique_lock lock(s.mutex);
      s.wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return s.stop || !s.queue.empty(); });
      batch.swap(s.queue);
      if (batch.empty() && s.stop) {
        break;
      }
    }
    std::string out;
    out.reserve(batch.size() * 96);
    for (const Entry& e : batch) {
      const double t = double(e.qpc - s.qpc_start) / double(s.qpc_freq);
      char prefix[48];
      const int n = std::snprintf(prefix, sizeof(prefix), "%10.3f %6u ", t, e.tid);
      out.append(prefix, n > 0 ? size_t(n) : 0);
      out.append(e.text);
      out.push_back('\n');
    }
    if (s.file && !out.empty()) {
      std::fwrite(out.data(), 1, out.size(), s.file);
      std::fflush(s.file);  // writer thread only
    }
    {
      std::lock_guard lock(s.mutex);
      s.written_seq += batch.size();
    }
    s.drained.notify_all();
    batch.clear();
  }
}

class SdkSink final : public rex::log::ILogSink {
 public:
  void Write(const rex::log::Message& message) override {
    std::string text;
    text.reserve(message.text.size() + message.category.size() + 8);
    text.push_back(rex::log::LevelLetter(message.level));
    text.append(" [");
    text.append(message.category);
    text.append("] ");
    text.append(message.text);
    Line(text);
  }
  // The SDK flushes at shutdown and from its crash handler: wait for the writer then.
  void Flush() override { Drain(std::chrono::milliseconds(500)); }
};

SdkSink g_sdk_sink;
rex::ScopeGuard g_sdk_guard;

}  // namespace

bool Start(const std::filesystem::path& folder, std::string_view name) {
  State& s = S();
  if (s.running) {
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const std::filesystem::path path = folder / std::string(name);
  std::filesystem::path prev = path;
  prev += ".prev";
  std::filesystem::rename(path, prev, ec);
  s.file = _wfopen(path.c_str(), L"wb");
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  s.qpc_freq = f.QuadPart;
  s.qpc_start = Qpc();
  s.folder = folder / "logs";  // dumps and traces
  std::filesystem::create_directories(s.folder, ec);
  s.running = true;
  s.writer = std::thread(WriterLoop);
  return s.file != nullptr;
}

const std::filesystem::path& Folder() {
  return S().folder;
}

void Line(std::string_view text) {
  State& s = S();
  Entry e{Qpc(), GetCurrentThreadId(), std::string(text)};
  {
    std::lock_guard lock(s.mutex);
    if (s.queue.size() >= kMaxQueued) {
      s.dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    s.queue.push_back(std::move(e));
    ++s.queued_seq;
  }
  s.wake.notify_one();
}

void Drain(std::chrono::milliseconds timeout) {
  State& s = S();
  if (!s.running) {
    return;
  }
  std::unique_lock lock(s.mutex);
  const uint64_t target = s.queued_seq;
  s.wake.notify_one();
  s.drained.wait_for(lock, timeout, [&] { return s.written_seq >= target; });
}

void Stop() {
  State& s = S();
  if (!s.running.exchange(false)) {
    return;
  }
  g_sdk_guard = {};
  {
    std::lock_guard lock(s.mutex);
    s.stop = true;
  }
  s.wake.notify_one();
  if (s.writer.joinable()) {
    s.writer.join();
  }
  if (s.file) {
    std::fclose(s.file);
    s.file = nullptr;
  }
}

void AttachSdkLogger() {
  g_sdk_guard = rex::GetLogger().AddSink(g_sdk_sink);
}

uint64_t LinesWritten() {
  std::lock_guard lock(S().mutex);
  return S().written_seq;
}

uint64_t LinesDropped() {
  return S().dropped.load();
}

}  // namespace fcp::log
