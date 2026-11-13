#include "fcp/watchdog.h"

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
#include <dbghelp.h>
#include <tlhelp32.h>

#include <rex/app/app.h>
#include <rex/chrono.h>
#include <rex/kernel/thread_registry.h>
#include <rex/kernel/thread_state.h>
#include <rex/kernel/title_runtime.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/imports.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"

namespace fcp::watch {
namespace {

using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_swaps{0};
std::atomic<int64_t> g_last_swap_ns{0};
std::atomic<uint8_t*> g_base{nullptr};

struct State {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable wake;
  bool stop = false;
  rex::kernel::TitleRuntime* runtime = nullptr;
  Clock::time_point start;
};
State g_state;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

// --- host stacks ------------------------------------------------------------------------------------

constexpr int kMaxFrames = 48;

bool Readable(DWORD64 address) {
  MEMORY_BASIC_INFORMATION info{};
  if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info))) {
    return false;
  }
  constexpr DWORD kRead = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  return info.State == MEM_COMMIT && (info.Protect & kRead) && !(info.Protect & PAGE_GUARD);
}

// Walks the stack of a suspended thread. No allocation (the thread may hold the heap lock).
int Walk(CONTEXT& c, DWORD64* frames) {
  int n = 0;
  frames[n++] = c.Rip;
  while (n < kMaxFrames) {
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
    if (!fe) {
      if (!Readable(c.Rsp)) {
        break;
      }
      c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
      c.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &handler_data, &establisher,
                       nullptr);
    }
    if (c.Rip == 0) {
      break;
    }
    frames[n++] = c.Rip;
  }
  return n;
}

std::string Symbol(DWORD64 address) {
  alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
  symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
  symbol->MaxNameLen = 255;
  DWORD64 displacement = 0;
  HMODULE module = nullptr;
  char path[MAX_PATH] = {};
  std::string module_name = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(address), &module) &&
      GetModuleFileNameA(module, path, MAX_PATH)) {
    module_name = path;
    if (const size_t slash = module_name.find_last_of("\\/"); slash != std::string::npos) {
      module_name = module_name.substr(slash + 1);
    }
  }
  if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
    return std::format("{}!{}+{:#x}", module_name, symbol->Name, displacement);
  }
  return std::format("{}+{:#x}", module_name, module ? address - reinterpret_cast<DWORD64>(module) : address);
}

void DumpHostThreads() {
  static bool sym_ready = [] {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
  }();
  (void)sym_ready;
  const DWORD self = GetCurrentThreadId();
  std::vector<DWORD> ids;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID == GetCurrentProcessId() && te.th32ThreadID != self) {
        ids.push_back(te.th32ThreadID);
      }
    }
    CloseHandle(snap);
  }
  std::vector<DWORD64> frames(kMaxFrames);
  for (DWORD id : ids) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                          FALSE, id);
    if (!h) {
      continue;
    }
    std::string name;
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc) {
      for (const wchar_t* p = desc; *p; ++p) {
        name.push_back(*p < 0x80 ? char(*p) : '?');
      }
      LocalFree(desc);
    }
    int n = 0;
    if (SuspendThread(h) != DWORD(-1)) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_FULL;
      if (GetThreadContext(h, &c)) {
        n = Walk(c, frames.data());
      }
      ResumeThread(h);
    }
    CloseHandle(h);
    FCPLOG("  host thread {} \"{}\" ({} frames)", id, name, n);
    for (int i = 0; i < n; ++i) {
      FCPLOG("    #{:02} {:016X} {}", i, frames[i], Symbol(frames[i]));
    }
  }
}

// --- guest stacks -----------------------------------------------------------------------------------

void DumpGuestThreads(rex::kernel::TitleRuntime& runtime) {
  uint8_t* base = g_base.load();
  auto threads = runtime.GetThreadRegistry().All();
  FCPLOG("  guest threads: {}", threads.size());
  for (auto& pin : threads) {
    rex::kernel::KernelThread& t = *pin;
    PPCContext* ctx = t.GetThreadState() ? t.GetThreadState()->GetContext() : nullptr;
    const uint32_t host_id = t.GetThread() ? t.GetThread()->GetSystemID() : 0;
    if (!ctx) {
      FCPLOG("  guest thread {:08X} \"{}\" host {} (no context)", t.GetThreadID(), t.GetName(), host_id);
      continue;
    }
    FCPLOG("  guest thread {:08X} \"{}\" host {} guest={} running={} r1={:08X} lr={:08X} r3={:08X}",
           t.GetThreadID(), t.GetName(), host_id, t.IsGuestThread(), t.IsRunning(), ctx->r1.u32,
           static_cast<uint32_t>(ctx->lr), ctx->r3.u32);
    if (!base) {
      continue;
    }
    // Xenon ABI: back chain at 0(r1), the caller's saved LR at -8 of the caller frame.
    uint32_t frame = ctx->r1.u32;
    std::string chain;
    for (int i = 0; i < 32 && frame < 0xE0000000u && fcp::guest::Readable(base, frame, 4); ++i) {
      const uint32_t next = fcp::guest::U32(base, frame);
      if (next <= frame || !fcp::guest::Readable(base, next - 8, 8)) {
        break;
      }
      const uint32_t lr = fcp::guest::U32(base, next - 8);
      chain += std::format(" {:08X}", lr);
      frame = next;
    }
    FCPLOG("    guest chain (saved LR):{}", chain);
  }
}

