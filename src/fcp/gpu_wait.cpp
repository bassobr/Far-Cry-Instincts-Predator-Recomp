// GPU wait loops of the D3D library without a spinning host core.
//
// The D3D library waits for the GPU by polling guest memory: BlockOnFence (ring space and fences against the
// read-pointer write-back block, device +0x2890) and three more loops call D3D::BreakOnGpuHang once per
// round; it returns 1 while the wait goes on and 0 after a hang (2 s). On the console the poll costs one of
// six hardware threads; here it cost a whole host core while the null driver sleeps at the vblank
// (measured: render thread 84 % CPU in the menu at 30 fps).
//
// The hook runs the original unchanged. When it returns 1 and the write-back block (16 words) is the same as
// at the previous round of the same wait, the thread sleeps until the command processor reports progress
// (rex::NotifyGpuProgress, SDK change 80: after every batch with the read-pointer write-back, before every
// WAIT_REG_MEM and when it goes idle), at most FCP_GPU_WAIT_TIMEOUT_MS (default 1). The loop then checks its
// condition exactly as before; only the number of empty rounds changes.
//
//   FCP_GPU_WAIT=0                  spin as before
//   FCP_GPU_WAIT_TIMEOUT_MS=<ms>    upper bound of one sleep (default 1)
#include "fcp/gpu_wait.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/gpu_progress.h>
#include <rex/platform/thread.h>
#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"

REX_EXTERN(__imp__D3D_BreakOnGpuHang);

namespace fcp::gpuwait {
namespace {

// D3D device: pointer to the GPU write-back block (read pointer at +0, fences after it).
constexpr uint32_t kDeviceWriteBack = 0x2890;
constexpr uint32_t kBlockWords = 16;

bool g_configured = false;
bool g_enabled = true;
uint32_t g_timeout_ms = 1;

std::atomic<uint64_t> g_rounds{0}, g_sleeps{0}, g_woken{0}, g_changed{0};

struct Last {
  uint32_t state = 0;
  uint32_t block[kBlockWords] = {};
};
thread_local Last t_last;

void Configure() {
  if (g_configured) return;
  g_configured = true;
  g_enabled = fcp::env::Flag("FCP_GPU_WAIT", true);
  g_timeout_ms = uint32_t(std::max<int64_t>(1, fcp::env::Int("FCP_GPU_WAIT_TIMEOUT_MS", 1)));
  FCPLOG("gpu wait: {} (timeout {} ms)", g_enabled ? "sleep until the command processor progresses" : "spin (FCP_GPU_WAIT=0)",
         g_timeout_ms);
}

}  // namespace

void WriteSummary() {
  if (!g_configured) return;
  FCPLOG("gpu wait: {} rounds, {} slept ({} woken by progress), {} saw a changed write-back", g_rounds.load(),
         g_sleeps.load(), g_woken.load(), g_changed.load());
}

}  // namespace fcp::gpuwait

REX_HOOK_RAW(D3D_BreakOnGpuHang) {
  using namespace fcp::gpuwait;
  Configure();
  const uint32_t state = ctx.r3.u32;
  __imp__D3D_BreakOnGpuHang(ctx, base);
  if (!g_enabled) return;
  if (ctx.r3.u32 != 1) {
    t_last.state = 0;
    return;
  }
  ++g_rounds;
  const uint32_t seen = rex::GpuProgress();
  const uint32_t device = fcp::guest::U32(base, state);
  const uint32_t block_address = fcp::guest::Plausible(device) ? fcp::guest::U32(base, device + kDeviceWriteBack) : 0;
  if (!fcp::guest::Plausible(block_address)) return;
  uint32_t block[kBlockWords];
  std::memcpy(block, fcp::guest::Ptr(base, block_address), sizeof(block));
  const bool same_wait = t_last.state == state;
  const bool changed = std::memcmp(block, t_last.block, sizeof(block)) != 0;
  t_last.state = state;
  std::memcpy(t_last.block, block, sizeof(block));
  // First round of a wait, or the GPU wrote something since the previous round: let the loop look again.
  if (!same_wait) return;
  if (changed) {
    ++g_changed;
    return;
  }
  // A stop request (shutdown) ends the thread here like at every SDK call, also when the command processor
  // is already gone and no progress will come.
  if (ctx.stop_requested && ctx.stop_requested->load(std::memory_order_acquire)) throw rex::ThreadStop{};
  ++g_sleeps;
  if (rex::WaitGpuProgress(seen, g_timeout_ms)) ++g_woken;
  if (ctx.stop_requested && ctx.stop_requested->load(std::memory_order_acquire)) throw rex::ThreadStop{};
}
