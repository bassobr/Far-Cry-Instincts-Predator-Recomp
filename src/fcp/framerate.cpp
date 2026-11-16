#include "fcp/framerate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
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
#include "fcp/render_bridge.h"
#include "fcp_addresses.h"

namespace fcp::framerate {
namespace {

using Clock = std::chrono::steady_clock;

// The plan in force. Written by Configure / Reconfigure (title launch, settings menu on the UI thread, the auto
// watcher), read by the main thread (OnMainLoop) and the game's threads (TargetFps in the frame coupling hooks).
std::atomic<int> g_target{30};              // reported target, 0 = unlimited
std::atomic<int> g_vsynch{-1};              // value forced into r_vsynch every frame, -1 = leave the game's value
std::atomic<double> g_limit_period{0.0};    // host limiter: shortest time between two frames, 0 = none
std::atomic<bool> g_display{false};         // FCP_FPS_PACING=display requested (and the target can be paced)
std::atomic<bool> g_logged_vsynch{false};
std::atomic<bool> g_restart_window{false};  // the plan changed: the main thread re-anchors the limiter, new stats window
std::atomic<bool> g_is_auto{false};         // FCP_FPS=auto in force
bool g_display_on = false;   // the renderer paces on the display right now (main thread)
int64_t g_game_vsynch = -1;  // the game's own r_vsynch before the first write (restored when no longer forced)
double g_pace_limit = 0.0;   // FCP_PACE_LIMIT: limiter at this factor of the display's frame period (0: none)
Clock::time_point g_next_frame;
std::mutex g_config_mutex;   // Configure / Reconfigure
bool g_configured = false;
std::atomic<bool> g_time_period{false};  // timeBeginPeriod(1) is set

// statistics (main thread)
double g_stats_window = 10.0;
Clock::time_point g_last_frame;
bool g_have_last = false;
Clock::time_point g_window_start;
double g_window_game0 = 0.0;
uint64_t g_window_bink0 = 0;   // Bink_NextFrame calls at the window start (video frames)
uint64_t g_window_shots0 = 0;  // Weapon_Fire calls at the window start (shots)
uint64_t g_window_ai0 = 0;     // AI turns (FCP_TRACE_AI) at the window start
int g_window_target = 30;      // the target the window's frames ran at (a window ended early keeps the old one)
std::vector<float> g_window_ms;  // frame times of the window
std::vector<float> g_all_ms;     // all frame times after the first window (summary at the end)

void Limit(double period_s) {
  if (period_s <= 0.0) {
    return;
  }
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period_s));
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
  int want = g_vsynch.load(std::memory_order_relaxed);
  if (g_display_on) {
    want = 0;  // the renderer waits for the display: no vblank wait of the null driver on top
  } else if (want < 0 && g_game_vsynch >= 0) {
    want = int(g_game_vsynch);  // not forced (any more): the game's own value again
  }
  if (want < 0 || !fcp::guest::Readable(base, fcp::addr::r_vsynch, 4)) {
    return;
  }
  const uint32_t old = fcp::guest::U32(base, fcp::addr::r_vsynch);
  if (g_game_vsynch < 0) {
    g_game_vsynch = old;
  }
  if (old != uint32_t(want)) {
    const uint32_t be = __builtin_bswap32(uint32_t(want));
    std::memcpy(fcp::guest::Ptr(base, fcp::addr::r_vsynch), &be, 4);
    if (!g_logged_vsynch.exchange(true)) {
      FCPLOG("framerate: r_vsynch {} -> {} (frame {})", old, want, fcp::gametime::Frame());
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
  const size_t over25 = size_t(std::count_if(ms.begin(), ms.end(), [](float v) { return v > 25.f; }));
  // long: over 1.5 frame periods of the target (a visible hitch at that rate; unlimited: 25 ms)
  const int target = g_window_target;
  const float long_ms = target > 0 ? 1500.f / float(target) : 25.f;
  const size_t longer = size_t(std::count_if(ms.begin(), ms.end(), [&](float v) { return v > long_ms; }));
  return std::format("<8:{} 8-12:{} 12-20:{} 20-30:{} 30-40:{} 40-60:{} >=60:{} >25:{} long:{}", bins[0], bins[1],
                     bins[2], bins[3], bins[4], bins[5], bins[6], over25, longer);
}

std::string TargetText(int target) { return target == 0 ? std::string("unlimited") : std::to_string(target); }

void StartWindow(Clock::time_point now, double game_seconds) {
  g_window_start = now;
  g_window_target = g_target.load(std::memory_order_relaxed);
  g_window_game0 = game_seconds;
  g_window_bink0 = fcp::engine::CountOf("Bink_NextFrame");
  g_window_shots0 = fcp::engine::CountOf("Weapon_Fire");
  g_window_ai0 = fcp::frame_coupling::AiTurns();
}

// force: the plan changed; the window so far is logged (when it has frames) and a new one starts
void Stats(double game_seconds, bool force) {
  const Clock::time_point now = Clock::now();
  if (g_have_last) {
    const float ms = float(std::chrono::duration<double, std::milli>(now - g_last_frame).count());
    g_window_ms.push_back(ms);
  } else {
    StartWindow(now, game_seconds);
  }
  g_last_frame = now;
  g_have_last = true;
  if (g_stats_window <= 0.0) {
    return;
  }
  const double wall = std::chrono::duration<double>(now - g_window_start).count();
  if (wall < g_stats_window && !force) {
    return;
  }
  if (!g_window_ms.empty() && wall > 0.0) {
    const Summary s = Summarize(g_window_ms);
    const double game = game_seconds - g_window_game0;
    FCPLOG("framestat: target {} | {} frames in {:.2f} s = {:.2f} fps | ms mean {:.2f} p50 {:.2f} p99 {:.2f} max {:.1f} | "
           "1% low {:.1f} fps | game/wall {:.3f} | level {} {:.1f} s | {} | video frames {} shots {} ai turns {}{}",
           TargetText(g_window_target), s.n, wall, double(s.n) / wall, s.mean, s.p50, s.p99,
           s.max, s.low1, game / wall, fcp::gametime::InLevel() ? fcp::gametime::LevelName() : std::string("-"),
           fcp::gametime::LevelSeconds(), Histogram(g_window_ms), fcp::engine::CountOf("Bink_NextFrame") - g_window_bink0,
           fcp::engine::CountOf("Weapon_Fire") - g_window_shots0, fcp::frame_coupling::AiTurns() - g_window_ai0,
           force ? " | window ended early: the frame rate changed" : "");
    g_all_ms.insert(g_all_ms.end(), g_window_ms.begin(), g_window_ms.end());
  }
  g_window_ms.clear();
  StartWindow(now, game_seconds);
}

// ---- [display] fps = auto ----
// The refresh rate of the monitor the game window is on (before the window: the primary monitor), as the system
// reports it. 59, 119, ... (the NTSC rates 59.94, 119.88 the system rounds down) count as 60, 120.
int MonitorHz() {
  // test: FCP_FPS_AUTO_HZ=<hz> stands for the monitor's refresh rate (auto on a 75 or 144 Hz display without one)
  int hz = int(fcp::env::Int("FCP_FPS_AUTO_HZ", 0));
  if (hz <= 0) {
    HWND hwnd = static_cast<HWND>(fcp::render::WindowHandle());
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!monitor) monitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!monitor || !GetMonitorInfoW(monitor, &info) || !EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
      return 0;
    }
    hz = int(dm.dmDisplayFrequency);
  }
  if (hz % 10 == 9) ++hz;
  return hz;
}

