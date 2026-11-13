/**
 * @file        kernel/xma/fcp_xma_stats.h
 *
 * FCP (local change 31, see UPSTREAM.md): counters of the XMA decoder and the audio render clients
 * for measurements. Only counted, nothing changes. The audio worker logs them every 10 s and at
 * shutdown (FCP_AUDIO_STATS=0 turns the lines off).
 */

#pragma once

#include <atomic>

#include <rex/types.h>

namespace rex::kernel {

struct FcpXmaStats {
  std::atomic<u64> contexts_created{0};
  std::atomic<u64> contexts_released{0};
  std::atomic<u32> contexts_live{0};
  std::atomic<u32> contexts_live_max{0};
  std::atomic<u64> kicks{0};
  std::atomic<u64> works{0};          // Work() on an allocated, enabled context
  std::atomic<u64> frames_decoded{0};  // FFmpeg returned a frame
  std::atomic<u64> frames_skipped{0};  // FFmpeg returned EAGAIN (no frame for the packet)
  std::atomic<u64> decode_errors{0};   // send/receive error
  std::atomic<u64> stream_errors{0};   // error_status 4, invalid packet index, no bits to copy
  std::atomic<u64> loops{0};
  std::atomic<u64> output_blocks{0};   // 256 byte output blocks written (128 mono samples)
  std::atomic<u64> codec_opens{0};
  std::atomic<u64> rate_index[4] = {};  // context sample rate index 0..3 at codec open
  std::atomic<u64> channels[2] = {};    // mono, stereo at codec open
  std::atomic<u64> clients_registered{0};
  std::atomic<u64> clients_unregistered{0};
  std::atomic<u64> client_callbacks{0};
  std::atomic<u64> callbacks_after_unregister{0};  // change 32 counts what it prevented
  std::atomic<u64> unregister_waits{0};

  void OnContextCreated() {
    contexts_created.fetch_add(1, std::memory_order_relaxed);
    const u32 live = contexts_live.fetch_add(1, std::memory_order_relaxed) + 1;
    u32 max = contexts_live_max.load(std::memory_order_relaxed);
    while (live > max && !contexts_live_max.compare_exchange_weak(max, live)) {
    }
  }
  void OnContextReleased() {
    contexts_released.fetch_add(1, std::memory_order_relaxed);
    contexts_live.fetch_sub(1, std::memory_order_relaxed);
  }
};

inline FcpXmaStats& GetFcpXmaStats() {
  static FcpXmaStats stats;
  return stats;
}

}  // namespace rex::kernel
