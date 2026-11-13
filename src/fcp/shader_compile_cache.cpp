#include "fcp/shader_compile_cache.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hash.h>
#include <rex/platform/file.h>
#include <rex/ppc/hook.h>
#include <rex/ppc/recompiled_code.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/guest_call.h"
#include "fcp/log.h"
#include "fcp/user_paths.h"
#include "fcp/watchdog.h"

// Game functions (names and roles in config/predator/default.toml, [function_names]).
REX_EXTERN(__imp__D3DX_CreateBuffer);              // (size, &buffer) -> HRESULT
REX_EXTERN(__imp__XMemAlloc);                      // (size, attributes) -> pointer
REX_EXTERN(__imp__D3DX_ConstantTable_Construct);   // (this)
REX_EXTERN(__imp__D3DX_ConstantTable_Init);        // (this, ctab blob, size, flag) -> HRESULT

namespace fcp::shadercache {
namespace {

using Clock = std::chrono::steady_clock;

// Engine_CompileShader arguments (disassembly of the wrapper, see the report): r3 &buffer, r4 &constant
// table (may be 0), r6 entry point (0 = assemble), r7 profile, r8 source, r9 source length, r10 file path
// (include base), stack +0x54 error string object, +0x5C define pairs {name, value} up to name 0.
constexpr uint32_t kStackDefines = 0x5C;
// Flags and attributes the wrapper and the D3DX constant table use (fixed in the game code).
constexpr uint32_t kCtabSize = 0x20;
constexpr uint32_t kCtabAlloc = 0x24810000;
// ID3DXBuffer / ID3DXConstantTable vtable slots.
constexpr uint32_t kRelease = 0x8, kGetPointer = 0xC, kGetSize = 0x10;

constexpr char kMagic[8] = {'F', 'C', 'P', 'H', 'L', 'S', 'L', '1'};

enum class Mode { kOff, kOn, kMemory, kVerify };

struct Entry {
  std::vector<uint8_t> code;
  bool has_ctab = false;
  std::vector<uint8_t> ctab;
};

Mode g_mode = Mode::kOn;
bool g_configured = false;
std::filesystem::path g_dir;
std::mutex g_mutex;
std::unordered_map<std::string, Entry> g_memory;

std::atomic<uint64_t> g_hits{0}, g_misses{0}, g_stored{0}, g_fallbacks{0}, g_uncacheable{0};
std::atomic<uint64_t> g_verify_equal{0}, g_verify_diff{0}, g_verify_words{0}, g_verify_pointer_words{0};
std::atomic<uint64_t> g_hit_us{0}, g_miss_us{0};

void LoadPack();

void Configure() {
  if (g_configured) return;
  g_configured = true;
  const std::string v = fcp::env::Get("FCP_SHADER_COMPILE_CACHE");
  if (v == "0" || v == "off" || v == "false") g_mode = Mode::kOff;
  else if (v == "verify") g_mode = Mode::kVerify;
  else if (v == "memory") g_mode = Mode::kMemory;
  else g_mode = Mode::kOn;
  const std::string dir = fcp::env::Get("FCP_SHADER_COMPILE_CACHE_DIR");
  g_dir = dir.empty() ? fcp::paths::Cache() / "hlsl" : std::filesystem::path(dir);  // user folder (fcp/user_paths.h)
  if (g_mode == Mode::kOn || g_mode == Mode::kVerify) {
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    LoadPack();
  }
  FCPLOG("shader compile cache: {} ({})",
         g_mode == Mode::kOff ? "off" : g_mode == Mode::kVerify ? "verify" : g_mode == Mode::kMemory ? "memory" : "on",
         g_mode == Mode::kMemory || g_mode == Mode::kOff ? std::string("-") : g_dir.string());
}

void PutU32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t be = __builtin_bswap32(value);
  std::memcpy(fcp::guest::Ptr(base, address), &be, 4);
}

// NUL terminated guest string, bounded; false when the pointer is implausible or unterminated.
bool ReadString(uint8_t* base, uint32_t address, std::string& out, size_t max = 4096) {
  out.clear();
  if (!fcp::guest::Plausible(address)) return false;
  const char* p = reinterpret_cast<const char*>(fcp::guest::Ptr(base, address));
  for (size_t i = 0; i < max; ++i) {
    if (!p[i]) return true;
    out.push_back(p[i]);
  }
  return false;
}

uint32_t CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot) {
  const uint32_t vtable = fcp::guest::U32(base, object);
  PPCFunc* f = rex::ppc::ResolveFunction(fcp::guest::U32(base, vtable + slot));
  if (!f) return 0;
  return fcp::guest::Call(ctx, base, *f, {.r3 = object});
}