// auto: the refresh rate, at most 120 (the verified ceiling; 240 with unsupported_uncapped_fps) and at least kMinFree
// (below that the frame coupling corrections are not active for every frame, see framerate.h): 30 then. Unknown (0, 1:
// virtual displays): 60.
int AutoFromHz(int hz) {
  if (hz < 24 || hz > 1000) hz = 60;
  if (hz > MaxFps()) return MaxFps();
  if (hz < kMinFree) return 30;
  return hz;
}

// The watcher's state lives as long as the process (the thread is detached; nothing to destroy at exit).
struct AutoState {
  std::mutex mutex;
  std::condition_variable cv;
  bool started = false, stop = false;
  int hz = 0;  // the refresh the plan was made with
};
AutoState& Auto() {
  static AutoState* state = new AutoState;
  return *state;
}

void Reconfig(const char* why, bool push_renderer);

// While auto is in force: every 2 s the refresh rate of the window's monitor (window dragged to another monitor,
// display mode changed); a change makes a new plan. Off the game's threads (EnumDisplaySettings takes a while).
void AutoWatch() {
  AutoState& a = Auto();
  std::unique_lock lock(a.mutex);
  while (!a.stop) {
    a.cv.wait_for(lock, std::chrono::seconds(2));
    if (a.stop) break;
    if (!g_is_auto.load()) continue;
    lock.unlock();
    const int hz = MonitorHz();
    lock.lock();
    if (hz > 0 && hz != a.hz && !a.stop) {
      const int before = a.hz;
      a.hz = hz;
      lock.unlock();
      FCPLOG("framerate: auto: the window's monitor runs at {} Hz now (before {} Hz)", hz, before);
      Reconfig("auto: refresh rate changed", true);
      lock.lock();
    }
  }
}

