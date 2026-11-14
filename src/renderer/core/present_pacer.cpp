// PresentPacer (renderer/present_pacer.h). This project's own code.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "renderer/present_pacer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>

#include "renderer/diagnostics.h"
#include "renderer/host_device.h"

namespace fcp::nr {
namespace {

using Clock = PresentPacer::Clock;

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
Clock::duration FromMs(double ms) {
  return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms));
}

// Sleeps coarse, then spins the last stretch (Wine's sleeps are fine-grained, Windows' are 1 ms with timeBeginPeriod).
void SleepUntil(Clock::time_point t) {
  for (Clock::time_point now = Clock::now(); now < t; now = Clock::now()) {
    const auto left = t - now;
    if (left > std::chrono::microseconds(1500)) std::this_thread::sleep_for(left - std::chrono::microseconds(1000));
    else std::this_thread::yield();
  }
}

double Percentile(std::vector<float> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(double(v.size()) * q))];
}

// Share of the intervals (>= 2 ms) that are a whole multiple (1..8) of t within the tolerance.
double Fit(const std::vector<float>& iv, double t) {
  size_t ok = 0, n = 0;
  const double tol = std::max(0.8, 0.06 * t);
  for (float v : iv) {
    if (v < 2.0f) continue;
    ++n;
    const double k = std::round(v / t);
    if (k >= 1 && k <= 8 && std::fabs(v - k * t) <= tol) ++ok;
  }
  return n ? double(ok) / double(n) : 0.0;
}

// Mean of interval / k over the fitting intervals: the period with less jitter than one sample.
double Refine(const std::vector<float>& iv, double t) {
  double sum = 0, count = 0;
  const double tol = std::max(0.8, 0.06 * t);
  for (float v : iv) {
    const double k = std::round(v / t);
    if (v < 2.0f || k < 1 || k > 8 || std::fabs(v - k * t) > tol) continue;
    sum += v, count += k;
  }
  return count > 0 ? sum / count : t;
}

}  // namespace

PresentPacer::~PresentPacer() { Stop(); }

void PresentPacer::Start(HostDevice* host, const PacerOptions& options) {
  Stop();
  std::lock_guard lock(mutex_);
  host_ = host;
  opt_ = options;
  stop_ = false;
  pending_.clear();
  presented_ = displayed_ = displayed_swap_ = 0;
  displayed_swap_atomic_ = 0;
  have_display_ = false;
  lost_in_row_ = 0;
  active_ = false;
  refresh_ms_ = 0.0;
  cadence_ = 0;
  recent_.clear();
  window_start_ = Clock::now();
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  uint32_t hz = 0;
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24 && dm.dmDisplayFrequency <= 500)
    hz = dm.dmDisplayFrequency, prior_ms_ = 1000.0 / hz;
  const bool timing = host && host->has_present_wait();
  char target[32];
  if (opt_.target_fps > 0) std::snprintf(target, sizeof(target), "%.0f fps", opt_.target_fps);
  else std::snprintf(target, sizeof(target), "%s", opt_.target_fps < 0 ? "every refresh" : "off");
  LogLine("renderer: present timing %s (system refresh %u Hz); display pacing %s%s, queue %u, hold %.2f refresh, early %.1f ms, spin %.1f ms",
          timing ? "from the display (present wait)" : "not available, present intervals only", hz, target,
          opt_.target_fps != 0 && !timing ? " (needs present wait: the game paces itself)" : "", opt_.queued, opt_.hold,
          opt_.early_ms, opt_.spin_ms);
  if (timing) thread_ = std::thread([this] { Run(); });
}

