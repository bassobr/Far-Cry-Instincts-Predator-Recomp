// Runs the recompiled semantic test functions.
//
//   fcp_semtest_<rex|xr>.exe <cases.bin> <results.bin> [<default.bin> to map at 0x82000000]
//
// Built twice from this source: with the ReXGlue generated code and the game's PCH (FCP_SEM_REX), and with
// the XenonRecomp generated code and its ppc_context.h (FCP_SEM_XR). Both see the same cases.
//
// cases.bin (little endian): "FCPS", u32 version 1, u32 count, then per case
//   u32 address, u32 flags (bits 0-1 FPSCR[RN], bit 2 VSCR[NJ]), u16 n_in, u16 n_out, u16 n_mem_in,
//   u16 n_mem_out, n_in * {u8 kind, u8 index, u16 0, u32 w[4]}, n_out * {u8 kind, u8 index, u16 0},
//   n_mem_in * {u32 address, u32 length, bytes padded to 4}, n_mem_out * {u32 address, u32 length}.
// Register kinds: 0 GPR, 1 FPR (bits), 2 VR (w[0] = element 0), 3 CR, 4 XER (SO<<31|OV<<30|CA<<29),
//   5 CTR, 6 LR, 7 VSCR (NJ<<16|SAT), 8 MSR, 9 FPSCR[RN].
// 64 bit values use w[0] = high word, w[1] = low word.
// results.bin: "FCPR", u32 count, then per case u32 status (0 ok, 1 no function, 2 host exception),
//   n_out * 16 bytes (w[4]) and the memory outputs padded to 4.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <new>
#include <xmmintrin.h>

#if defined(FCP_SEM_XR)
#include <ppc_context.h>
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "sem_runner_api.h"

namespace {

constexpr uint64_t kGuestSize = 0x100000000ull;
constexpr uint32_t kDataBase = 0x10000000u;
constexpr uint32_t kDataSize = 0x00100000u;
constexpr uint32_t kStackBase = 0x6F000000u;
constexpr uint32_t kStackSize = 0x00100000u;
constexpr uint32_t kImageBase = 0x82000000u;
constexpr uint32_t kImageSize = 0x00980000u;

#define FCP_R(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) \
  X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31)
#define FCP_V(X) FCP_R(X) X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) \
  X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) X(56) X(57) X(58) X(59) X(60) X(61) \
  X(62) X(63) X(64) X(65) X(66) X(67) X(68) X(69) X(70) X(71) X(72) X(73) X(74) X(75) X(76) X(77) X(78) \
  X(79) X(80) X(81) X(82) X(83) X(84) X(85) X(86) X(87) X(88) X(89) X(90) X(91) X(92) X(93) X(94) X(95) \
  X(96) X(97) X(98) X(99) X(100) X(101) X(102) X(103) X(104) X(105) X(106) X(107) X(108) X(109) X(110) \
  X(111) X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) X(120) X(121) X(122) X(123) X(124) \
  X(125) X(126) X(127)

struct Regs {
  uint64_t* r[32];
  uint64_t* f[32];
  uint32_t* v[128];  // u32[4], u32[3] = element 0
  decltype(PPCContext::cr0)* cr[8];
};

Regs Bind(PPCContext& ctx) {
  Regs g{};
#define FCP_BR(n) g.r[n] = &ctx.r##n.u64; g.f[n] = reinterpret_cast<uint64_t*>(&ctx.f##n.u64);
  FCP_R(FCP_BR)
#define FCP_BV(n) g.v[n] = ctx.v##n.u32;
  FCP_V(FCP_BV)
  g.cr[0] = &ctx.cr0; g.cr[1] = &ctx.cr1; g.cr[2] = &ctx.cr2; g.cr[3] = &ctx.cr3;
  g.cr[4] = &ctx.cr4; g.cr[5] = &ctx.cr5; g.cr[6] = &ctx.cr6; g.cr[7] = &ctx.cr7;
  return g;
}

uint32_t CrGet(const Regs& g) {
  uint32_t v = 0;
  for (int i = 0; i < 8; ++i) {
    auto* c = g.cr[i];
    v |= ((c->lt << 3) | (c->gt << 2) | (c->eq << 1) | c->so) << (28 - 4 * i);
  }
  return v;
}

