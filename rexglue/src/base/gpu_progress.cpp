// FCP local change 80 (UPSTREAM.md): progress counter of the command processor for title-side waits.
#include <rex/gpu_progress.h>

#include <atomic>
#include <chrono>
#include <thread>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

namespace rex {

namespace {
std::atomic<u32> g_progress{0};
std::atomic<u32> g_waiters{0};
}  // namespace

u32 GpuProgress() {
  return g_progress.load(std::memory_order_acquire);
}

void NotifyGpuProgress() {
  g_progress.fetch_add(1);  // seq_cst with the waiter count (Dekker)
  if (g_waiters.load() != 0) {
#if REX_PLATFORM_WIN32
    WakeByAddressAll(&g_progress);
#else
    g_progress.notify_all();
#endif
  }
}

bool WaitGpuProgress(u32 seen, u32 timeout_ms) {
  g_waiters.fetch_add(1);
  bool changed = g_progress.load() != seen;
  if (!changed) {
#if REX_PLATFORM_WIN32
    WaitOnAddress(&g_progress, &seen, sizeof(seen), timeout_ms);
#else
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (g_progress.load(std::memory_order_acquire) == seen && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
#endif
    changed = g_progress.load(std::memory_order_acquire) != seen;
  }
  g_waiters.fetch_sub(1, std::memory_order_acq_rel);
  return changed;
}

}  // namespace rex