void PresentPacer::Stop() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
    active_ = false;
    frame_ms_ = 0.0;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void PresentPacer::Run() {
  std::unique_lock lk(mutex_);
  while (!stop_) {
    if (pending_.empty()) {
      cv_.wait_for(lk, std::chrono::milliseconds(100));
      if (opt_.stats_seconds > 0 && Clock::now() - window_start_ >= FromMs(opt_.stats_seconds * 1000)) LogWindow(Clock::now());
      continue;
    }
    const Pending p = pending_.front();
    lk.unlock();
    const auto t_wait = Clock::now();
    const bool ok = host_->WaitPresented(p.id, 20'000'000);  // 20 ms, then look at stop_ again
    const auto t = Clock::now();
    if (!ok && t - t_wait < std::chrono::milliseconds(1)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    lk.lock();
    if (ok) {
      pending_.pop_front();
      OnDisplayed(p, t);
      lost_in_row_ = 0;
      cv_.notify_all();
    } else if (t - p.presented > std::chrono::milliseconds(250)) {
      // Never seen on the display (window hidden, swapchain replaced): count it as gone so no wait hangs on it.
      pending_.pop_front();
      ++displayed_;
      displayed_swap_ = std::max(displayed_swap_, p.swap);
      displayed_swap_atomic_.store(displayed_swap_, std::memory_order_release);
      have_display_ = false;
      ++w_lost_;
      if (++lost_in_row_ == 3 && active_) {
        LogLine("renderer: display pacing paused: presents do not reach the display");
        UpdateCadence();
      }
      cv_.notify_all();
    }
    if (opt_.stats_seconds > 0 && t - window_start_ >= FromMs(opt_.stats_seconds * 1000)) LogWindow(t);
  }
}

void PresentPacer::OnDisplayed(const Pending& p, Clock::time_point t) {
  ++displayed_;
  displayed_swap_ = std::max(displayed_swap_, p.swap);
  displayed_swap_atomic_.store(displayed_swap_, std::memory_order_release);
  w_latency_.push_back(float(Ms(t - p.presented)));
  if (have_display_) {
    const float iv = float(Ms(t - last_display_));
    w_display_.push_back(iv);
    recent_.push_back(iv);
    if (recent_.size() > 240) recent_.erase(recent_.begin(), recent_.begin() + 60);
    const double period = refresh_ms_.load(std::memory_order_relaxed);
    if (period > 0) {
      const uint32_t r = uint32_t(std::clamp(std::lround(iv / period), 1l, 255l));
      w_refreshes_.push_back(uint8_t(r));
      if (w_prev_refreshes_ && r != w_prev_refreshes_) ++w_changes_;
      w_prev_refreshes_ = r;
      // FCP_PACE_TRACE: where a frame that stayed too long lost its time, relative to the display before it
      if (opt_.trace && active_ && r > cadence_ && traced_ < 400) {
        ++traced_;
        const auto rel = [&](Clock::time_point x) { return x.time_since_epoch().count() ? Ms(x - last_display_) : -1.0; };
        const SwapTimes& s = swap_times_[p.swap % swap_times_.size()];
        LogLine("pace late: swap %llu %u refreshes | swap +%.1f ms (prev swap +%.1f), wait done +%.1f, present start "
                "+%.1f, release +%.1f, presented +%.1f, displayed +%.1f",
                (unsigned long long)p.swap, r, s.swap == p.swap ? rel(s.entered) : -1.0,
                s.prev_entered.time_since_epoch().count() ? rel(s.prev_entered) : -1.0,
                s.swap == p.swap ? rel(s.left) : -1.0, rel(p.before), rel(p.release), rel(p.presented), Ms(t - last_display_));
      }
    }
    if (recent_.size() % 30 == 0) {
      EstimateRefresh();
      UpdateCadence();
    }
  }
  last_display_ = t;
  have_display_ = true;
}

void PresentPacer::EstimateRefresh() {
  if (recent_.size() < 40) return;
  const double cur = refresh_ms_.load(std::memory_order_relaxed);
  if (cur > 0) {
    // Keep it while the intervals are whole multiples of it and none is clearly shorter than one refresh.
    const size_t shorter = size_t(std::count_if(recent_.begin(), recent_.end(), [&](float v) { return v >= 2.0f && v < 0.8 * cur; }));
    const double fit = Fit(recent_, cur);
    if (fit >= 0.85 && shorter <= 2) return;
    // The refresh changed (Steam's slider): measure anew from intervals of the new rate only, meanwhile no pacing.
    LogLine("renderer: refresh %.3f ms does not fit the display any more (fit %.2f, %zu shorter): measuring again", cur, fit, shorter);
    recent_.clear();
    refresh_ms_ = 0.0;
    return;
  }
  // The system's value first when it fits, else the largest period (4-42 ms) every interval is a multiple of.
  const auto estimate = [&](const std::vector<float>& iv) {
    double best = 0;
    if (prior_ms_ > 0 && Fit(iv, prior_ms_) >= 0.9) return prior_ms_;
    for (size_t i = 0; i < iv.size(); ++i) {
      for (int k = 1; k <= 4; ++k) {
        const double t = iv[i] / k;
        if (t < 4.0 || t > 42.0 || t <= best + 0.3) continue;
        if (Fit(iv, t) >= 0.9) best = t;
      }
    }
    return best;
  };
  // Both halves of the window must give the same period: a window across a refresh change (90 -> 60 Hz) would
  // otherwise give a common divisor of both (5.6 ms, measured). Then the older half goes and the next try waits.
  const size_t half = recent_.size() / 2;
  const double a = estimate(std::vector<float>(recent_.begin(), recent_.begin() + half));
  const double b = estimate(std::vector<float>(recent_.begin() + half, recent_.end()));
  if (a <= 0 || b <= 0 || std::fabs(a - b) > 0.3) {
    recent_.erase(recent_.begin(), recent_.begin() + half);
    refresh_ms_ = 0.0;
    return;
  }
  const double best = estimate(recent_);
  const double refined = best > 0 ? Refine(recent_, best) : 0.0;
  if (refined > 0 && std::fabs(refined - cur) > 0.02)
    LogLine("renderer: display refresh %.3f ms = %.2f Hz (measured from %zu presents%s)", refined, 1000.0 / refined,
            recent_.size(), best == prior_ms_ ? ", as the system reports" : "");
  refresh_ms_ = refined;
}

void PresentPacer::UpdateCadence() {
  const double period = refresh_ms_.load(std::memory_order_relaxed);
  uint32_t n = 0;
  if (period > 0 && opt_.target_fps != 0 && lost_in_row_ < 3) {
    if (opt_.target_fps < 0) {
      n = 1;
    } else {
      const double ratio = 1000.0 / opt_.target_fps / period;
      const double r = std::round(ratio);
      if (r >= 1 && std::fabs(ratio - r) <= 0.06 * r) n = uint32_t(r);
    }
  }
  // One refresh per frame (60 on 60 Hz): FIFO alone keeps every frame one refresh, and the swap throttle only costs
  // frames (measured in 21_dunea at 60 Hz: 54.5 fps with 90 % single refreshes, the game's own pacing 58.2 with 97 %).
  // FCP_PACE_SINGLE=1 paces it anyway.
  const bool single = n == 1;
  if (single && !opt_.single) n = 0;
  if (n != cadence_) {
    if (n)
      LogLine("renderer: display pacing on: %.2f Hz display, %.0f fps = %u refresh%s per frame", 1000.0 / period,
              opt_.target_fps > 0 ? opt_.target_fps : 1000.0 / period, n, n == 1 ? "" : "es");
    else if (cadence_)
      LogLine("renderer: display pacing off (refresh %.3f ms)", period);
  }
  if (single && !n && !reported_uneven_) {
    reported_uneven_ = true;
    LogLine("renderer: display pacing not needed: %.0f fps on the %.1f Hz display is one refresh per frame (FIFO "
            "paces it; FCP_PACE_SINGLE=1 paces anyway)", opt_.target_fps > 0 ? opt_.target_fps : 1000.0 / period,
            1000.0 / period);
  }
  if (!single && !n && period > 0 && opt_.target_fps > 0 && !reported_uneven_) {
    reported_uneven_ = true;
    const double hz = 1000.0 / period;
    LogLine("renderer: display pacing: %.0f fps does not divide the %.1f Hz display (every frame cannot stay equally "
            "long); even rates are %.1f, %.1f, %.1f fps. The game paces itself", opt_.target_fps, hz, hz, hz / 2, hz / 3);
  }
  cadence_ = n;
  frame_ms_ = n ? double(n) * period : 0.0;
  active_ = n > 0;
}

void PresentPacer::BeforePresent() {
  if (!active_.load(std::memory_order_relaxed)) return;
  std::unique_lock lk(mutex_);
  const uint32_t n = cadence_;
  const double period = refresh_ms_.load(std::memory_order_relaxed);
  const auto t0 = Clock::now();
  before_ = release_ = t0;
  if (n < 2 || period <= 0) return;  // n = 1: FIFO shows the next image at the next refresh anyway
  // The hold counts from the moment the previous present reached the display.
  const bool ok = cv_.wait_until(lk, t0 + FromMs((n + 1) * period + 5.0),
                                 [&] { return stop_ || !active_ || displayed_ >= presented_; });
  if (!ok) {
    ++w_timeouts_;
    return;
  }
  if (stop_ || !active_ || !have_display_) return;
  const Clock::time_point release = last_display_ + FromMs((double(n - 1) + opt_.hold) * period);
  release_ = release;
  lk.unlock();
  SleepUntil(release);
  lk.lock();
  w_hold_us_ += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count());
  ++w_holds_;
}

