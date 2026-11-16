// PresentPacer: when the presented images reach the display, and frame pacing on the display's own clock.
//
// Measurement (always, when the host has display timing: Vulkan with VK_KHR_present_wait): a waiter thread sees each
// present reach the display (HostDevice::WaitPresented) and logs a "presentstat:" line every stats_seconds: display
// interval distribution in refreshes per frame, cadence changes (judder), present-to-display latency, the measured
// refresh period. This is what the player sees, unlike the game's frame times (framestat, fcp/framerate.h).
//
// Display pacing (target_fps > 0, FCP_FPS_PACING=display): every frame stays on the display for the same whole number
// n of refreshes, n = refresh rate / target (45 on a 90 Hz display: n = 2; 30: 3; 90: 1). Two parts:
//   - Present hold (present thread, before the present, without the renderer lock): the present of a frame goes out
//     only after the previous frame's (n-1)-th refresh began, so FIFO shows it at the n-th refresh, not earlier. The
//     GPU work was submitted before; only the queue present waits.
//   - Swap throttle (guest render thread, after its swap): waits until the previous swap reached the display (queue
//     depth `queued`), so the game runs at the display's cadence, renders every frame it shows once, and its frame
//     time (dt) is even.
// Both waits are bounded ((n + 1) refreshes plus a margin); without display timing, before the refresh period is
// known, or when the target does not divide the refresh rate (60 on 90 Hz) pacing stays off (active() false) and the
// game paces itself as before (fcp/framerate.h).
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fcp::nr {

class HostDevice;

struct PacerOptions {
  double stats_seconds = 10;  // presentstat line every n seconds (0: none)
  double target_fps = 0;      // display pacing: wanted frames per second (0: off; < 0: every refresh)
  uint32_t queued = 1;        // display pacing: swaps that may wait for the display before the guest waits
  // display pacing: present released this fraction of a refresh after the (n-1)-th refresh began. 0 measured best on the
  // Deck (gamescope takes a frame only well before its refresh: 0.3 missed 9-15 %, 0.6 70 % of the refreshes, 0 < 1 %)
  double hold = 0.0;
  double early_ms = 0;        // display pacing: the guest goes on this long before the awaited swap is displayed
  double spin_ms = 0;         // display pacing: the guest polls (instead of sleeping) the last stretch of its wait
  bool trace = false;         // FCP_PACE_TRACE=1: a "pace late:" line for each frame that stayed too long
  bool single = false;        // FCP_PACE_SINGLE=1: pace also at one refresh per frame (off: FIFO alone)
};

class PresentPacer {
 public:
  using Clock = std::chrono::steady_clock;
  ~PresentPacer();
  // Starts the waiter thread (only when the host has display timing). Again after Stop for a new host.
  void Start(HostDevice* host, const PacerOptions& options);
  void Stop();
  // Settings menu (any thread): a new display pacing target (PacerOptions::target_fps), at once; the cadence follows
  // from the measured refresh (pacing on, off, or another number of refreshes per frame).
  void SetTarget(double target_fps);
  // Present thread, right before the present, without the renderer lock: holds the present for the cadence.
  void BeforePresent();
  // Present thread, right after HostDevice::Present: the present carried guest swap `swap` (1, 2, ...).
  void AfterPresent(uint64_t swap);
  // Guest render thread, after swap `swap`, without the renderer lock: waits while the display is behind.
  void AfterSwap(uint64_t swap);
  // Display pacing is on (the refresh is known and divisible by the target).
  bool active() const { return active_.load(std::memory_order_relaxed); }
  // Measured refresh period in ms (0: not yet known).
  double refresh_ms() const { return refresh_ms_.load(std::memory_order_relaxed); }
  // While active: the display's frame period (refreshes per frame x refresh period) in ms, else 0.
  double frame_ms() const { return frame_ms_.load(std::memory_order_relaxed); }

 private:
  struct Pending {
    uint64_t id = 0, swap = 0;
    Clock::time_point presented, before, release;  // present call; BeforePresent entry and release (trace)
  };
  struct SwapTimes {  // AfterSwap of a swap: entry, end of the wait, entry of the swap before (trace)
    uint64_t swap = 0;
    Clock::time_point entered, left, prev_entered;
  };
  void Run();
  void OnDisplayed(const Pending& p, Clock::time_point t);  // under mutex_
  void EstimateRefresh();                                  // under mutex_
  void UpdateCadence();                                    // under mutex_
  void LogWindow(Clock::time_point now);                   // under mutex_

  HostDevice* host_ = nullptr;
  PacerOptions opt_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::deque<Pending> pending_;
  uint64_t presented_ = 0, displayed_ = 0;  // presents handed to the host / seen on the display
  uint64_t displayed_swap_ = 0;             // guest swap of the latest present seen on the display
  std::atomic<uint64_t> displayed_swap_atomic_{0};  // the same, for polling without the mutex
  bool have_display_ = false;
  Clock::time_point last_display_, last_present_;
  uint32_t lost_in_row_ = 0;                // presents never seen on the display, in a row
  std::atomic<bool> active_{false};
  std::atomic<double> refresh_ms_{0.0};
  std::atomic<double> frame_ms_{0.0};
  uint32_t cadence_ = 0;                    // n while active
  double prior_ms_ = 0;                     // refresh period the system reports (EnumDisplaySettings), 0: none
  std::vector<float> recent_;               // display intervals for the refresh estimate (ms)
  bool reported_uneven_ = false;
  std::array<SwapTimes, 8> swap_times_{};
  Clock::time_point before_, release_;      // of the present in progress
  uint32_t traced_ = 0;
  // statistics window
  Clock::time_point window_start_;
  std::vector<float> w_display_, w_present_, w_latency_;
  std::vector<uint8_t> w_refreshes_;
  uint64_t w_hold_us_ = 0, w_holds_ = 0, w_swap_wait_us_ = 0, w_swap_waits_ = 0, w_timeouts_ = 0, w_lost_ = 0;
  uint32_t w_prev_refreshes_ = 0;
  uint64_t w_changes_ = 0;
};

}  // namespace fcp::nr
