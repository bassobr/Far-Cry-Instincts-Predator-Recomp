#include "fcp/cpu_stats.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <windows.h>
// windows.h first
#include <tlhelp32.h>

#include <rex/app/app.h>
#include <rex/kernel/object/kernel_thread.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/thread.h>

#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/log.h"

namespace fcp::cpu {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t Ticks(const FILETIME& f) { return uint64_t(f.dwHighDateTime) << 32 | f.dwLowDateTime; }

struct ThreadInfo {
  HANDLE handle = nullptr;
  std::string name;
  uint64_t user = 0, kernel = 0;  // at the start of the window (100 ns)
  bool seen = false;
};

struct ProcessTimes {
  uint64_t user = 0, kernel = 0;
};

ProcessTimes ReadProcess() {
  FILETIME c, e, k, u;
  if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return {};
  return {Ticks(u), Ticks(k)};
}

std::string ThreadName(HANDLE h, DWORD tid) {
  PWSTR w = nullptr;
  std::string out;
  if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
    for (const wchar_t* p = w; *p && out.size() < 48; ++p) out.push_back(*p < 0x80 ? char(*p) : '?');
    LocalFree(w);
  }
  if (out.empty()) out = std::format("thread {}", tid);
  return out;
}

std::atomic<bool> g_stop{false};
std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_thread;
rex::kernel::TitleRuntime* g_runtime = nullptr;

// Totals for the shutdown line.
Clock::time_point g_t0, g_level_t0;
ProcessTimes g_p0, g_level_p0;
uint64_t g_frame0 = 0, g_level_frame0 = 0;
bool g_level_seen = false;

void Loop(double window_s) {
  SetThreadDescription(GetCurrentThread(), L"fcp cpu stats");
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  std::unordered_map<DWORD, ThreadInfo> threads;
  Clock::time_point window_start = Clock::now();
  ProcessTimes p_start = ReadProcess();
  uint64_t frame_start = fcp::gametime::Frame();

  auto scan = [&](bool report, double wall) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    for (auto& [tid, t] : threads) t.seen = false;
    // Guest threads by host id: entry point, stable across runs.
    std::unordered_map<DWORD, uint32_t> guest_entry;
    // Not once shutdown is requested: the SDK tears the thread registry down before OnShutdown stops this
    // thread (crash 0xC0000005 seen when a window ended exactly at FCP_EXIT_AFTER), like thread_profile/watchdog.
    if (g_runtime && !rex::app::IsShutdownRequested()) {
      for (auto& pin : g_runtime->GetThreadRegistry().All()) {
        rex::kernel::KernelThread& k = *pin;
        if (k.IsGuestThread() && k.GetThread()) guest_entry[DWORD(k.GetThread()->GetSystemID())] = k.GetCreationParams()->start_address;
      }
    }
    struct Row {
      double pct, user_pct;
      std::string name;
      DWORD tid;
    };
    std::vector<Row> rows;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
      auto [it, inserted] = threads.try_emplace(te.th32ThreadID);
      ThreadInfo& t = it->second;
      if (inserted) {
        t.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t.handle) continue;
      }
      if (!t.handle) continue;
      t.seen = true;
      FILETIME c, e, k, u;
      if (!GetThreadTimes(t.handle, &c, &e, &k, &u)) continue;
      // Names can change after creation (the SDK names guest threads when they start).
      t.name = ThreadName(t.handle, te.th32ThreadID);
      if (auto g = guest_entry.find(te.th32ThreadID); g != guest_entry.end()) t.name += std::format(" guest {:08X}", g->second);
      const uint64_t user = Ticks(u), kernel = Ticks(k);
      if (report && !inserted && wall > 0) {
        const double du = double(user - t.user) / 1e7, dk = double(kernel - t.kernel) / 1e7;
        const double pct = 100.0 * (du + dk) / wall;
        if (pct >= 2.0) rows.push_back({pct, 100.0 * du / wall, t.name, te.th32ThreadID});
      }
      t.user = user;
      t.kernel = kernel;
    }
    CloseHandle(snap);
    for (auto it = threads.begin(); it != threads.end();) {
      if (!it->second.seen) {
        if (it->second.handle) CloseHandle(it->second.handle);
        it = threads.erase(it);
      } else {
        ++it;
      }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    for (const Row& r : rows) {
      FCPLOG("cpu:   {:5.1f}% (user {:5.1f}%, kernel {:5.1f}%) {} [{}]", r.pct, r.user_pct, r.pct - r.user_pct,
             r.name, r.tid);
    }
  };

  scan(false, 0);
  while (true) {
    {
      std::unique_lock lock(g_mutex);
      if (g_cv.wait_for(lock, std::chrono::duration<double>(window_s), [] { return g_stop.load(); })) break;
    }
    const Clock::time_point now = Clock::now();
    const double wall = std::chrono::duration<double>(now - window_start).count();
    const ProcessTimes p = ReadProcess();
    const double du = double(p.user - p_start.user) / 1e7, dk = double(p.kernel - p_start.kernel) / 1e7;
    const uint64_t frames = fcp::gametime::Frame() - frame_start;
    if (fcp::gametime::InLevel() && !g_level_seen) {
      g_level_seen = true;
      g_level_t0 = now;
      g_level_p0 = p;
      g_level_frame0 = fcp::gametime::Frame();
    }
    FCPLOG("cpu: window {:.1f} s | process {:.1f}% of a core (user {:.1f}%, kernel {:.1f}%) | {} frames {:.1f} fps "
           "| level {} {:.1f} s",
           wall, 100.0 * (du + dk) / wall, 100.0 * du / wall, 100.0 * dk / wall, frames, frames / wall,
           fcp::gametime::InLevel() ? fcp::gametime::LevelName() : std::string("-"), fcp::gametime::LevelSeconds());
    scan(true, wall);
    window_start = now;
    p_start = p;
    frame_start = fcp::gametime::Frame();
  }
  for (auto& [tid, t] : threads)
    if (t.handle) CloseHandle(t.handle);
}