void CrSet(const Regs& g, uint32_t v) {
  for (int i = 0; i < 8; ++i) {
    uint32_t f = (v >> (28 - 4 * i)) & 0xF;
    auto* c = g.cr[i];
    c->lt = (f >> 3) & 1;
    c->gt = (f >> 2) & 1;
    c->eq = (f >> 1) & 1;
    c->so = f & 1;
  }
}

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  uint32_t U32() {
    uint32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
  uint16_t U16() {
    uint16_t v;
    std::memcpy(&v, p, 2);
    p += 2;
    return v;
  }
  uint8_t U8() { return *p++; }
};

void SetRounding(PPCContext& ctx, uint32_t rn) {
#if defined(FCP_SEM_REX)
  ctx.fpscr.InitHost();
  ctx.fpscr.StoreToHost(rn);
  ctx.fpscr.DisableFlushModeUnconditional();
#else
  ctx.fpscr.loadFromHost();
  ctx.fpscr.storeFromGuest(rn);
  ctx.fpscr.disableFlushModeUnconditional();
#endif
}

uint32_t GetRounding(PPCContext& ctx) {
#if defined(FCP_SEM_REX)
  return ctx.fpscr.LoadFromHost();
#else
  return ctx.fpscr.loadFromHost();
#endif
}

int Call(FcpSemFn fn, PPCContext& ctx, uint8_t* base) {
  __try {
    fn(ctx, base);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 2;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <cases.bin> <results.bin> [default.bin]\n", argv[0]);
    return 2;
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  std::vector<uint8_t> data;
  {
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
      data.insert(data.end(), buf, buf + n);
    std::fclose(f);
  }
  Reader rd{data.data(), data.data() + data.size()};
  if (data.size() < 12 || std::memcmp(rd.p, "FCPS", 4) != 0) {
    std::fprintf(stderr, "bad case file\n");
    return 2;
  }
  rd.p += 4;
  rd.U32();
  const uint32_t count = rd.U32();

  auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kGuestSize, MEM_RESERVE, PAGE_NOACCESS));
  if (!base) {
    std::fprintf(stderr, "cannot reserve guest memory\n");
    return 2;
  }
  VirtualAlloc(base + kDataBase, kDataSize, MEM_COMMIT, PAGE_READWRITE);
  VirtualAlloc(base + kStackBase, kStackSize, MEM_COMMIT, PAGE_READWRITE);
  if (argc > 3) {
    FILE* img = std::fopen(argv[3], "rb");
    if (img) {
      VirtualAlloc(base + kImageBase, kImageSize, MEM_COMMIT, PAGE_READWRITE);
      size_t got = std::fread(base + kImageBase, 1, kImageSize, img);
      (void)got;
      std::fclose(img);
    }
  }

  // FCP_SEM_DLL: the test functions are in a DLL built like predator_code.dll (cmake/fcp_semtest.cmake).
#if defined(FCP_SEM_DLL)
  HMODULE code = LoadLibraryA(FCP_SEM_DLL);
  const auto* funcs = code ? reinterpret_cast<const FcpSemFunc*>(GetProcAddress(code, "fcp_sem_funcs")) : nullptr;
  if (!funcs) {
    std::fprintf(stderr, "cannot load %s (error %lu)\n", FCP_SEM_DLL, GetLastError());
    return 2;
  }
#else
  const FcpSemFunc* funcs = fcp_sem_funcs;