// Bytes of a D3DX buffer or constant table through its interface (GetBufferPointer/GetBufferSize).
bool ReadBuffer(PPCContext& ctx, uint8_t* base, uint32_t object, std::vector<uint8_t>& out) {
  if (!fcp::guest::Plausible(object)) return false;
  const uint32_t data = CallVirtual(ctx, base, object, kGetPointer);
  const uint32_t size = CallVirtual(ctx, base, object, kGetSize);
  if (size && !fcp::guest::Plausible(data)) return false;
  if (size > (64u << 20)) return false;
  out.assign(fcp::guest::Ptr(base, data), fcp::guest::Ptr(base, data) + size);
  return true;
}

struct Call {
  uint32_t pp_buffer = 0, pp_ctab = 0;
  bool compile = false;
  std::string entry, profile, path;
  std::string key;
  std::string label;
};

// Key material, or false when an input cannot be read (then the original runs uncached).
bool BuildKey(const PPCContext& ctx, uint8_t* base, Call& c) {
  c.pp_buffer = ctx.r3.u32;
  c.pp_ctab = ctx.r4.u32;
  c.compile = ctx.r6.u32 != 0;
  if (!fcp::guest::Plausible(c.pp_buffer)) return false;
  if (c.compile && (!ReadString(base, ctx.r6.u32, c.entry) || !ReadString(base, ctx.r7.u32, c.profile))) return false;
  if (!ReadString(base, ctx.r10.u32, c.path)) return false;
  const uint32_t src = ctx.r8.u32, len = ctx.r9.u32;
  if (!fcp::guest::Plausible(src) || len > (16u << 20)) return false;
  std::string m;
  m.reserve(len + 512);
  m.append("fcp-hlsl-1|flags 200|FC_XENON|");
  m.append(c.compile ? "compile|" : "assemble|");
  m.append(c.entry).push_back('\0');
  m.append(c.profile).push_back('\0');
  m.append(c.path).push_back('\0');
  m.append(std::to_string(len)).push_back('\0');
  m.append(reinterpret_cast<const char*>(fcp::guest::Ptr(base, src)), len);
  // Define pairs exactly as the wrapper copies them: while the array and its current name are non-zero.
  uint32_t defines = fcp::guest::U32(base, ctx.r1.u32 + kStackDefines);
  if (defines) {
    if (!fcp::guest::Plausible(defines)) return false;
    for (int i = 0; i < 256; ++i) {
      const uint32_t name = fcp::guest::U32(base, defines + 8 * i);
      if (!name) break;
      const uint32_t value = fcp::guest::U32(base, defines + 8 * i + 4);
      std::string n, v;
      if (!ReadString(base, name, n)) return false;
      m.append("|D").append(n).push_back('\0');
      if (value) {
        if (!ReadString(base, value, v)) return false;
        m.append(v).push_back('\0');
      } else {
        m.append("<null>").push_back('\0');
      }
    }
  }
  c.key = rex::HashBytes(m);
  c.label = c.compile ? std::format("{} {} {}", c.entry, c.profile, c.path) : std::format("asm {}", c.path);
  return true;
}

// One pack file (<dir>\hlsl.bin): "FCPHLSL1", then records {"FCPR", 32-char key, u32 code size, u32 has ctab,
// u32 ctab size, code, ctab}. Read once at start; a new entry is appended with a single WriteFile on a handle
// opened with FILE_APPEND_DATA, so two processes never interleave inside a record. A damaged tail ends the
// read (the rest of the file is ignored).
constexpr char kRecord[4] = {'F', 'C', 'P', 'R'};
constexpr size_t kKeyChars = 32;

std::filesystem::path PackPath() { return g_dir / "hlsl.bin"; }

