// Worker threads that create graphics pipelines (GameRenderer, FCP_RENDER_PIPELINE_ASYNC).
//
// Two queues: urgent (a draw of the current command list needs the pipeline) before background (the pipeline list
// prewarmed at startup). A thread that waits for a job helps: it takes that job itself if it is still queued, then
// other urgent jobs, so a wait never leaves the calling thread idle and the pool works with zero threads too.
// Own code; the idea of urgent jobs ahead of the prewarm follows King Kong Recompiled (pipeline_workers.h, MIT).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <nvrhi/nvrhi.h>

namespace fcp::nr {

struct PipelineJob {
  enum State : int { kQueued = 0, kRunning = 1, kDone = 2 };
  uint64_t key = 0;
  std::function<nvrhi::GraphicsPipelineHandle()> make;
  nvrhi::GraphicsPipelineHandle result;  // valid once state == kDone (null: creation failed)
  std::atomic<int> state{kQueued};
  bool urgent = false;
  uint32_t groups = 0;   // prewarm: bit mask of the pipeline list groups (menu, levels) the pipeline was seen in
  double create_ms = 0;  // time in make()
};
using PipelineJobPtr = std::shared_ptr<PipelineJob>;

// Default thread count: logical CPUs the process may run on (affinity) minus two (the game's main and render
// threads), 1..8 (deferred_list.cpp).
unsigned AutoPipelineThreads();
// Thread priority of the calling worker: below normal while it runs a background (prewarm) job, so the prewarm
// does not take CPU time from the game's main and render threads (deferred_list.cpp; no-op outside Windows).
void SetPoolThreadBackground(bool background);

class PipelinePool {
 public:
  PipelinePool() = default;
  ~PipelinePool() { Stop(); }
  PipelinePool(const PipelinePool&) = delete;
  PipelinePool& operator=(const PipelinePool&) = delete;

  void Start(unsigned threads) {
    std::lock_guard lock(m_);
    stop_ = false;
    for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this, i] { Worker(i); });
  }
  // Drops queued jobs (they stay kQueued, nobody waits for them after Stop) and joins the threads.
  void Stop() {
    {
      std::lock_guard lock(m_);
      stop_ = true;
      urgent_.clear();
      background_.clear();
    }
    cv_work_.notify_all();
    cv_done_.notify_all();
    for (std::thread& t : threads_)
      if (t.joinable()) t.join();
    threads_.clear();
  }
  unsigned threads() const { return unsigned(threads_.size()); }
  // FCP_RENDER_PREWARM_LOW_PRIORITY=0: background jobs at normal priority (before Start).
  void SetLowPriorityBackground(bool on) { low_priority_background_ = on; }
  // Moves the queued background jobs whose groups intersect `mask` to the front of the background queue (order
  // among them kept). Returns how many were moved.
  size_t PrioritizeBackground(uint32_t mask) {
    std::lock_guard lock(m_);
    auto mid = std::stable_partition(background_.begin(), background_.end(),
                                     [mask](const PipelineJobPtr& j) { return (j->groups & mask) != 0; });
    return size_t(mid - background_.begin());
  }
  size_t background_queued() {
    std::lock_guard lock(m_);
    return background_.size();
  }

  void Submit(const PipelineJobPtr& j, bool urgent) {
    {
      std::lock_guard lock(m_);
      j->urgent = urgent;
      (urgent ? urgent_ : background_).push_back(j);
    }
    cv_work_.notify_one();
  }
  // Moves a still queued background job to the end of the urgent queue.
  void Promote(const PipelineJobPtr& j) {
    std::lock_guard lock(m_);
    if (j->urgent || j->state.load() != PipelineJob::kQueued) return;
    for (auto it = background_.begin(); it != background_.end(); ++it)
      if (*it == j) {
        background_.erase(it);
        j->urgent = true;
        urgent_.push_back(j);
        break;
      }
  }
  // Until j is done: runs j on this thread if it is still queued, else other urgent jobs, else sleeps.
  void Wait(const PipelineJobPtr& j) { WaitUntil(j, std::chrono::steady_clock::time_point::max()); }
  // Same with a deadline (a job this thread runs itself is finished first). True: j is done.
  bool WaitUntil(const PipelineJobPtr& j, std::chrono::steady_clock::time_point deadline) {
    while (j->state.load(std::memory_order_acquire) != PipelineJob::kDone) {
      PipelineJobPtr run;
      {
        std::unique_lock lock(m_);
        if (j->state.load() == PipelineJob::kDone) break;
        if (j->state.load() == PipelineJob::kQueued) run = Take(j);
        if (!run && !urgent_.empty() && std::chrono::steady_clock::now() < deadline) run = Pop(urgent_);
        if (!run) {
          if (stop_) return false;
          auto ready = [&] { return stop_ || j->state.load() == PipelineJob::kDone || !urgent_.empty(); };
          if (deadline == std::chrono::steady_clock::time_point::max()) cv_done_.wait(lock, ready);
          else if (!cv_done_.wait_until(lock, deadline, ready)) return false;
          if (std::chrono::steady_clock::now() >= deadline && j->state.load() != PipelineJob::kDone) return false;
          continue;
        }
      }
      Run(run);
    }
    return true;
  }
  uint64_t jobs_run() const { return jobs_run_.load(); }
  uint64_t jobs_helped() const { return jobs_helped_.load(); }

 private:
  PipelineJobPtr Pop(std::deque<PipelineJobPtr>& q) {
    PipelineJobPtr j = std::move(q.front());
    q.pop_front();
    j->state = PipelineJob::kRunning;
    return j;
  }
  PipelineJobPtr Take(const PipelineJobPtr& j) {
    for (auto* q : {&urgent_, &background_})
      for (auto it = q->begin(); it != q->end(); ++it)
        if (*it == j) {
          PipelineJobPtr r = std::move(*it);
          q->erase(it);
          r->state = PipelineJob::kRunning;
          return r;
        }
    return nullptr;
  }
  void Run(const PipelineJobPtr& j, bool worker = false) {
    const auto t0 = std::chrono::steady_clock::now();
    nvrhi::GraphicsPipelineHandle p = j->make ? j->make() : nullptr;
    j->create_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    j->make = nullptr;  // release captured shader handles
    {
      std::lock_guard lock(m_);
      j->result = std::move(p);
      j->state.store(PipelineJob::kDone, std::memory_order_release);
    }
    ++jobs_run_;
    if (!worker) ++jobs_helped_;
    cv_done_.notify_all();
  }
  void Worker(unsigned index) {
#ifdef _WIN32
    (void)index;
#endif
    bool lowered = false;
    for (;;) {
      PipelineJobPtr j;
      bool background = false;
      {
        std::unique_lock lock(m_);
        cv_work_.wait(lock, [&] { return stop_ || !urgent_.empty() || !background_.empty(); });
        if (stop_) return;
        background = urgent_.empty();
        j = Pop(!urgent_.empty() ? urgent_ : background_);
      }
      const bool lower = background && low_priority_background_;
      if (lower != lowered) SetPoolThreadBackground(lower), lowered = lower;
      Run(j, true);
    }
  }

  std::mutex m_;
  std::condition_variable cv_work_, cv_done_;
  std::deque<PipelineJobPtr> urgent_, background_;
  std::vector<std::thread> threads_;
  bool stop_ = false;
  bool low_priority_background_ = true;
  std::atomic<uint64_t> jobs_run_{0}, jobs_helped_{0};
};

}  // namespace fcp::nr