#endif
  std::unordered_map<uint32_t, FcpSemFn> table;
  for (const FcpSemFunc* e = funcs; e->fn; ++e)
    table[e->address] = e->fn;

  std::vector<uint8_t> out;
  out.insert(out.end(), {'F', 'C', 'P', 'R'});
  auto put32 = [&](uint32_t v) {
    uint8_t b[4];
    std::memcpy(b, &v, 4);
    out.insert(out.end(), b, b + 4);
  };
  put32(count);

  const uint32_t default_csr = _mm_getcsr();
  auto* ctx_mem = static_cast<PPCContext*>(_aligned_malloc(sizeof(PPCContext), 64));
  for (uint32_t c = 0; c < count; ++c) {
    const uint32_t address = rd.U32();
    const uint32_t flags = rd.U32();
    const uint16_t n_in = rd.U16(), n_out = rd.U16(), n_mem_in = rd.U16(), n_mem_out = rd.U16();
    PPCContext& ctx = *new (ctx_mem) PPCContext{};
    Regs g = Bind(ctx);
    *g.r[1] = kStackBase + kStackSize - 0x1000;
    ctx.lr = 0x0BADC0DCu;  // the reference's return sentinel (ppcref.RETURN_SENTINEL)
#if defined(FCP_SEM_REX)
    ctx.vscr.nj = (flags >> 2) & 1;
#endif
    SetRounding(ctx, flags & 3);
    for (uint16_t i = 0; i < n_in; ++i) {
      const uint8_t kind = rd.U8(), idx = rd.U8();
      rd.U16();
      uint32_t w[4];
      for (auto& x : w)
        x = rd.U32();
      const uint64_t v64 = (uint64_t(w[0]) << 32) | w[1];
      switch (kind) {
        case 0: *g.r[idx] = v64; break;
        case 1: *g.f[idx] = v64; break;
        case 2: for (int k = 0; k < 4; ++k) g.v[idx][3 - k] = w[k]; break;
        case 3: CrSet(g, w[0]); break;
        case 4: ctx.xer.so = w[0] >> 31; ctx.xer.ov = (w[0] >> 30) & 1; ctx.xer.ca = (w[0] >> 29) & 1; break;
        case 5: ctx.ctr.u64 = v64; break;
        case 6: ctx.lr = v64; break;
#if defined(FCP_SEM_REX)
        case 7: ctx.vscr.Write(w[0]); break;
#endif
        case 8: ctx.msr = w[0]; break;
        default: break;
      }
    }
    std::vector<std::pair<uint8_t, uint8_t>> outs(n_out);
    for (auto& o : outs) {
      o.first = rd.U8();
      o.second = rd.U8();
      rd.U16();
    }
    for (uint16_t i = 0; i < n_mem_in; ++i) {
      const uint32_t a = rd.U32(), len = rd.U32();
      std::memcpy(base + a, rd.p, len);
      rd.p += (len + 3) & ~3u;
    }
    std::vector<std::pair<uint32_t, uint32_t>> mem_outs(n_mem_out);
    for (auto& m : mem_outs) {
      m.first = rd.U32();
      m.second = rd.U32();
    }

    uint32_t status = 0;
    auto it = table.find(address);
    if (it == table.end())
      status = 1;
    else
      status = Call(it->second, ctx, base);
    const uint32_t rn_after = GetRounding(ctx);
    _mm_setcsr(default_csr);

    put32(status);
    for (auto& [kind, idx] : outs) {
      uint32_t w[4] = {0, 0, 0, 0};
      uint64_t v64 = 0;
      switch (kind) {
        case 0: v64 = *g.r[idx]; break;
        case 1: v64 = *g.f[idx]; break;
        case 2: for (int k = 0; k < 4; ++k) w[k] = g.v[idx][3 - k]; break;
        case 3: w[0] = CrGet(g); break;
        case 4: w[0] = (uint32_t(ctx.xer.so) << 31) | (uint32_t(ctx.xer.ov) << 30) | (uint32_t(ctx.xer.ca) << 29); break;
        case 5: v64 = ctx.ctr.u64; break;
        case 6: v64 = ctx.lr; break;
#if defined(FCP_SEM_REX)
        case 7: w[0] = ctx.vscr.Read(); break;
#endif
        case 8: w[0] = ctx.msr; break;
        case 9: w[0] = rn_after; break;
        default: break;
      }
      if (kind == 0 || kind == 1 || kind == 5 || kind == 6) {
        w[0] = uint32_t(v64 >> 32);
        w[1] = uint32_t(v64);
      }
      for (auto x : w)
        put32(x);
    }
    for (auto& [a, len] : mem_outs) {
      out.insert(out.end(), base + a, base + a + len);
      for (uint32_t pad = len; pad & 3; ++pad)
        out.push_back(0);
    }
    // clear what the case touched so the next one starts from zero
    for (auto& [a, len] : mem_outs)
      std::memset(base + a, 0, len);
  }

  FILE* o = std::fopen(argv[2], "wb");
  if (!o) {
    std::fprintf(stderr, "cannot write %s\n", argv[2]);
    return 2;
  }
  std::fwrite(out.data(), 1, out.size(), o);
  std::fclose(o);
  return 0;
}