void StartAutoWatch() {
  AutoState& a = Auto();
  std::lock_guard lock(a.mutex);
  if (a.started) return;
  a.started = true;
  a.stop = false;
  std::thread(AutoWatch).detach();
}
// ---- end [display] fps = auto ----

// 0 = unlimited; "auto" = AutoTarget()
int ParseTarget(const std::string& v, bool zero_unlimited, bool* is_auto) {
  if (is_auto) *is_auto = false;
  if (v == "auto") {
    if (is_auto) *is_auto = true;
    return AutoTarget();
  }
  if (v == "unlimited" || v == "uncapped" || (zero_unlimited && v == "0")) {
    return 0;
  }
  const int n = std::atoi(v.c_str());
  return n > 0 ? n : 30;
}

// The configured target from the switches (also before Configure: the renderer module starts first).
int ConfiguredTarget(bool* is_auto = nullptr, std::string* from = nullptr, std::string* value = nullptr) {
  std::string v = fcp::env::Get("FCP_FPS");
  if (!v.empty()) {
    if (from) *from = "FCP_FPS";
    if (value) *value = v;
    return ParseTarget(v, false, is_auto);
  }
  if (!(v = fcp::env::Get("FCP_FPS_LIMIT")).empty()) {
    if (from) *from = "FCP_FPS_LIMIT";
    if (value) *value = v;
    return ParseTarget(v, true, is_auto);
  }
  if (is_auto) *is_auto = false;
  return 30;
}

void EnsureTimePeriod() {
  if (!g_time_period.exchange(true)) timeBeginPeriod(1);
}

// The plan from the switches: target, r_vsynch, host limiter, display pacing; the renderer follows (present mode,
// display pacing target) unless this is the first configuration (the renderer read the same switches at its start).
void Reconfig(const char* why, bool push_renderer) {
  std::lock_guard lock(g_config_mutex);
  std::string from, value;
  bool is_auto = false;
  const int target = ConfiguredTarget(&is_auto, &from, &value);
  int vsynch = -1;
  double period = 0.0;
  if (!from.empty()) {
    if (target == 30) {
      vsynch = -1;  // original: the game's r_vsynch (2)
    } else if (target == 60 && fcp::env::Get("FCP_FPS_PACING") != "limiter") {
      vsynch = 1;  // one vblank of the null driver (60 Hz), as the console does at interval ONE
    } else {
      vsynch = 0;  // immediate: SynchronizeToPresentationInterval writes no vblank wait
      period = target > 0 ? 1.0 / target : 0.0;
    }
  }
  if (const std::string iv = fcp::env::Get("FCP_PRESENT_INTERVAL"); !iv.empty()) {
    vsynch = iv == "immediate" || iv == "0" ? 0 : int(std::clamp<int64_t>(fcp::env::Int("FCP_PRESENT_INTERVAL", 2), 1, 2));
    FCPLOG("FCP_PRESENT_INTERVAL={}: r_vsynch {}", iv, vsynch);
  }
  const int before = g_target.load();
  const double pace = DisplayPacingTarget();
  g_target = target;
  g_vsynch = vsynch;
  g_limit_period = period;
  g_display = pace != 0.0;
  g_is_auto = is_auto;
  g_logged_vsynch = false;
  if (period > 0.0 || pace != 0.0) EnsureTimePeriod();
  if (g_configured) g_restart_window = true;
  int auto_hz = 0;
  if (is_auto) {
    auto_hz = MonitorHz();
    {
      std::lock_guard alock(Auto().mutex);
      Auto().hz = auto_hz;
    }
    StartAutoWatch();
  }
  if (pace != 0.0) {
    FCPLOG("FCP_FPS_PACING=display: the renderer paces on the display's refresh once it is measured and divisible by "
           "the target; until then and otherwise the pacing below");
  }
  if (!from.empty() || g_configured) {
    FCPLOG("{}{}={}: target {}{}, r_vsynch {}, host limiter {}{}{}", g_configured ? std::format("framerate ({}): ", why) : "",
           from.empty() ? "FCP_FPS" : from, value.empty() ? std::string("(unset)") : value, TargetText(target),
           is_auto ? std::format(" (auto: monitor {} Hz)", auto_hz) : "",
           vsynch < 0 ? std::string("original") : std::to_string(vsynch),
           period > 0.0 ? std::format("{:.3f} ms", period * 1000.0) : std::string("off"),
           target > kMaxFps || target == 0
               ? (UncappedAllowed() && target > 0 && target <= kMaxUnsupported
                      ? " (over 120: UNSUPPORTED, unsupported_uncapped_fps)"
                      : " (over the verified 120)")
               : "",
           g_configured && before != target ? std::format(" (was {})", TargetText(before)) : "");
  }
  if (push_renderer) {
    fcp::render::SetPresentVsync(HostVsync());
    fcp::render::SetPaceTarget(pace);
  }
  g_configured = true;
}

}  // namespace