void LoadPack() {
  std::ifstream f(PackPath(), std::ios::binary);
  if (!f) return;
  std::vector<char> all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t at = 0, records = 0;
  auto take = [&](void* out, size_t n) {
    if (at + n > all.size()) return false;
    std::memcpy(out, all.data() + at, n);
    at += n;
    return true;
  };
  char magic[8];
  if (!take(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) {
    FCPLOG("shader compile cache: {} has no valid header, ignored", PackPath().string());
    return;
  }
  while (at < all.size()) {
    char rec[4], key[kKeyChars];
    uint32_t code_size = 0, has_ctab = 0, ctab_size = 0;
    if (!take(rec, 4) || std::memcmp(rec, kRecord, 4) != 0 || !take(key, kKeyChars) || !take(&code_size, 4) ||
        !take(&has_ctab, 4) || !take(&ctab_size, 4) || at + size_t(code_size) + ctab_size > all.size())
      break;
    Entry e;
    e.code.assign(all.data() + at, all.data() + at + code_size);
    at += code_size;
    e.ctab.assign(all.data() + at, all.data() + at + ctab_size);
    at += ctab_size;
    e.has_ctab = has_ctab != 0;
    g_memory[std::string(key, kKeyChars)] = std::move(e);
    ++records;
  }
  FCPLOG("shader compile cache: {} entries from {} ({} bytes{})", g_memory.size(), PackPath().string(), all.size(),
         at < all.size() ? ", damaged tail ignored" : "");
}

bool Load(const std::string& key, Entry& e) {
  std::lock_guard lock(g_mutex);
  auto it = g_memory.find(key);
  if (it == g_memory.end()) return false;
  e = it->second;
  return true;
}

void Store(const std::string& key, const Entry& e) {
  {
    std::lock_guard lock(g_mutex);
    if (!g_memory.emplace(key, e).second) return;
  }
  ++g_stored;
  if (g_mode == Mode::kMemory || key.size() != kKeyChars) return;
  std::string rec;
  const uint32_t code_size = uint32_t(e.code.size()), has_ctab = e.has_ctab ? 1 : 0, ctab_size = uint32_t(e.ctab.size());
  rec.append(kRecord, 4).append(key);
  rec.append(reinterpret_cast<const char*>(&code_size), 4);
  rec.append(reinterpret_cast<const char*>(&has_ctab), 4);
  rec.append(reinterpret_cast<const char*>(&ctab_size), 4);
  rec.append(reinterpret_cast<const char*>(e.code.data()), code_size);
  rec.append(reinterpret_cast<const char*>(e.ctab.data()), ctab_size);
  HANDLE h = CreateFileW(PackPath().wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  LARGE_INTEGER size{};
  DWORD written = 0;
  if (GetFileSizeEx(h, &size) && size.QuadPart == 0) WriteFile(h, kMagic, 8, &written, nullptr);
  WriteFile(h, rec.data(), DWORD(rec.size()), &written, nullptr);
  CloseHandle(h);
}
// Result of a successful original: buffer bytes, constant table blob.
bool Capture(PPCContext& ctx, uint8_t* base, const Call& c, Entry& e) {
  const uint32_t buffer = fcp::guest::U32(base, c.pp_buffer);
  if (!buffer || !ReadBuffer(ctx, base, buffer, e.code)) return false;
  e.has_ctab = false;
  e.ctab.clear();
  if (c.compile && c.pp_ctab) {
    const uint32_t ctab = fcp::guest::U32(base, c.pp_ctab);
    if (ctab) {
      if (!ReadBuffer(ctx, base, ctab, e.ctab)) return false;
      e.has_ctab = true;
    }
  }
  return true;
}

void Release(PPCContext& ctx, uint8_t* base, uint32_t object) {
  if (object) CallVirtual(ctx, base, object, kRelease);
}

// Builds the original's objects from cached bytes: *out_buffer = D3DX buffer with the microcode,
// *out_ctab (when given) = constant table built from the blob like the compiler does (constructor plus
// initializer with flag 0). False when a game function failed; nothing is left allocated then.
bool Build(PPCContext& ctx, uint8_t* base, const Entry& e, uint32_t out_buffer, uint32_t out_ctab) {
  PutU32(base, out_buffer, 0);
  uint32_t hr = fcp::guest::Call(ctx, base, __imp__D3DX_CreateBuffer, {.r3 = uint32_t(e.code.size()), .r4 = out_buffer});
  const uint32_t buffer = fcp::guest::U32(base, out_buffer);
  if (int32_t(hr) < 0 || !buffer) return false;
  const uint32_t data = CallVirtual(ctx, base, buffer, kGetPointer);
  if (CallVirtual(ctx, base, buffer, kGetSize) != e.code.size() || (!e.code.empty() && !fcp::guest::Plausible(data))) {
    Release(ctx, base, buffer);
    PutU32(base, out_buffer, 0);
    return false;
  }
  if (!e.code.empty()) std::memcpy(fcp::guest::Ptr(base, data), e.code.data(), e.code.size());
  if (!out_ctab) return true;
  PutU32(base, out_ctab, 0);
  if (!e.has_ctab) return true;
  // The initializer copies the blob; a temporary D3DX buffer holds it in guest memory meanwhile.
  bool ok = false;
  hr = fcp::guest::Call(ctx, base, __imp__D3DX_CreateBuffer, {.r3 = uint32_t(e.ctab.size()), .r4 = out_ctab});
  const uint32_t temp = fcp::guest::U32(base, out_ctab);
  PutU32(base, out_ctab, 0);
  if (int32_t(hr) >= 0 && temp) {
    const uint32_t temp_data = CallVirtual(ctx, base, temp, kGetPointer);
    if (fcp::guest::Plausible(temp_data)) {
      std::memcpy(fcp::guest::Ptr(base, temp_data), e.ctab.data(), e.ctab.size());
      const uint32_t ctab = fcp::guest::Call(ctx, base, __imp__XMemAlloc, {.r3 = kCtabSize, .r4 = kCtabAlloc});
      if (ctab) {
        fcp::guest::Call(ctx, base, __imp__D3DX_ConstantTable_Construct, {.r3 = ctab});
        hr = fcp::guest::Call(ctx, base, __imp__D3DX_ConstantTable_Init,
                              {.r3 = ctab, .r4 = temp_data, .r5 = uint32_t(e.ctab.size()), .r6 = 0});
        if (int32_t(hr) >= 0) {
          PutU32(base, out_ctab, ctab);
          ok = true;
        } else {
          Release(ctx, base, ctab);
        }
      }
    }
    Release(ctx, base, temp);
  }
  if (!ok) {
    Release(ctx, base, buffer);
    PutU32(base, out_buffer, 0);
  }
  return ok;
}

// Field comparison of two constant tables (verify mode): vtable, reference count, blob size and bytes,
// header offset into the blob, constant count.
std::string CompareCtab(uint8_t* base, uint32_t a, uint32_t b) {
  using fcp::guest::U32;
  std::string diff;
  auto field = [&](const char* name, uint32_t va, uint32_t vb) {
    if (va != vb) diff += std::format(" {} {:X}/{:X}", name, va, vb);
  };
  field("vtable", U32(base, a), U32(base, b));
  field("refs", U32(base, a + 4), U32(base, b + 4));
  field("size", U32(base, a + 0xC), U32(base, b + 0xC));
  field("header", U32(base, a + 0x10) - U32(base, a + 8), U32(base, b + 0x10) - U32(base, b + 8));
  field("count", U32(base, a + 0x18), U32(base, b + 0x18));
  const uint32_t size = U32(base, a + 0xC);
  if (size == U32(base, b + 0xC) && size < (64u << 20) &&
      std::memcmp(fcp::guest::Ptr(base, U32(base, a + 8)), fcp::guest::Ptr(base, U32(base, b + 8)), size) != 0)
    diff += " blob";
  // Constant objects (0x44 bytes each, built by the initializer): word by word. A word that differs is
  // accepted when both point into their own blob copy at the same offset, or both are heap pointers
  // (other allocations of the same table); every other difference is reported.
  const uint32_t count = U32(base, a + 0x18);
  if (count == U32(base, b + 0x18) && count < 4096) {
    const uint32_t blob_a = U32(base, a + 8), blob_b = U32(base, b + 8);
    auto heap = [](uint32_t v) { return (v >= 0x40000000u && v < 0x80000000u) || v >= 0xA0000000u; };
    uint32_t words = 0, pointers = 0;
    for (uint32_t i = 0; i < count && diff.size() < 200; ++i) {
      const uint32_t ca = U32(base, U32(base, a + 0x14) + 4 * i), cb = U32(base, U32(base, b + 0x14) + 4 * i);
      if (!ca || !cb) {
        diff += std::format(" constant{} missing", i);
        break;
      }
      for (uint32_t w = 0; w < 0x44; w += 4) {
        const uint32_t va = U32(base, ca + w), vb = U32(base, cb + w);
        ++words;
        if (va == vb) continue;
        if (va - blob_a < size && vb - blob_b < size && va - blob_a == vb - blob_b) continue;
        if (heap(va) && heap(vb)) {
          ++pointers;
          continue;
        }
        diff += std::format(" constant{}+{:X} {:X}/{:X}", i, w, va, vb);
      }
    }
    g_verify_words += words;
    g_verify_pointer_words += pointers;
  }  return diff;
}

void Verify(PPCContext& ctx, uint8_t* base, const Call& c, const Entry& fresh, const Entry& cached) {
  std::string diff;
  if (fresh.code != cached.code) diff += std::format(" microcode ({} vs {} bytes)", fresh.code.size(), cached.code.size());
  if (fresh.has_ctab != cached.has_ctab) diff += " ctab presence";
  if (fresh.ctab != cached.ctab) diff += std::format(" ctab blob ({} vs {} bytes)", fresh.ctab.size(), cached.ctab.size());
  // Rebuild from the cached bytes next to the original's objects and compare those too.
  const PPCContext saved = ctx;
  ctx.r1.u64 = (saved.r1.u32 - 0x40) & ~0xFull;
  const uint32_t slot_buffer = ctx.r1.u32 + 0x10, slot_ctab = ctx.r1.u32 + 0x18;
  const bool want_ctab = c.compile && c.pp_ctab;
  if (!Build(ctx, base, cached, slot_buffer, want_ctab ? slot_ctab : 0)) {
    diff += " rebuild failed";
  } else {
    const uint32_t rb = fcp::guest::U32(base, slot_buffer);
    const uint32_t ob = fcp::guest::U32(base, c.pp_buffer);
    if (fcp::guest::U32(base, rb) != fcp::guest::U32(base, ob)) diff += " buffer vtable";
    std::vector<uint8_t> bytes;
    if (!ReadBuffer(ctx, base, rb, bytes) || bytes != fresh.code) diff += " rebuilt microcode";
    if (want_ctab) {
      const uint32_t rc = fcp::guest::U32(base, slot_ctab), oc = fcp::guest::U32(base, c.pp_ctab);
      if (bool(rc) != bool(oc)) diff += " rebuilt ctab presence";
      else if (rc) diff += CompareCtab(base, oc, rc);
      Release(ctx, base, rc);
    }
    Release(ctx, base, rb);
  }
  ctx.r1 = saved.r1;
  if (diff.empty()) {
    ++g_verify_equal;
  } else {
    ++g_verify_diff;
    FCPLOG("shader compile cache: VERIFY DIFFERENT {} ({}):{}", c.key, c.label, diff);
  }
}

}  // namespace

void Compile(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  Configure();
  if (g_mode == Mode::kOff) {
    original(ctx, base);
    return;
  }
  const Clock::time_point t0 = Clock::now();
  Call c;
  if (!BuildKey(ctx, base, c)) {
    ++g_uncacheable;
    original(ctx, base);
    return;
  }
  Entry cached;
  const bool have = Load(c.key, cached);
  if (have && g_mode != Mode::kVerify) {
    const PPCContext entry_ctx = ctx;
    if (Build(ctx, base, cached, c.pp_buffer, c.compile ? c.pp_ctab : 0)) {
      ctx.r3.u64 = 1;  // success, like the original
      const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
      g_hit_us += us;
      const uint64_t n = ++g_hits;
      if (n <= 3) FCPLOG("shader compile cache: hit {} ({}) f{} {} us", c.key, c.label, fcp::watch::SwapCount(), us);
      return;
    }
    // Could not rebuild (out of guest memory?): the original decides.
    ++g_fallbacks;
    FCPLOG("shader compile cache: rebuild failed for {} ({}), running the original", c.key, c.label);
    ctx = entry_ctx;
  }
  original(ctx, base);
  const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
  g_miss_us += us;
  ++g_misses;
  if (ctx.r3.u32 != 1) return;  // failures are not cached; the original wrote its error text
  const PPCContext after = ctx;
  Entry fresh;
  if (Capture(ctx, base, c, fresh)) {
    if (g_mode == Mode::kVerify && have) {
      Verify(ctx, base, c, fresh, cached);
    } else {
      Store(c.key, fresh);
    }
  }
  ctx = after;
}

void WriteSummary() {
  if (!g_configured) return;
  FCPLOG("shader compile cache: {} hits ({:.1f} ms), {} originals ({:.1f} ms), {} stored, {} fallbacks, {} uncacheable, "
         "verify {} equal / {} different ({} constant words compared, {} of them heap pointers)",
         g_hits.load(), g_hit_us.load() / 1000.0, g_misses.load(), g_miss_us.load() / 1000.0, g_stored.load(),
         g_fallbacks.load(), g_uncacheable.load(), g_verify_equal.load(), g_verify_diff.load(), g_verify_words.load(),
         g_verify_pointer_words.load());
}

}  // namespace fcp::shadercache