void Total(const char* what, Clock::time_point t0, const ProcessTimes& p0, uint64_t frame0) {
  const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
  const ProcessTimes p = ReadProcess();
  const double du = double(p.user - p0.user) / 1e7, dk = double(p.kernel - p0.kernel) / 1e7;
  const uint64_t frames = fcp::gametime::Frame() - frame0;
  if (wall <= 0) return;
  FCPLOG("cpu total {}: {:.1f} s | process {:.1f}% of a core (user {:.1f}%, kernel {:.1f}%) | {} frames {:.1f} fps "
         "| {:.2f} ms CPU per frame",
         what, wall, 100.0 * (du + dk) / wall, 100.0 * du / wall, 100.0 * dk / wall, frames, frames / wall,
         frames ? 1000.0 * (du + dk) / double(frames) : 0.0);
}

}  // namespace

void Start(rex::kernel::TitleRuntime& runtime) {
  const int64_t window = fcp::env::Int("FCP_CPU_STATS", 0);
  if (window <= 0 || g_thread.joinable()) return;
  FCPLOG("FCP_CPU_STATS={}: process and thread CPU every {} s", window, window);
  g_runtime = &runtime;
  g_t0 = Clock::now();
  g_p0 = ReadProcess();
  g_frame0 = fcp::gametime::Frame();
  g_stop = false;
  g_thread = std::thread(Loop, double(window));
}

void Stop() {
  if (!g_thread.joinable()) return;
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_cv.notify_all();
  g_thread.join();
  Total("run", g_t0, g_p0, g_frame0);
  if (g_level_seen) Total("level", g_level_t0, g_level_p0, g_level_frame0);
}

}  // namespace fcp::cpu