void Configure() {
  g_stats_window = double(fcp::env::Int("FCP_FPS_STATS", 10));
  if (const std::string pl = fcp::env::Get("FCP_PACE_LIMIT"); !pl.empty()) {
    g_pace_limit = std::clamp(std::atof(pl.c_str()), 0.0, 1.0);
  }
  // Configure may run twice: the second time like a change (the renderer already runs)
  Reconfig("configure", g_configured);
}

bool LiveChanges() { return fcp::env::Flag("FCP_FPS_LIVE", true); }

bool UncappedAllowed() { return fcp::env::Flag("FCP_UNSUPPORTED_UNCAPPED_FPS"); }

int MaxFps() { return UncappedAllowed() ? kMaxUnsupported : kMaxFps; }

bool Reconfigure(const char* why) {
  if (!LiveChanges()) {
    FCPLOG("framerate: {}: not applied now (FCP_FPS_LIVE=0: from the next start)", why);
    return false;
  }
  Reconfig(why, true);
  return true;
}

void OnMainLoop(uint8_t* base, double game_seconds) {
  const bool restart = g_restart_window.exchange(false);
  if (restart) {
    g_next_frame = {};  // the limiter starts anew with the new period
  }
  if (g_display.load(std::memory_order_relaxed) || g_display_on) {
    const bool on = fcp::render::DisplayPacingActive();
    if (on != g_display_on) {
      g_display_on = on;
      FCPLOG("framerate: display pacing {} (frame {}): {}", on ? "on" : "off", fcp::gametime::Frame(),
             on ? "r_vsynch 0, no host limiter" : "the game's own pacing");
      g_next_frame = {};  // the limiter starts anew when it is needed again
    }
  }
  if (!g_display_on) {
    Limit(g_limit_period.load(std::memory_order_relaxed));
  } else if (g_pace_limit > 0.0) {
    // Display pacing with the limiter at the display's own frame period (a little shorter, so the display, not the
    // limiter, sets the rate): the main thread waits here as with the plain limiter instead of the render thread
    // waiting for the display (FCP_PACE_LIMIT=<factor>).
    const double ms = fcp::render::DisplayFrameMs();
    if (ms > 0.0) {
      Limit(ms * g_pace_limit / 1000.0);
    }
  }
  ApplyVsynch(base);
  Stats(game_seconds, restart);
}

void Shutdown() {
  {
    std::lock_guard lock(Auto().mutex);
    Auto().stop = true;
  }
  Auto().cv.notify_all();
  if (g_time_period.exchange(false)) {
    timeEndPeriod(1);
  }
  if (!g_all_ms.empty()) {
    const Summary s = Summarize(g_all_ms);
    FCPLOG("framestat total: {} frames | ms mean {:.2f} p50 {:.2f} p99 {:.2f} max {:.1f} | 1% low {:.1f} fps", s.n,
           s.mean, s.p50, s.p99, s.max, s.low1);
  }
}

int TargetFps() {
  return g_target.load(std::memory_order_relaxed);
}

int AutoTarget() { return AutoFromHz(MonitorHz()); }

bool HostPresentImmediate() {
  if (DisplayPacingTarget() != 0.0) {
    return false;  // display pacing needs FIFO: the image waits for its refresh
  }
  const int target = ConfiguredTarget();
  return target == 0 || target > 60;
}

bool HostVsync() { return fcp::env::Flag("FCP_RENDER_VSYNC", !HostPresentImmediate()); }

double DisplayPacingTarget() {
  if (fcp::env::Get("FCP_FPS_PACING") != "display" || !fcp::env::Get("FCP_PRESENT_INTERVAL").empty()) {
    return 0.0;
  }
  const int target = ConfiguredTarget();
  // Over 60 (and unlimited) the display cannot be paced on the Deck's 90 Hz with frames of two refreshes or more,
  // and the FIFO present it needs cost 7 fps at 90 (measured: 57 instead of 64 in 21_dunea): the game's own pacing.
  if (target == 0 || target > 60) {
    return 0.0;
  }
  return double(target);
}

}  // namespace fcp::framerate
