// limiter_selftest.cpp: synthetic test of rex::apu::PeakLimiter (rexglue/src/apu/mix.*, UPSTREAM 371) without the game.
// Build and run (from the repository root, LLVM of tools/get_llvm.ps1):
//   toolchain\llvm\bin\clang++ -std=c++23 -O2 -ffp-contract=off -Irexglue/include -Irexglue/src
//       tools/audio/limiter_selftest.cpp rexglue/src/apu/mix.cpp -o out/limiter_selftest.exe
// Checks, for stereo and 6 channels: below the ceiling every sample passes bit for bit, delayed by 63 frames; a burst
// up to +6 dBFS (and a single-sample spike of +12 dBFS) never leaves the ceiling; the gain is back to exactly 1 after
// the release. Prints one line per case and exits non-zero on a failure. With a WAV of FCP_RECORD_AUDIO as argument it
// runs that 6 channel mix through the limiter as a 5.1 device would get it (see Replay).
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "apu/mix.h"

using rex::apu::LimiterStats;
using rex::apu::PeakLimiter;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
  g_failures += ok ? 0 : 1;
}

// level(n) * sine on every channel (another phase per channel)
std::vector<float> Signal(unsigned ch, size_t frames, float (*level)(size_t)) {
  std::vector<float> s(frames * ch);
  for (size_t n = 0; n < frames; ++n)
    for (unsigned c = 0; c < ch; ++c) s[n * ch + c] = level(n) * std::sin(0.031f * float(n) + float(c));
  return s;
}

void Run(unsigned ch) {
  std::printf("%u channels\n", ch);
  constexpr size_t kFrames = 48000 * 4;
  constexpr unsigned kDelay = PeakLimiter::kLookahead - 1;
  // quiet: 0.9 peak, nothing changes
  {
    auto in = Signal(ch, kFrames, [](size_t) { return 0.9f; });
    auto out = in;
    PeakLimiter l;
    l.Reset(ch);
    LimiterStats st;
    for (size_t at = 0; at < kFrames; at += 256) l.Process(out.data() + at * ch, 256, st);
    bool equal = true;
    for (size_t i = 0; i < (kFrames - kDelay) * ch; ++i) equal = equal && out[i + kDelay * ch] == in[i];
    Check(equal, "0.9 peak: output == input delayed by 63 frames, bit for bit");
    Check(st.limited_frames == 0 && st.min_gain == 1.0f, "0.9 peak: no frame limited");
  }
  // burst of +6 dBFS (2.0) from 0.5 s to 0.7 s, a +12 dBFS one-sample spike at 1.0 s, quiet otherwise
  {
    auto in = Signal(ch, kFrames, [](size_t n) { return n >= 24000 && n < 33600 ? 2.0f : 0.5f; });
    in[48000 * ch] = 4.0f;
    auto out = in;
    PeakLimiter l;
    l.Reset(ch);
    LimiterStats st;
    for (size_t at = 0; at < kFrames; at += 256) l.Process(out.data() + at * ch, 256, st);
    float peak = 0;
    for (float v : out) peak = std::max(peak, std::fabs(v));
    char line[160];
    std::snprintf(line, sizeof line, "burst: peak in %.3f, out %.4f <= ceiling %.3f (limited frames %llu, min gain %.3f)",
                  st.peak_in, peak, PeakLimiter::kCeiling, (unsigned long long)st.limited_frames, st.min_gain);
    Check(peak <= PeakLimiter::kCeiling * 1.000001f, line);
    // 0.2 s before the burst (well before the look-ahead) the output equals the input
    bool before = true;
    for (size_t n = 14400; n < 24000 - 2 * PeakLimiter::kLookahead; ++n)
      for (unsigned c = 0; c < ch; ++c) before = before && out[(n + kDelay) * ch + c] == in[n * ch + c];
    Check(before, "before the burst: bit for bit");
    // 1.5 s after the spike (release time constant 0.1 s, back to exactly 1 below 1e-5) the gain is 1 again
    bool after = true;
    for (size_t n = 48000 + 72000; n < kFrames - kDelay; ++n)
      for (unsigned c = 0; c < ch; ++c) after = after && out[(n + kDelay) * ch + c] == in[n * ch + c];
    Check(after, "after the release: bit for bit again");
  }
}

}  // namespace