void PresentPacer::AfterPresent(uint64_t swap) {
  if (!host_) return;
  const uint64_t id = host_->last_present_id();
  const auto now = Clock::now();
  std::lock_guard lock(mutex_);
  if (presented_) w_present_.push_back(float(Ms(now - last_present_)));
  last_present_ = now;
  ++presented_;
  if (thread_.joinable() && id) {
    pending_.push_back({id, swap, now, before_, release_});
    before_ = release_ = {};
    cv_.notify_all();
  } else {
    ++displayed_;  // no display timing: nothing to wait for
    displayed_swap_ = swap;
    displayed_swap_atomic_.store(swap, std::memory_order_release);
    if (opt_.stats_seconds > 0 && now - window_start_ >= FromMs(opt_.stats_seconds * 1000)) LogWindow(now);
  }
}

void PresentPacer::AfterSwap(uint64_t swap) {
  if (!active_.load(std::memory_order_relaxed)) return;
  std::unique_lock lk(mutex_);
  const auto t0 = Clock::now();
  SwapTimes& st = swap_times_[swap % swap_times_.size()];
  st.prev_entered = swap_times_[(swap + swap_times_.size() - 1) % swap_times_.size()].entered;
  st.swap = swap, st.entered = st.left = t0;
  const uint64_t need = swap > opt_.queued ? swap - opt_.queued : 0;
  if (displayed_swap_ >= need) return;
  const double period = refresh_ms_.load(std::memory_order_relaxed);
  const auto limit = t0 + FromMs((cadence_ + 1) * period + 10.0);
  // When the swap the guest waits for will be on the display (the next refresh cycle after the last one seen).
  const uint64_t behind = need - displayed_swap_;
  Clock::time_point due = limit;
  if (have_display_ && behind <= 4) due = std::min(limit, last_display_ + FromMs(double(cadence_ * behind) * period));
  // early: go on that much before it (more time for the next frame); spin: poll the last stretch instead of sleeping,
  // so the CPU does not drop its clock in the wait (measured on the Deck: 2.7 instead of 3.5 GHz with plain waits).
  const Clock::time_point go = due - FromMs(opt_.early_ms);
  const Clock::time_point sleep_until = std::min(limit, go - FromMs(opt_.spin_ms));
  auto done = [&] { return stop_ || !active_ || displayed_swap_ >= need; };
  bool ok = cv_.wait_until(lk, sleep_until, done);
  if (!ok && opt_.early_ms + opt_.spin_ms > 0 && have_display_) {
    lk.unlock();
    while (Clock::now() < go && displayed_swap_atomic_.load(std::memory_order_acquire) < need) std::this_thread::yield();
    lk.lock();
    ok = done() || Clock::now() >= go;
    if (!ok) ok = cv_.wait_until(lk, limit, done);
  } else if (!ok) {
    ok = cv_.wait_until(lk, limit, done);
  }
  st.left = Clock::now();
  w_swap_wait_us_ += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(st.left - t0).count());
  ++w_swap_waits_;
  if (!ok) ++w_timeouts_;
}

