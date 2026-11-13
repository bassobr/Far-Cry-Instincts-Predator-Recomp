// Fast readability check for fcp::guest::Readable.
#include "fcp/guest.h"

#include <atomic>
#include <cstring>
#include <string>

#include <rex/kernel/guest_memory.h>
#include <rex/kernel/title_runtime.h>

#include "fcp/env.h"
#include "fcp/log.h"

namespace fcp::guest {
namespace {

// 0 = VirtualQuery only, 1 = guest page table first, 2 = both and compare
int Mode() {
  static const int mode = [] {
    const std::string v = fcp::env::Get("FCP_GUEST_READABLE_VQ");
    if (v == "verify") return 2;
    return fcp::env::Flag("FCP_GUEST_READABLE_VQ") ? 0 : 1;
  }();
  return mode;
}

std::atomic<uint64_t> g_checks{0}, g_mismatch{0}, g_table_no{0};

}  // namespace

bool FastReadable() { return Mode() != 0; }

bool VerifyReadableOn() { return Mode() == 2; }

bool GuestTableReadable(uint32_t address, size_t size) {
  if (size > 0xFFFFFFFFu) {
    return false;
  }
  rex::kernel::GuestMemory* memory = REX_GUEST_MEMORY();
  if (!memory) {
    return false;
  }
  // GetReadOnlySpan checks heap, heap end and the read bit of every guest page under the heap lock.
  return memory->GetReadOnlySpan(address, uint32_t(size)).has_value();
}

bool VerifyReadable(uint8_t* base, uint32_t address, size_t size) {
  const bool table = GuestTableReadable(address, size);
  const bool host = HostReadable(base, address, size);
  const uint64_t n = g_checks.fetch_add(1, std::memory_order_relaxed) + 1;
  // Readable answers like the fast path: true when the table says yes, else VirtualQuery.
  const bool fast = table || host;
  if (!table) g_table_no.fetch_add(1, std::memory_order_relaxed);
  if (fast != host) {
    const uint64_t m = g_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
    if (m <= 20) {
      FCPLOG("guest readable verify: mismatch #{} at {:08X} size {}: table {} VirtualQuery {}", m, address, size,
             table, host);
    }
  }
  if ((n & (n - 1)) == 0 && n >= 1024) {
    FCPLOG("guest readable verify: {} checks, {} mismatches, {} not readable in the table", n,
           g_mismatch.load(std::memory_order_relaxed), g_table_no.load(std::memory_order_relaxed));
  }
  return host;
}

void SelfTestGuardedString();

bool GuardedStrings() {
  static const bool on = [] {
    SelfTestGuardedString();
    return !fcp::env::Flag("FCP_GUEST_STRING_UNGUARDED");
  }();
  return on;
}

// No C++ objects in this function (__try). Readable can say yes for a page that is gone a moment later or that the
// host never committed; a guarded copy keeps diagnostic string reads from taking the process down (one crash seen in
// Game_DispatchCommands with the settings menu open).
//
// clang-cl without -fasync-exceptions only covers faults inside calls made from a __try body, not faulting
// instructions in the body itself: the copy loop inline in the __try was not protected (measured: crash report
// "unhandled title read" at the loop in CopyGuestString, soak test 2026-11-13). The loop therefore lives in its own
// function; it reports its progress through `done`. FCP_GUEST_STRING_INLINE_TRY=1 = the old inline form.
__declspec(noinline) static void CopyGuestStringLoop(const uint8_t* src, char* dst, size_t max, volatile size_t* done) {
  for (size_t n = 0; n < max; ++n) {
    const uint8_t c = src[n];
    if (!c) return;
    dst[n] = static_cast<char>(c);
    *done = n + 1;
  }
}

static size_t CopyGuestStringInline(const uint8_t* src, char* dst, size_t max) {
  size_t n = 0;
  __try {
    for (; n < max && src[n]; ++n) {
      dst[n] = static_cast<char>(src[n]);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return n;
  }
  return n;
}

size_t CopyGuestString(const uint8_t* src, char* dst, size_t max) {
  static const bool inline_try = fcp::env::Flag("FCP_GUEST_STRING_INLINE_TRY");
  if (inline_try) return CopyGuestStringInline(src, dst, max);
  volatile size_t done = 0;
  __try {
    CopyGuestStringLoop(src, dst, max, &done);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return done;  // what was readable before the fault
  }
  return done;
}

// FCP_TEST_GUARDED_STRING=1: at the first guarded read, copy from a reserved, uncommitted host page (8 readable bytes
// before it) and log what came back; without a working guard the process ends here with a crash report.
void SelfTestGuardedString() {
  if (!fcp::env::Flag("FCP_TEST_GUARDED_STRING")) return;
  auto* block = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * 4096, MEM_RESERVE, PAGE_NOACCESS));
  if (!block || !VirtualAlloc(block, 4096, MEM_COMMIT, PAGE_READWRITE)) {
    FCPLOG("FCP_TEST_GUARDED_STRING: no test memory");
    return;
  }
  std::memset(block + 4096 - 8, 'x', 8);
  char out[64];
  const size_t n = CopyGuestString(block + 4096 - 8, out, sizeof(out));
  FCPLOG("FCP_TEST_GUARDED_STRING: copy across an uncommitted page returned {} bytes (expected 8), no crash", n);
  VirtualFree(block, 0, MEM_RELEASE);
}

}  // namespace fcp::guest
