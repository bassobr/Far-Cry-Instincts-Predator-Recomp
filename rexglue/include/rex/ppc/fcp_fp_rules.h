/**
 * @file        rex/ppc/fcp_fp_rules.h
 *
 * @brief       FCP: PowerPC rules for NaN results and for the single rounding of fmadds, used by the
 *              code that the codegen emits when the title configuration enables `[fp_rules]`
 *              (rexglue/UPSTREAM.md, change 70).
 *
 * @license     BSD 3-Clause License (same as the SDK). Local addition of the Far Cry Instincts
 *              Predator project, not part of upstream ReXGlue.
 *
 * Every helper has an inline fast path that only tests the host result and an out-of-line slow path
 * that recomputes the guest result:
 *  - Scalar NaN (Book I 4.3.2, 4.6.6 of the PowerPC ISA): when the host result is a NaN, the result is
 *    the first NaN operand in the order frA, frB, frC, quieted (bit 51 set), else the default QNaN
 *    0x7FF8000000000000. The sign is never changed (also for fnmadd/fnmsub/fmsub). Single forms keep
 *    the high 23 fraction bits (frsp of a NaN).
 *  - VMX NaN (AltiVec PEM, vaddfp etc.): per lane the first NaN operand in the order vA, vB, vC,
 *    quieted (bit 22 set), else the default QNaN 0x7FC00000. vmaxfp/vminfp: a NaN operand gives the
 *    quieted NaN, max(+0,-0) = +0 and min(+0,-0) = -0.
 *  - fmadds family: frA*frC+frB is fused and rounded once to single. The host computes the fused
 *    result in double (one rounding) and rounds again to single. That second rounding is only wrong in
 *    round-to-nearest when the double result lies exactly halfway between two singles; the slow path
 *    then decides with the exact sign of (frA*frC+frB - result).
 *
 * The slow paths also count, per guest address, how often they ran and how often the guest result
 * differs from the host result (fcp_fp_rules::Stats, written to the log by src/fcp/fp_stats.cpp).
 */
#pragma once

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include <simde/x86/sse4.1.h>

#include <rex/types.h>

