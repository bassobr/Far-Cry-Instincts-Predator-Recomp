// Micro benchmark of the [fp_rules] helpers (rex/ppc/fcp_fp_rules.h).
// Not part of the build; tools/fp_rules_bench.cmd compiles it with the flags of the recompiled code
// (-O3 -msse4.1 -ffp-model=strict) and runs it. Each kernel reads and writes double "registers" in
// memory like the emitted code (ctx.f[n]); the time per operation is the best of 7 runs.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <rex/ppc/fcp_fp_rules.h>

using rex::f32;
using rex::f64;
namespace r = rex::ppc::fcp_fp_rules;

static constexpr size_t kN = 1 << 16;
static volatile double g_sink;

template <typename F>
static double Best(F&& f) {
  double best = 1e30;
  for (int rep = 0; rep < 7; ++rep) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  return best;
}

int main(int argc, char** argv) {
  const int loops = argc > 1 ? std::atoi(argv[1]) : 64;
  std::mt19937 rng(20261112);
  std::uniform_real_distribution<float> u(-100.0f, 100.0f);
  std::uniform_int_distribution<int> small(-64, 64);
  std::vector<f64> a(kN), b(kN), c(kN), d(kN), sa(kN), sb(kN), sc(kN);
  for (size_t i = 0; i < kN; ++i) {
    a[i] = u(rng);
    b[i] = u(rng);
    c[i] = u(rng);
    // "simple" values like many game constants: small integers and halves (fmadds ties are common)
    sa[i] = small(rng) * 0.5;
    sb[i] = small(rng) * 0.25;
    sc[i] = small(rng);
  }
  std::vector<float> va(kN * 4), vb(kN * 4), vd(kN * 4);
  for (auto& x : va) x = u(rng);
  for (auto& x : vb) x = u(rng);

  auto run = [&](const char* name, auto&& body) {
    const double ns = Best([&] {
      for (int l = 0; l < loops; ++l) body();
    });
    std::printf("%-34s %7.3f ns/op\n", name, ns / (double(loops) * kN));
  };

  run("fadds host", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = f64(f32(a[i] + b[i]));
  });
  run("fadds nan=ppc", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = r::Nan2(f64(f32(a[i] + b[i])), a[i], b[i], 1, 0x82000000);
  });
  run("fmuls host", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = f64(f32(a[i] * c[i]));
  });
  run("fmuls nan=ppc", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = r::Nan2(f64(f32(a[i] * c[i])), a[i], c[i], 1, 0x82000000);
  });
  run("fmadds host", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = f64(f32(std::fma(a[i], c[i], b[i])));
  });
  run("fmadds rounding=single", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = r::FmaddsRound(std::fma(a[i], c[i], b[i]), a[i], c[i], b[i], 0x82000000);
  });
  run("fmadds single+nan", [&] {
    for (size_t i = 0; i < kN; ++i)
      d[i] = r::Nan3(r::FmaddsRound(std::fma(a[i], c[i], b[i]), a[i], c[i], b[i], 0x82000000), a[i], b[i], c[i], 1,
                     0x82000000);
  });
  run("fmadds host (simple values)", [&] {
    for (size_t i = 0; i < kN; ++i) d[i] = f64(f32(std::fma(sa[i], sc[i], sb[i])));
  });
  run("fmadds single (simple values)", [&] {
    for (size_t i = 0; i < kN; ++i)
      d[i] = r::FmaddsRound(std::fma(sa[i], sc[i], sb[i]), sa[i], sc[i], sb[i], 0x82000000);
  });
  run("vaddfp host", [&] {
    for (size_t i = 0; i < kN; ++i)
      simde_mm_store_ps(&vd[i * 4], simde_mm_add_ps(simde_mm_load_ps(&va[i * 4]), simde_mm_load_ps(&vb[i * 4])));
  });
  run("vaddfp vmx_nan=ppc", [&] {
    for (size_t i = 0; i < kN; ++i) {
      const simde__m128 x = simde_mm_load_ps(&va[i * 4]), y = simde_mm_load_ps(&vb[i * 4]);
      simde_mm_store_ps(&vd[i * 4], r::VNan2(simde_mm_add_ps(x, y), x, y, 0x82000000));
    }
  });
  run("vmaxfp host", [&] {
    for (size_t i = 0; i < kN; ++i)
      simde_mm_store_ps(&vd[i * 4], simde_mm_max_ps(simde_mm_load_ps(&va[i * 4]), simde_mm_load_ps(&vb[i * 4])));
  });
  run("vmaxfp vmx_nan=ppc (nj)", [&] {
    for (size_t i = 0; i < kN; ++i)
      simde_mm_store_ps(&vd[i * 4],
                        r::VMinMax(simde_mm_load_ps(&va[i * 4]), simde_mm_load_ps(&vb[i * 4]), true, true, 0x82000000));
  });
  double s = 0;
  for (size_t i = 0; i < kN; ++i) s += d[i] + vd[i];
  g_sink = s;
  return 0;
}