// limiter_selftest <mix.wav>: the 6 channel mix of FCP_RECORD_AUDIO through the limiter as a 5.1 device would get it
// (MixSurround with weights 1 and gain 1 = the mix itself), in periods of 256 frames as the SDK does.
int Replay(const char* path) {
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return std::printf("cannot open %s\n", path), 2;
  std::vector<unsigned char> d;
  unsigned char buf[1 << 16];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  size_t pos = 12, data = 0, bytes = 0;
  while (pos + 8 <= d.size()) {
    const unsigned size = d[pos + 4] | d[pos + 5] << 8 | d[pos + 6] << 16 | unsigned(d[pos + 7]) << 24;
    if (std::memcmp(&d[pos], "data", 4) == 0) {
      data = pos + 8;
      bytes = std::min<size_t>(size, d.size() - data);
      break;
    }
    pos += 8 + size + (size & 1);
  }
  if (!data) return std::printf("%s: no data\n", path), 2;
  const size_t frames = bytes / 24 / 256 * 256;
  std::vector<float> in(frames * 6);
  std::memcpy(in.data(), &d[data], in.size() * 4);
  auto out = in;
  PeakLimiter l;
  l.Reset(6);
  LimiterStats st;
  for (size_t at = 0; at < frames; at += 256) l.Process(out.data() + at * 6, 256, st);
  const unsigned kDelay = PeakLimiter::kLookahead - 1;
  size_t changed = 0, over_in[6] = {}, over_out = 0, below_01db = 0, below_1db = 0;
  float peak_in[6] = {}, peak_out = 0;
  for (size_t n = 0; n + kDelay < frames; ++n) {
    bool c = false;
    for (unsigned k = 0; k < 6; ++k) {
      const float a = std::fabs(in[n * 6 + k]), b = std::fabs(out[(n + kDelay) * 6 + k]);
      peak_in[k] = std::max(peak_in[k], a);
      over_in[k] += a > 1.0f;
      peak_out = std::max(peak_out, b);
      over_out += b > 1.0f;
      c = c || out[(n + kDelay) * 6 + k] != in[n * 6 + k];
    }
    changed += c;
    // the frame's gain, from its largest sample (only where there is one to measure)
    float a_in = 0, a_out = 0;
    for (unsigned k = 0; k < 6; ++k)
      if (std::fabs(in[n * 6 + k]) > a_in) a_in = std::fabs(in[n * 6 + k]), a_out = std::fabs(out[(n + kDelay) * 6 + k]);
    if (a_in > 1e-3f) {
      below_01db += a_out < a_in * 0.98855f;
      below_1db += a_out < a_in * 0.89125f;
    }
  }
  std::printf("%s: %zu frames (%.1f s) through the 6 channel limiter\n", path, frames, frames / 48000.0);
  const char* names[6] = {"FL", "FR", "C", "LFE", "SL", "SR"};
  for (unsigned k = 0; k < 6; ++k)
    std::printf("  before %-3s peak %.4f (%+.2f dBFS), samples > 1.0: %zu\n", names[k], peak_in[k],
                20 * std::log10(std::max(peak_in[k], 1e-9f)), over_in[k]);
  std::printf("  after: peak %.4f (%+.2f dBFS), samples > 1.0: %zu; frames changed %zu (%.4f %%), min gain %.3f (%+.2f dB)\n",
              peak_out, 20 * std::log10(std::max(peak_out, 1e-9f)), over_out, changed, 100.0 * changed / frames,
              st.min_gain, 20 * std::log10(st.min_gain));
  std::printf("  frames turned down by more than 0.1 dB: %zu (%.4f %%), by more than 1 dB: %zu (%.4f %%)\n", below_01db,
              100.0 * below_01db / frames, below_1db, 100.0 * below_1db / frames);
  return 0;
}

int main(int argc, char** argv) {
  if (argc > 1) return Replay(argv[1]);
  Run(2);
  Run(6);
  std::printf("%s\n", g_failures ? "FAILED" : "all passed");
  return g_failures ? 1 : 0;
}