void PresentPacer::LogWindow(Clock::time_point now) {
  const double secs = Ms(now - window_start_) / 1000.0;
  char buf[640];
  int at = 0;
  auto add = [&](const char* fmt, auto... args) {
    if (at < int(sizeof(buf))) at += std::snprintf(buf + at, sizeof(buf) - size_t(at), fmt, args...);
  };
  const double period = refresh_ms_.load(std::memory_order_relaxed);
  if (!w_display_.empty()) {
    size_t r[5] = {};
    for (uint8_t v : w_refreshes_) ++r[std::min<size_t>(v, 4) - 1];
    add("presentstat: %zu on the display in %.2f s = %.2f/s | refresh %.2f ms | pacing %s%u | display ms p50 %.2f p99 %.2f "
        "max %.1f | refreshes per frame 1:%zu 2:%zu 3:%zu 4+:%zu | cadence changes %llu | latency ms mean %.2f p99 %.2f",
        w_display_.size(), secs, double(w_display_.size()) / secs, period, active_ ? "on, n " : "off ", cadence_,
        Percentile(w_display_, 0.5), Percentile(w_display_, 0.99), Percentile(w_display_, 1.0), r[0], r[1], r[2], r[3] + r[4],
        (unsigned long long)w_changes_,
        w_latency_.empty() ? 0.0 : std::accumulate(w_latency_.begin(), w_latency_.end(), 0.0) / double(w_latency_.size()),
        Percentile(w_latency_, 0.99));
  } else {
    add("presentstat: no display timing | %zu presents in %.2f s", w_present_.size(), secs);
  }
  add(" | present ms p50 %.2f p99 %.2f max %.1f", Percentile(w_present_, 0.5), Percentile(w_present_, 0.99),
      Percentile(w_present_, 1.0));
  if (w_holds_ || w_swap_waits_ || w_timeouts_ || w_lost_)
    add(" | hold ms %.2f x%llu, swap wait ms %.2f x%llu, timeouts %llu, lost %llu",
        w_holds_ ? double(w_hold_us_) / 1000.0 / double(w_holds_) : 0.0, (unsigned long long)w_holds_,
        w_swap_waits_ ? double(w_swap_wait_us_) / 1000.0 / double(w_swap_waits_) : 0.0, (unsigned long long)w_swap_waits_,
        (unsigned long long)w_timeouts_, (unsigned long long)w_lost_);
  LogLine("%s", buf);
  window_start_ = now;
  w_display_.clear(), w_present_.clear(), w_latency_.clear(), w_refreshes_.clear();
  w_hold_us_ = w_holds_ = w_swap_wait_us_ = w_swap_waits_ = w_timeouts_ = w_lost_ = 0;
  w_changes_ = 0;
}

}  // namespace fcp::nr