void Stats(Clock::time_point now, uint64_t& last_swaps) {
  const double t = std::chrono::duration<double>(now - g_state.start).count();
  const uint64_t swaps = g_swaps.load();
  const double guest_ticks_s =
      double(rex::Clock::QueryGuestTickCount()) / double(rex::Clock::GetGuestTickFrequency());
  const uint32_t uptime_ms = rex::Clock::GetGuestUptimeMillis();
  size_t threads = 0;
  {
    std::lock_guard lock(g_state.mutex);
    if (g_state.runtime) {
      threads = g_state.runtime->GetThreadRegistry().All().size();
    }
  }
  FCPLOG(
      "stat t={:.1f}s swaps={} (+{}) guest_threads={} | time: mftb={:.3f}s uptime_ms={} freq={} | "
      "imports: VdSwap={} KeWaitForSingleObject={} NtWaitForSingleObjectEx={} RtlRaiseException={}",
      t, swaps, swaps - last_swaps, threads, guest_ticks_s, uptime_ms,
      rex::Clock::GetGuestTickFrequency(), fcp::imports::CountOf("VdSwap"),
      fcp::imports::CountOf("KeWaitForSingleObject"), fcp::imports::CountOf("NtWaitForSingleObjectEx"),
      fcp::imports::CountOf("RtlRaiseException"));
  last_swaps = swaps;
}

void Loop() {
  SetThreadDescription(GetCurrentThread(), L"fcp watchdog");
  const bool watchdog = fcp::env::Flag("FCP_WATCHDOG", true);
  const int64_t limit_s = fcp::env::Int("FCP_WATCHDOG_SECONDS", 5);
  const int64_t exit_after = fcp::env::Int("FCP_EXIT_AFTER", 0);
  const bool stats = fcp::env::Flag("FCP_STATS", true);
  const int64_t test_dump_s = fcp::env::Int("FCP_WATCHDOG_TEST", 0);  // one dump at t=s (test)
  bool test_done = false;
  int dumps = 0;
  bool shutdown_requested = false;
  Clock::time_point shutdown_at{};
  Clock::time_point next_stats = g_state.start + std::chrono::seconds(5);
  int64_t watch_from_ns = NowNs();
  uint64_t last_swaps = 0;
  for (;;) {
    {
      std::unique_lock lock(g_state.mutex);
      g_state.wake.wait_for(lock, std::chrono::milliseconds(250), [] { return g_state.stop; });
      if (g_state.stop) {
        break;
      }
    }
    const Clock::time_point now = Clock::now();
    // The app unloads the runtime during shutdown: stop touching it once shutdown is requested.
    if (rex::app::IsShutdownRequested() && !shutdown_requested) {
      shutdown_requested = true;
      shutdown_at = now;
      std::lock_guard lock(g_state.mutex);
      g_state.runtime = nullptr;
    }
    if (stats && now >= next_stats) {
      Stats(now, last_swaps);
      next_stats += std::chrono::seconds(5);
    }
    if (test_dump_s > 0 && !test_done && now - g_state.start >= std::chrono::seconds(test_dump_s)) {
      test_done = true;
      DumpAllThreads(std::format("FCP_WATCHDOG_TEST at {} s (swaps={})", test_dump_s, g_swaps.load()).c_str());
    }
    if (watchdog && dumps < 3) {
      const int64_t last = std::max(g_last_swap_ns.load(), watch_from_ns);
      if (NowNs() - last >= limit_s * 1000000000LL) {
        ++dumps;
        DumpAllThreads(std::format("watchdog {}/3: {} s without Swap (swaps={})", dumps, limit_s,
                                   g_swaps.load()).c_str());
        watch_from_ns = NowNs();
      }
    }
    // During a relaunch hand-over the new process carries on; this one ends with the hand-over.
    if (exit_after > 0 && !shutdown_requested && !fcp::relaunch::Active() &&
        now - g_state.start >= std::chrono::seconds(exit_after)) {
      FCPLOG("FCP_EXIT_AFTER={} reached, swaps={}, requesting shutdown", exit_after, g_swaps.load());
      rex::app::RequestShutdown(0);
    }
    if (shutdown_requested && now - shutdown_at >= std::chrono::seconds(15)) {
      FCPLOG("shutdown did not finish within 15 s, dumping threads and terminating");
      DumpAllThreads("shutdown hang");
      fcp::imports::WriteSummary();
      fcp::log::Drain(std::chrono::milliseconds(3000));
      TerminateProcess(GetCurrentProcess(), 3);
    }
  }
}

}  // namespace

void Start(rex::kernel::TitleRuntime& runtime) {
  if (g_state.thread.joinable()) {
    return;
  }
  g_state.runtime = &runtime;
  g_state.start = Clock::now();
  g_state.stop = false;
  g_state.thread = std::thread(Loop);
}

void Stop() {
  {
    std::lock_guard lock(g_state.mutex);
    g_state.stop = true;
  }
  g_state.wake.notify_all();
  if (g_state.thread.joinable()) {
    g_state.thread.join();
  }
  g_state.runtime = nullptr;
}

void OnSwap() {
  if (g_swaps.fetch_add(1, std::memory_order_relaxed) == 0) {
    fcp::relaunch::OnFirstSwap();  // hand-over: the new process shows its first frame
  }
  g_last_swap_ns.store(NowNs(), std::memory_order_relaxed);
}

uint64_t SwapCount() {
  return g_swaps.load(std::memory_order_relaxed);
}

void SetGuestBase(uint8_t* base) {
  uint8_t* expected = nullptr;
  g_base.compare_exchange_strong(expected, base);
}

uint8_t* GuestBase() {
  return g_base.load();
}

void DumpAllThreads(const char* reason) {
  FCPLOG("=== thread dump: {} ===", reason);
  {
    std::lock_guard lock(g_state.mutex);
    if (g_state.runtime) {
      DumpGuestThreads(*g_state.runtime);
    }
  }
  DumpHostThreads();
  FCPLOG("=== end of thread dump ===");
}

}  // namespace fcp::watch
