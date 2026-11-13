#include "fcp/framerate.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
// windows.h first
#include <timeapi.h>

#include "fcp/engine_hooks.h"
#include "fcp/frame_coupling.h"
#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/log.h"
#include "fcp_addresses.h"

namespace fcp::framerate {
namespace {

using Clock = std::chrono::steady_clock;

int g_target = 30;          // reported target, 0 = unlimited
int g_vsynch = -1;          // value forced into r_vsynch every frame, -1 = leave the game's value
double g_limit_period = 0;  // host limiter: shortest time between two frames, 0 = none
bool g_logged_vsynch = false;
Clock::time_point g_next_frame;

// statistics
double g_stats_window = 10.0;
Clock::time_point g_last_frame;
bool g_have_last = false;
Clock::time_point g_window_start;
double g_window_game0 = 0.0;
uint64_t g_window_bink0 = 0;   // Bink_NextFrame calls at the window start (video frames)
uint64_t g_window_shots0 = 0;  // Weapon_Fire calls at the window start (shots)
uint64_t g_window_ai0 = 0;     // AI turns (FCP_TRACE_AI) at the window start
std::vector<float> g_window_ms;  // frame times of the window
std::vector<float> g_all_ms;     // all frame times after the first window (summary at the end)

void Limit() {
  if (g_limit_period <= 0.0) {
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(g_limit_period));
  Clock::time_point now = Clock::now();
  if (g_next_frame.time_since_epoch().count() == 0 || now > g_next_frame + period * 4) {
    g_next_frame = now;  // first frame or far behind (loading): re-anchor
  }
  // Sleep coarse, spin the last 1.5 ms (timeBeginPeriod(1) is set).
  while (now < g_next_frame) {
    const auto left = g_next_frame - now;
    if (left > std::chrono::microseconds(2000)) {
      std::this_thread::sleep_for(left - std::chrono::microseconds(1500));
    } else {
      std::this_thread::yield();
    }
    now = Clock::now();
  }
  // Minimum distance between frames: never more than one period of credit.
  g_next_frame = std::max(g_next_frame + period, now);
}

void ApplyVsynch(uint8_t* base) {
  if (g_vsynch < 0 || !fcp::guest::Readable(base, fcp::addr::r_vsynch, 4)) {
    return;
  }
  const uint32_t old = fcp::guest::U32(base, fcp::addr::r_vsynch);
  if (old != uint32_t(g_vsynch)) {
    const uint32_t be = __builtin_bswap32(uint32_t(g_vsynch));
    std::memcpy(fcp::guest::Ptr(base, fcp::addr::r_vsynch), &be, 4);
    if (!g_logged_vsynch) {
      g_logged_vsynch = true;
      FCPLOG("framerate: r_vsynch {} -> {} (frame {})", old, g_vsynch, fcp::gametime::Frame());
    }
  }
}

struct Summary {
  size_t n = 0;
  double mean = 0, p50 = 0, p99 = 0, max = 0, low1 = 0;
};

Summary Summarize(std::vector<float> ms) {
  Summary s;
  s.n = ms.size();
  if (ms.empty()) {
    return s;
  }
  std::sort(ms.begin(), ms.end());
  s.mean = std::accumulate(ms.begin(), ms.end(), 0.0) / double(ms.size());
  s.p50 = ms[ms.size() / 2];
  s.p99 = ms[std::min(ms.size() - 1, size_t(double(ms.size()) * 0.99))];
  s.max = ms.back();
  // 1 % low: frame rate over the slowest 1 % of the frames (at least one frame).
  const size_t worst = std::max<size_t>(1, ms.size() / 100);
  const double worst_mean = std::accumulate(ms.end() - worst, ms.end(), 0.0) / double(worst);
  s.low1 = worst_mean > 0 ? 1000.0 / worst_mean : 0;
  return s;
}

// Coarse histogram of the window: <8, 8-12, 12-20, 20-30, 30-40, 40-60, >=60 ms.
std::string Histogram(const std::vector<float>& ms) {
  static constexpr float kEdges[] = {8.f, 12.f, 20.f, 30.f, 40.f, 60.f};
  size_t bins[7] = {};
  for (float v : ms) {
    size_t b = 0;
    while (b < 6 && v >= kEdges[b]) {
      ++b;
    }
    ++bins[b];
  }
  return std::format("<8:{} 8-12:{} 12-20:{} 20-30:{} 30-40:{} 40-60:{} >=60:{}", bins[0], bins[1], bins[2], bins[3],
                     bins[4], bins[5], bins[6]);
}

void Stats(double game_seconds) {
  const Clock::time_point now = Clock::now();
  if (g_have_last) {
    const float ms = float(std::chrono::duration<double, std::milli>(now - g_last_frame).count());
    g_window_ms.push_back(ms);
  } else {
    g_window_start = now;
    g_window_game0 = game_seconds;
    g_window_bink0 = fcp::engine::CountOf("Bink_NextFrame");
    g_window_shots0 = fcp::engine::CountOf("Weapon_Fire");
    g_window_ai0 = fcp::frame_coupling::AiTurns();
  }
  g_last_frame = now;
  g_have_last = true;
  if (g_stats_window <= 0.0) {
    return;
  }
  const double wall = std::chrono::duration<double>(now - g_window_start).count();
  if (wall < g_stats_window) {
    return;
  }
  const Summary s = Summarize(g_window_ms);
  const double game = game_seconds - g_window_game0;
  FCPLOG("framestat: target {} | {} frames in {:.2f} s = {:.2f} fps | ms mean {:.2f} p50 {:.2f} p99 {:.2f} max {:.1f} | "
         "1% low {:.1f} fps | game/wall {:.3f} | level {} {:.1f} s | {} | video frames {} shots {} ai turns {}",
         g_target == 0 ? std::string("unlimited") : std::to_string(g_target), s.n, wall, double(s.n) / wall, s.mean,
         s.p50, s.p99, s.max, s.low1, game / wall,
         fcp::gametime::InLevel() ? fcp::gametime::LevelName() : std::string("-"), fcp::gametime::LevelSeconds(),
         Histogram(g_window_ms), fcp::engine::CountOf("Bink_NextFrame") - g_window_bink0,
         fcp::engine::CountOf("Weapon_Fire") - g_window_shots0, fcp::frame_coupling::AiTurns() - g_window_ai0);
  g_all_ms.insert(g_all_ms.end(), g_window_ms.begin(), g_window_ms.end());
  g_window_ms.clear();
  g_window_start = now;
  g_window_game0 = game_seconds;
  g_window_bink0 = fcp::engine::CountOf("Bink_NextFrame");
  g_window_shots0 = fcp::engine::CountOf("Weapon_Fire");
  g_window_ai0 = fcp::frame_coupling::AiTurns();
}

int ParseTarget(const std::string& v, bool zero_unlimited) {
  if (v == "unlimited" || v == "uncapped" || (zero_unlimited && v == "0")) {
    return 0;
  }
  const int n = std::atoi(v.c_str());
  return n > 0 ? n : 30;
}

}  // namespace

void Configure() {
  g_stats_window = double(fcp::env::Int("FCP_FPS_STATS", 10));
  std::string from;
  std::string value = fcp::env::Get("FCP_FPS");
  if (!value.empty()) {
    from = "FCP_FPS";
    g_target = ParseTarget(value, false);
  } else if (value = fcp::env::Get("FCP_FPS_LIMIT"); !value.empty()) {
    from = "FCP_FPS_LIMIT";
    g_target = ParseTarget(value, true);
  }
  if (!from.empty()) {
    if (g_target == 30) {
      g_vsynch = -1;  // original: the game's r_vsynch (2)
    } else if (g_target == 60 && fcp::env::Get("FCP_FPS_PACING") != "limiter") {
      g_vsynch = 1;  // one vblank of the null driver (60 Hz), as the console does at interval ONE
    } else {
      g_vsynch = 0;  // immediate: SynchronizeToPresentationInterval writes no vblank wait
      g_limit_period = g_target > 0 ? 1.0 / g_target : 0.0;
    }
  }
  if (const std::string iv = fcp::env::Get("FCP_PRESENT_INTERVAL"); !iv.empty()) {
    g_vsynch = iv == "immediate" || iv == "0" ? 0 : int(std::clamp<int64_t>(fcp::env::Int("FCP_PRESENT_INTERVAL", 2), 1, 2));
    FCPLOG("FCP_PRESENT_INTERVAL={}: r_vsynch {}", iv, g_vsynch);
  }
  if (g_limit_period > 0.0) {
    timeBeginPeriod(1);
  }
  if (!from.empty()) {
    FCPLOG("{}={}: target {}, r_vsynch {}, host limiter {}{}", from, value,
           g_target == 0 ? std::string("unlimited") : std::to_string(g_target),
           g_vsynch < 0 ? std::string("original") : std::to_string(g_vsynch),
           g_limit_period > 0.0 ? std::format("{:.3f} ms", g_limit_period * 1000.0) : std::string("off"),
           g_target > 120 || g_target == 0 ? " (over the verified 120)" : "");
  }
}

void OnMainLoop(uint8_t* base, double game_seconds) {
  Limit();
  ApplyVsynch(base);
  Stats(game_seconds);
}

void Shutdown() {
  if (g_limit_period > 0.0) {
    timeEndPeriod(1);
  }
  if (!g_all_ms.empty()) {
    const Summary s = Summarize(g_all_ms);
    FCPLOG("framestat total: {} frames | ms mean {:.2f} p50 {:.2f} p99 {:.2f} max {:.1f} | 1% low {:.1f} fps", s.n,
           s.mean, s.p50, s.p99, s.max, s.low1);
  }
}

int TargetFps() {
  return g_target;
}

bool HostPresentImmediate() {
  // May run before Configure (the renderer module starts first), so it reads the switches itself.
  std::string v = fcp::env::Get("FCP_FPS");
  const int target = !v.empty() ? ParseTarget(v, false)
                     : !(v = fcp::env::Get("FCP_FPS_LIMIT")).empty() ? ParseTarget(v, true)
                                                                     : 30;
  return target == 0 || target > 60;
}

}  // namespace fcp::framerate