namespace rex::ppc::fcp_fp_rules {

/** Kind of a slow path entry (index into Stats::kinds). */
enum Kind : u32 {
  kScalarNan = 0,  /**< scalar FPU NaN result */
  kVectorNan = 1,  /**< VMX add/sub/mul NaN lane */
  kVectorMinMax = 2, /**< vmaxfp/vminfp with NaN or signed zero operands */
  kFmaddsTie = 3,  /**< fmadds family: double result halfway between two singles */
  kKindCount = 4,
};

/** Per site counters (open addressing on guest address | kind, 0 = free). */
struct Site {
  std::atomic<u32> key{0};
  std::atomic<u64> hits{0};
  std::atomic<u64> differs{0};
};

struct Stats {
  static constexpr u32 kSites = 8192;
  std::atomic<u64> hits[kKindCount]{};
  std::atomic<u64> differs[kKindCount]{};
  std::atomic<u64> sites_dropped{0};
  Site sites[kSites];
};

inline Stats g_stats;

/**
 * Counting costs shared atomics in the slow paths, so it is on only with FCP_FP_STATS=1 (read once).
 * -1: not read yet.
 */
inline std::atomic<int> g_stats_on{-1};

[[gnu::noinline]] inline bool ReadStatsSwitch() {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const char* v = std::getenv("FCP_FP_STATS");
#pragma clang diagnostic pop
  const int on = (v != nullptr && v[0] != 0 && v[0] != '0') ? 1 : 0;
  g_stats_on.store(on, std::memory_order_relaxed);
  return on != 0;
}

inline bool StatsOn() {
  const int on = g_stats_on.load(std::memory_order_relaxed);
  return on < 0 ? ReadStatsSwitch() : on != 0;
}

inline void Count(u32 kind, u32 address, bool differs) {
  if (!StatsOn()) {
    return;
  }
  g_stats.hits[kind].fetch_add(1, std::memory_order_relaxed);
  if (differs) {
    g_stats.differs[kind].fetch_add(1, std::memory_order_relaxed);
  }
  const u32 key = (address & ~3u) | kind;  // guest instructions are 4 byte aligned
  u32 slot = (key * 2654435761u) >> 19;     // 13 bits
  for (u32 probe = 0; probe < 64; ++probe, slot = (slot + 1) & (Stats::kSites - 1)) {
    Site& s = g_stats.sites[slot];
    u32 cur = s.key.load(std::memory_order_relaxed);
    if (cur == 0 && s.key.compare_exchange_strong(cur, key, std::memory_order_relaxed)) {
      cur = key;
    }
    if (cur == key) {
      s.hits.fetch_add(1, std::memory_order_relaxed);
      if (differs) {
        s.differs.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
  }
  g_stats.sites_dropped.fetch_add(1, std::memory_order_relaxed);
}

// ---- scalar NaN ------------------------------------------------------------------------------------

inline bool IsNan64(u64 b) {
  return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
}

/**
 * Guest result for a host NaN result. a, b, c: operands in PowerPC priority order (frA, frB, frC);
 * count: how many of them take part. single: single precision form.
 */
[[gnu::noinline]] inline f64 NanSlow(f64 host, f64 a, f64 b, f64 c, u32 count, u32 single,
                                     u32 address) {
  const u64 ops[3] = {std::bit_cast<u64>(a), std::bit_cast<u64>(b), std::bit_cast<u64>(c)};
  u64 r = 0x7FF8000000000000ull;  // default QNaN [ISA 4.3.2]
  for (u32 i = 0; i < count; ++i) {
    if (IsNan64(ops[i])) {
      r = ops[i] | 0x0008000000000000ull;  // quiet [ISA 4.3.2]
      break;
    }
  }
  if (single) {
    r &= 0xFFFFFFFFE0000000ull;  // frsp of a NaN keeps the high 23 fraction bits [ISA 4.6.2]
  }
  Count(kScalarNan, address, r != std::bit_cast<u64>(host));
  return std::bit_cast<f64>(r);
}

/** fadd, fsub, fdiv (frA, frB) and fmul (frA, frC). */
[[gnu::always_inline]] inline f64 Nan2(f64 host, f64 a, f64 b, u32 single, u32 address) {
  if (!std::isnan(host)) [[likely]] {
    return host;
  }
  return NanSlow(host, a, b, 0.0, 2, single, address);
}

/** Fused forms: frA, frB, frC. */
[[gnu::always_inline]] inline f64 Nan3(f64 host, f64 a, f64 b, f64 c, u32 single, u32 address) {
  if (!std::isnan(host)) [[likely]] {
    return host;
  }
  return NanSlow(host, a, b, c, 3, single, address);
}

/** fsqrt, fsqrts (frB). */
[[gnu::always_inline]] inline f64 Nan1(f64 host, f64 b, u32 single, u32 address) {
  if (!std::isnan(host)) [[likely]] {
    return host;
  }
  return NanSlow(host, b, 0.0, 0.0, 1, single, address);
}

// ---- fmadds single rounding -------------------------------------------------------------------------

/** Error free sum (Knuth), round to nearest. */
inline void TwoSum(f64 a, f64 b, f64& s, f64& e) {
  s = a + b;
  const f64 bb = s - a;
  e = (a - (s - bb)) + (b - bb);
}

/**
 * Sign of a*c + b - r computed exactly (round to nearest, no overflow; a product below the double
 * normal range loses its low part). -1, 0 or +1.
 */
inline int ResidualSign(f64 a, f64 c, f64 b, f64 r) {
  const f64 p = a * c;
  // a*c = p + pe exactly; two singles (24 bit significands) multiply exactly in double
  const bool singles = ((std::bit_cast<u64>(a) | std::bit_cast<u64>(c)) & 0x1FFFFFFFull) == 0;
  const f64 pe = singles ? 0.0 : std::fma(a, c, -p);
  // expansion of p + b - r + pe, components grow in magnitude (Shewchuk, grow-expansion)
  f64 e[4];
  int n = 0;
  auto grow = [&](f64 x) {
    f64 q = x;
    int m = 0;
    for (int i = 0; i < n; ++i) {
      f64 s, h;
      TwoSum(q, e[i], s, h);
      if (h != 0.0) {
        e[m++] = h;
      }
      q = s;
    }
    if (q != 0.0) {
      e[m++] = q;
    }
    n = m;
  };
  grow(p);
  grow(b);
  grow(-r);
  grow(pe);
  if (n == 0) {
    return 0;
  }
  return e[n - 1] > 0.0 ? 1 : -1;
}

/** r: the fused result rounded to double by the host; a, c, b as in frA*frC + frB. */
[[gnu::noinline]] inline f64 FmaddsSlow(f64 r, f64 a, f64 c, f64 b, u32 address) {
  const f32 s = static_cast<f32>(r);  // host double rounding (current rounding mode)
  if (!std::isfinite(r) || (simde_mm_getcsr() & SIMDE_MM_ROUND_MASK) != SIMDE_MM_ROUND_NEAREST) {
    return s;  // directed rounding: rounding twice in the same direction is exact
  }
  const u64 bits = std::bit_cast<u64>(r);
  const f64 mag = std::fabs(r);
  f64 toward_zero;  // the single next to r towards zero
  if (mag >= 0x1p128) {
    return s;  // overflow either way
  }
  if (mag >= 0x1p-126) {
    if ((bits & 0x1FFFFFFFull) != 0x10000000ull) {
      return s;  // not halfway: the double rounding cannot have changed the single result
    }
    toward_zero = std::bit_cast<f64>(bits & ~0x1FFFFFFFull);
  } else {
    const f64 q = r * 0x1p149;  // single subnormal grid, exact scaling
    const f64 t = std::trunc(q);
    if (std::fabs(q - t) != 0.5) {
      return s;
    }
    toward_zero = t * 0x1p-149;
  }
  const int sign = ResidualSign(a, c, b, r);
  f32 result = s;  // exact tie: round half to even, as the host did
  if (sign != 0) {
    const f32 tz = static_cast<f32>(toward_zero);  // exact
    const f32 away = std::nextafter(tz, std::copysign(std::numeric_limits<f32>::infinity(), tz));
    result = ((sign > 0) == (r > 0.0)) ? away : tz;
  }
  Count(kFmaddsTie, address, result != s);
  return result;
}

/**
 * f64(f32(r)) with a single rounding of the exact frA*frC+frB. r: std::fma result (one rounding to
 * double). Fast path: the low 28 bits are not all zero, so r is not halfway between two singles.
 */
[[gnu::always_inline]] inline f64 FmaddsRound(f64 r, f64 a, f64 c, f64 b, u32 address) {
  const u64 bits = std::bit_cast<u64>(r);
  if ((bits & 0x0FFFFFFFull) == 0) [[unlikely]] {
    // halfway in the normal range (bit 28 set), or below the single normal range (nonzero)
    if ((bits & 0x10000000ull) != 0 ||
        ((bits & 0x7FF0000000000000ull) < 0x3810000000000000ull && (bits << 1) != 0)) {
      return FmaddsSlow(r, a, c, b, address);
    }
  }
  return static_cast<f64>(static_cast<f32>(r));
}

// ---- VMX NaN ---------------------------------------------------------------------------------------

inline bool IsNan32(u32 b) {
  return (b & 0x7FFFFFFFu) > 0x7F800000u;
}

/** Per lane: first NaN operand of a, b, c (count of them), quieted, else 0x7FC00000. */
[[gnu::noinline]] inline simde__m128 VNanSlow(simde__m128 host, simde__m128 a, simde__m128 b,
                                              simde__m128 c, u32 count, u32 address) {
  alignas(16) u32 h[4], x[3][4];
  simde_mm_store_ps(reinterpret_cast<f32*>(h), host);
  simde_mm_store_ps(reinterpret_cast<f32*>(x[0]), a);
  simde_mm_store_ps(reinterpret_cast<f32*>(x[1]), b);
  simde_mm_store_ps(reinterpret_cast<f32*>(x[2]), c);
  bool differs = false;
  for (int l = 0; l < 4; ++l) {
    if (!IsNan32(h[l])) {
      continue;
    }
    u32 r = 0x7FC00000u;  // default QNaN of VMX [PEM]
    for (u32 i = 0; i < count; ++i) {
      if (IsNan32(x[i][l])) {
        r = x[i][l] | 0x00400000u;
        break;
      }
    }
    differs |= r != h[l];
    h[l] = r;
  }
  Count(kVectorNan, address, differs);
  return simde_mm_load_ps(reinterpret_cast<const f32*>(h));
}

[[gnu::always_inline]] inline simde__m128 VNan2(simde__m128 host, simde__m128 a, simde__m128 b,
                                                u32 address) {
  if (simde_mm_movemask_ps(simde_mm_cmpunord_ps(host, host)) == 0) [[likely]] {
    return host;
  }
  return VNanSlow(host, a, b, simde_mm_setzero_ps(), 2, address);
}

[[gnu::always_inline]] inline simde__m128 VNan3(simde__m128 host, simde__m128 a, simde__m128 b,
                                                simde__m128 c, u32 address) {
  if (simde_mm_movemask_ps(simde_mm_cmpunord_ps(host, host)) == 0) [[likely]] {
    return host;
  }
  return VNanSlow(host, a, b, c, 3, address);
}

/**
 * vmaxfp/vminfp per lane [PEM]: NaN operand -> quieted NaN (vA first), equal values -> max: a & b,
 * min: a | b (so max(+0,-0) = +0, min(+0,-0) = -0); with nj, operands below the normal range count
 * as zero of their sign and a result below the normal range is flushed to a zero of its sign.
 */
[[gnu::noinline]] inline simde__m128 VMinMaxSlow(simde__m128 host, simde__m128 a, simde__m128 b,
                                                 bool is_max, bool nj, u32 address) {
  alignas(16) u32 h[4], x[4], y[4];
  simde_mm_store_ps(reinterpret_cast<f32*>(h), host);
  simde_mm_store_ps(reinterpret_cast<f32*>(x), a);
  simde_mm_store_ps(reinterpret_cast<f32*>(y), b);
  bool differs = false;
  for (int l = 0; l < 4; ++l) {
    u32 xa = x[l], yb = y[l];
    if (nj) {
      if ((xa & 0x7F800000u) == 0) xa &= 0x80000000u;
      if ((yb & 0x7F800000u) == 0) yb &= 0x80000000u;
    }
    u32 r;
    if (IsNan32(xa)) {
      r = xa | 0x00400000u;
    } else if (IsNan32(yb)) {
      r = yb | 0x00400000u;
    } else {
      const f32 fx = std::bit_cast<f32>(xa), fy = std::bit_cast<f32>(yb);
      if (fx == fy) {
        r = is_max ? (xa & yb) : (xa | yb);
      } else {
        r = ((fx > fy) == is_max) ? xa : yb;
      }
    }
    differs |= r != h[l];
    h[l] = r;
  }
  Count(kVectorMinMax, address, differs);
  return simde_mm_load_ps(reinterpret_cast<const f32*>(h));
}

/**
 * Fast path: no NaN operand, no lane whose operands compare equal with different bits (+0/-0) and,
 * with nj, no operand below the normal range. Only those lanes can differ from maxps/minps.
 */
[[gnu::always_inline]] inline simde__m128 VMinMax(simde__m128 a, simde__m128 b, bool is_max, bool nj,
                                                  u32 address) {
  const simde__m128 host = is_max ? simde_mm_max_ps(a, b) : simde_mm_min_ps(a, b);
  const simde__m128i ia = simde_mm_castps_si128(a), ib = simde_mm_castps_si128(b);
  // NaN lanes, and lanes that compare equal with different bits (+0/-0)
  simde__m128 special = simde_mm_or_ps(
      simde_mm_cmpunord_ps(a, b),
      simde_mm_andnot_ps(simde_mm_castsi128_ps(simde_mm_cmpeq_epi32(ia, ib)), simde_mm_cmpeq_ps(a, b)));
  if (nj) {
    // denormal operands (exponent 0, fraction not 0); a zero against a nonzero value is the same on
    // both, so zeros stay on the fast path
    const simde__m128i exp = simde_mm_set1_epi32(0x7F800000);
    const simde__m128i mag = simde_mm_set1_epi32(0x7FFFFFFF);
    const simde__m128i zero = simde_mm_setzero_si128();
    const simde__m128i da = simde_mm_andnot_si128(simde_mm_cmpeq_epi32(simde_mm_and_si128(ia, mag), zero),
                                                  simde_mm_cmpeq_epi32(simde_mm_and_si128(ia, exp), zero));
    const simde__m128i db = simde_mm_andnot_si128(simde_mm_cmpeq_epi32(simde_mm_and_si128(ib, mag), zero),
                                                  simde_mm_cmpeq_epi32(simde_mm_and_si128(ib, exp), zero));
    special = simde_mm_or_ps(special, simde_mm_castsi128_ps(simde_mm_or_si128(da, db)));
  }
  if (simde_mm_movemask_ps(special) == 0) [[likely]] {
    return host;
  }
  return VMinMaxSlow(host, a, b, is_max, nj, address);
}

}  // namespace rex::ppc::fcp_fp_rules
