#include "fcp/file_patch.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/log.h"

REX_EXTERN(__imp__Archive_Decompress);

namespace fcp::file_patch {
namespace {

struct Patch {
  std::string name;  // FCP_TEST_PATCH, FCP_TEST_PATCH2 ...
  std::string from;
  std::string to;
  std::atomic<uint64_t> hits{0};
};

std::vector<Patch*> g_patches;
bool g_raw = false;
std::atomic<uint64_t> g_buffers{0};

void Apply(uint8_t* base, uint32_t address, uint32_t size, const char* where) {
  if (g_patches.empty() || size == 0 || !fcp::guest::Readable(base, address, size)) {
    return;
  }
  g_buffers.fetch_add(1, std::memory_order_relaxed);
  uint8_t* data = fcp::guest::Ptr(base, address);
  for (Patch* p : g_patches) {
    const size_t n = p->from.size();
    uint8_t* at = data;
    uint8_t* end = data + size;
    while (true) {
      at = std::search(at, end, p->from.begin(), p->from.end());
      if (at == end) {
        break;
      }
      std::memcpy(at, p->to.data(), n);
      const uint64_t hit = p->hits.fetch_add(1, std::memory_order_relaxed) + 1;
      if (hit <= 20) {
        FCPLOG("file patch: {} replaced at {:08X}+{:#x} ({} buffer of {} bytes, wall {:.2f} s, frame {})", p->name,
               address, uint32_t(at - data), where, size, fcp::gametime::WallSeconds(), fcp::gametime::Frame());
      }
      at += n;
    }
  }
}

}  // namespace

void Configure() {
  for (int i = 1; i <= 9; ++i) {
    const std::string name = i == 1 ? std::string("FCP_TEST_PATCH") : std::format("FCP_TEST_PATCH{}", i);
    const std::string value = fcp::env::Get(name.c_str());
    if (value.empty()) {
      continue;
    }
    const size_t bar = value.find('|');
    if (bar == std::string::npos || bar == 0 || value.size() - bar - 1 != bar) {
      FCPLOG("{}: '{}' ignored (<old>|<new>, both of the same length)", name, value);
      continue;
    }
    auto* p = new Patch{name, value.substr(0, bar), value.substr(bar + 1)};
    g_patches.push_back(p);
    FCPLOG("{}: '{}' -> '{}' ({} bytes) in decompressed archive entries{}", name, p->from, p->to, p->from.size(),
           fcp::env::Flag("FCP_TEST_PATCH_RAW") ? " and raw reads" : "");
  }
  g_raw = !g_patches.empty() && fcp::env::Flag("FCP_TEST_PATCH_RAW");
}

bool PatchRawReads() {
  return g_raw;
}

void OnRead(uint8_t* base, uint32_t buffer, uint32_t size) {
  if (g_raw) {
    Apply(base, buffer, size, "NtReadFile");
  }
}

void WriteSummary() {
  for (const Patch* p : g_patches) {
    FCPLOG("file patch: {} '{}' replaced {} times ({} buffers checked)", p->name, p->from, p->hits.load(),
           g_buffers.load());
  }
}

}  // namespace fcp::file_patch

// Archive_Decompress(src r3, src size r4, dst r5, ...): src = 1 flag byte + LZMA "alone" header (5 bytes of
// properties, u64 size little endian at +6) + stream; returns 0 when the entry was unpacked into dst.
REX_HOOK_RAW(Archive_Decompress) {
  const uint32_t src = ctx.r3.u32;
  const uint32_t dst = ctx.r5.u32;
  uint32_t size = 0;
  const bool patch = !fcp::file_patch::g_patches.empty() && fcp::guest::Readable(base, src, 14);
  if (patch) {
    const uint8_t* h = fcp::guest::Ptr(base, src);
    size = uint32_t(h[6]) | uint32_t(h[7]) << 8 | uint32_t(h[8]) << 16 | uint32_t(h[9]) << 24;
  }
  __imp__Archive_Decompress(ctx, base);
  if (patch && ctx.r3.u32 == 0) {
    fcp::file_patch::Apply(base, dst, size, "decompressed");
  }
}
