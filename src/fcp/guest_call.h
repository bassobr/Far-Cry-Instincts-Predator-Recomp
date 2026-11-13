// Calls a recompiled guest function from inside a hook, on the hooked guest thread.
//
// Only for hooks that run on a guest thread with a valid context (engine hooks). The caller's volatile
// registers (r0, r2-r12, lr, ctr, xer, cr) are restored afterwards; r13-r31 and f14-f31 are saved by
// the callee per the Xenon ABI. An optional string goes into a scratch frame below the current stack
// pointer, which the callee's own frames do not reach (they grow below the lowered r1).
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

#include <rex/ppc/context.h>

#include "fcp/guest.h"

namespace fcp::guest {

struct CallArgs {
  uint32_t r3 = 0;
  uint32_t r4 = 0;
  uint32_t r5 = 0;
  uint32_t r6 = 0;
  std::string_view string_r4;  // non-empty: copied to the scratch frame, r4 points at it
  bool zero_r4_r5 = false;     // r4 -> a zero u32, r5 -> an empty string, both in the scratch frame
  const double* f1 = nullptr;  // non-null: f1 for the call (the caller's f1 is restored afterwards)
};

/** Calls f(r3, r4, r5) and returns the guest r3. */
inline uint32_t Call(PPCContext& ctx, uint8_t* base, PPCFunc& f, const CallArgs& args) {
  const PPCContext saved = ctx;
  constexpr uint32_t kScratch = 0x400;  // frame size, 16-byte aligned
  constexpr uint32_t kStringAt = 0x100; // string offset inside the frame (above the callee's frames)
  ctx.r1.u64 = (saved.r1.u32 - kScratch) & ~0xFull;
  // Back chain, so stack walks (watchdog dumps) stay intact.
  const uint32_t chain_be = __builtin_bswap32(saved.r1.u32);
  std::memcpy(Ptr(base, ctx.r1.u32), &chain_be, 4);
  ctx.r3.u64 = args.r3;
  ctx.r4.u64 = args.r4;
  ctx.r5.u64 = args.r5;
  ctx.r6.u64 = args.r6;
  if (args.zero_r4_r5) {
    std::memset(Ptr(base, ctx.r1.u32 + kStringAt), 0, 0x40);
    ctx.r4.u64 = ctx.r1.u32 + kStringAt;
    ctx.r5.u64 = ctx.r1.u32 + kStringAt + 0x20;
  }
  if (!args.string_r4.empty()) {
    const size_t n = args.string_r4.size() < kScratch - kStringAt - 1 ? args.string_r4.size()
                                                                       : kScratch - kStringAt - 1;
    uint8_t* dst = Ptr(base, ctx.r1.u32 + kStringAt);
    std::memcpy(dst, args.string_r4.data(), n);
    dst[n] = 0;
    ctx.r4.u64 = ctx.r1.u32 + kStringAt;
  }
  if (args.f1) {
    ctx.f1.f64 = *args.f1;
  }
  f(ctx, base);
  const uint32_t result = ctx.r3.u32;
  if (args.f1) {
    ctx.f1 = saved.f1;
  }
  // Restore what the caller of the hook may still rely on.
  ctx.r0 = saved.r0;
  ctx.r1 = saved.r1;
  ctx.r2 = saved.r2;
  ctx.r3 = saved.r3;
  ctx.r4 = saved.r4;
  ctx.r5 = saved.r5;
  ctx.r6 = saved.r6;
  ctx.r7 = saved.r7;
  ctx.r8 = saved.r8;
  ctx.r9 = saved.r9;
  ctx.r10 = saved.r10;
  ctx.r11 = saved.r11;
  ctx.r12 = saved.r12;
  ctx.lr = saved.lr;
  ctx.ctr = saved.ctr;
  ctx.xer = saved.xer;
  ctx.cr0 = saved.cr0;
  ctx.cr1 = saved.cr1;
  ctx.cr2 = saved.cr2;
  ctx.cr3 = saved.cr3;
  ctx.cr4 = saved.cr4;
  ctx.cr5 = saved.cr5;
  ctx.cr6 = saved.cr6;
  ctx.cr7 = saved.cr7;
  return result;
}

}  // namespace fcp::guest
