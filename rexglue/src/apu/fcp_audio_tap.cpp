// FCP (local change 30, see UPSTREAM.md): measurement tap on the finished mix. See fcp_audio_tap.h.

#include "apu/fcp_audio_tap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging/log_category.h>
#include <rex/platform/process.h>

#include "apu/audio_system.h"

namespace rex::apu::fcp {
namespace {

using Clock = std::chrono::steady_clock;

constexpr u32 kChannels = kAudioFrameChannels;
constexpr u32 kSamples = kAudioChannelSamples;
constexpr u32 kStatsPeriods = kAudioSampleRate * 10 / kSamples;  // 1875 periods = 10 s audio time

std::string Env(std::string_view name) {
  return rex::platform::GetEnvironmentValue(name).value_or("");
}

std::FILE* OpenForWrite(const std::string& path) {
#if defined(_WIN32)
  std::FILE* file = nullptr;
  return fopen_s(&file, path.c_str(), "wb") == 0 ? file : nullptr;
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

// --- WAV writer (own thread; the mix only appends to a buffer) ---------------------------------

struct Recorder {
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<f32> pending;
  std::string gaps_pending;
  bool stop = false;
  std::thread thread;
  std::FILE* wav = nullptr;
  std::FILE* gaps = nullptr;
  u64 data_bytes = 0;
  std::string path;

  void WriteHeader() {
    // WAVE_FORMAT_EXTENSIBLE, IEEE float, 6 channels FL FR FC LFE BL BR (mask 0x3F).
    auto u16le = [](u8* p, u16 v) { p[0] = u8(v); p[1] = u8(v >> 8); };
    auto u32le = [](u8* p, u32 v) { for (int i = 0; i < 4; ++i) p[i] = u8(v >> (8 * i)); };
    u8 h[68] = {};
    std::memcpy(h, "RIFF", 4);
    const u64 riff = 60 + data_bytes;
    u32le(h + 4, riff > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(riff));
    std::memcpy(h + 8, "WAVEfmt ", 8);
    u32le(h + 16, 40);
    u16le(h + 20, 0xFFFE);
    u16le(h + 22, kChannels);
    u32le(h + 24, kAudioSampleRate);
    u32le(h + 28, kAudioSampleRate * kChannels * 4);
    u16le(h + 32, kChannels * 4);
    u16le(h + 34, 32);
    u16le(h + 36, 22);
    u16le(h + 38, 32);
    u32le(h + 40, 0x3F);
    static constexpr u8 kFloatGuid[16] = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                          0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    std::memcpy(h + 44, kFloatGuid, 16);
    std::memcpy(h + 60, "data", 4);
    u32le(h + 64, data_bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : u32(data_bytes));
    std::fseek(wav, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), wav);
    std::fseek(wav, 0, SEEK_END);
  }

  bool Open(const std::string& file) {
    path = file;
    wav = OpenForWrite(file);
    if (!wav) {
      return false;
    }
    WriteHeader();
    gaps = OpenForWrite(file + ".gaps.tsv");
    if (gaps) {
      std::fputs("period\tseconds\tunderrun_slots\topen_streams\n", gaps);
    }
    thread = std::thread([this] { Run(); });
    return true;
  }

  void Run() {
    std::vector<f32> local;
    std::string local_gaps;
    for (;;) {
      bool done;
      {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(250), [&] { return stop; });
        local.swap(pending);
        local_gaps.swap(gaps_pending);
        done = stop;
      }
      if (!local.empty()) {
        std::fwrite(local.data(), sizeof(f32), local.size(), wav);
        data_bytes += local.size() * sizeof(f32);
        local.clear();
        WriteHeader();  // the file stays valid if the process dies
        std::fflush(wav);
      }
      if (gaps && !local_gaps.empty()) {
        std::fwrite(local_gaps.data(), 1, local_gaps.size(), gaps);
        std::fflush(gaps);
        local_gaps.clear();
      }
      if (done) {
        break;
      }
    }
  }

  void Close() {
    if (!thread.joinable()) {
      return;
    }
    {
      std::lock_guard lock(mutex);
      stop = true;
    }
    wake.notify_all();
    thread.join();
    WriteHeader();
    std::fclose(wav);
    wav = nullptr;
    if (gaps) {
      std::fclose(gaps);
      gaps = nullptr;
    }
  }
};

// --- statistics over 10 s of audio time ---------------------------------------------------------

struct Window {
  u64 periods = 0;
  u64 pulled = 0;
  u64 with_streams = 0;
  u64 frames = 0;
  u64 underruns = 0;
  u64 underrun_periods = 0;
  u32 max_open = 0;
  u32 max_queue = 0;
  f64 sum_sq[kChannels] = {};
  f32 peak = 0.0f;
  u64 over_unity = 0;  // samples above 1.0 before the output clamp
  u64 silent_periods = 0;
};

struct State {
  std::once_flag once;
  bool stats = true;
  Recorder recorder;
  bool recording = false;
  std::mutex record_mutex;  // guards recorder.pending against Shutdown
  std::vector<f32> interleaved = std::vector<f32>(kChannels * kSamples);
  Window window;
  u64 period = 0;
  u64 total_underruns = 0;
  u64 total_underrun_periods = 0;
  Clock::time_point start;
};

State& S() {
  static State* state = new State();  // never destroyed: the mix may run during static teardown
  return *state;
}

void Init(State& s) {
  s.start = Clock::now();
  s.stats = Env("FCP_AUDIO_STATS") != "0";
  std::string path = Env("FCP_RECORD_AUDIO");
  if (path.empty()) {
    return;
  }
  const std::string gen = Env("FCP_RELAUNCH_GEN");
  if (!gen.empty() && gen != "0") {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    const std::string suffix = ".gen" + gen;
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
      path.insert(dot, suffix);
    } else {
      path += suffix;
    }
  }
  if (s.recorder.Open(path)) {
    s.recording = true;
    REXAPU_INFO("FCP_RECORD_AUDIO: recording the mix (6 ch float, {} Hz, before gain and mute) to {}",
                kAudioSampleRate, path);
  } else {
    REXAPU_WARN("FCP_RECORD_AUDIO: cannot open {}", path);
  }
}

std::string Db(f64 sum_sq, u64 n) {
  if (n == 0 || sum_sq <= 0.0) {
    return "-inf";
  }
  return std::format("{:.1f}", 10.0 * std::log10(sum_sq / f64(n)));
}

void LogWindow(State& s) {
  Window& w = s.window;
  const f64 audio_s = f64(s.period) * kSamples / kAudioSampleRate;
  const f64 wall_s = std::chrono::duration<f64>(Clock::now() - s.start).count();
  const u64 n = w.periods * kSamples;
  REXAPU_INFO(
      "fcp audio: t_audio={:.1f}s t_wall={:.1f}s periods={} ({}) streams_max={} frames={} "
      "underruns={} (periods {}) queue_max={} silent_periods={} peak={:.3f} over_unity={} "
      "rms_dB L/R/C/LFE/SL/SR={}/{}/{}/{}/{}/{} | total underruns={}",
      audio_s, wall_s, w.periods, w.pulled == w.periods ? "device" : (w.pulled ? "mixed" : "timer"),
      w.max_open, w.frames, w.underruns, w.underrun_periods, w.max_queue, w.silent_periods, w.peak,
      w.over_unity, Db(w.sum_sq[0], n), Db(w.sum_sq[1], n), Db(w.sum_sq[2], n), Db(w.sum_sq[3], n),
      Db(w.sum_sq[4], n), Db(w.sum_sq[5], n), s.total_underruns);
  w = Window{};
}

}  // namespace

void OnPeriod(const f32* planar, const PeriodInfo& info) {
  State& s = S();
  std::call_once(s.once, [&] { Init(s); });
  if (!s.stats && !s.recording) {
    return;
  }
  const u64 period = s.period++;
  const u32 underruns = static_cast<u32>(std::popcount(info.underrun_mask));
  s.total_underruns += underruns;
  s.total_underrun_periods += underruns ? 1 : 0;

  if (s.stats) {
    Window& w = s.window;
    ++w.periods;
    w.pulled += info.device_pull ? 1 : 0;
    w.with_streams += info.open_streams ? 1 : 0;
    w.frames += info.streams_with_frame;
    w.underruns += underruns;
    w.underrun_periods += underruns ? 1 : 0;
    w.max_open = std::max(w.max_open, info.open_streams);
    w.max_queue = std::max(w.max_queue, info.max_queue);
    bool silent = true;
    for (u32 c = 0; c < kChannels; ++c) {
      const f32* in = planar + c * kSamples;
      f64 sum = 0.0;
      for (u32 k = 0; k < kSamples; ++k) {
        const f32 v = in[k];
        const f32 a = std::fabs(v);
        sum += f64(v) * v;
        w.peak = std::max(w.peak, a);
        w.over_unity += a > 1.0f ? 1 : 0;
        silent = silent && v == 0.0f;
      }
      w.sum_sq[c] += sum;
    }
    w.silent_periods += silent ? 1 : 0;
    if (w.periods >= kStatsPeriods) {
      LogWindow(s);
    }
  }

  if (s.recording) {
    for (u32 k = 0; k < kSamples; ++k) {
      for (u32 c = 0; c < kChannels; ++c) {
        s.interleaved[k * kChannels + c] = planar[c * kSamples + k];
      }
    }
    std::lock_guard lock(s.record_mutex);
    if (!s.recording) {
      return;
    }
    {
      std::lock_guard rec(s.recorder.mutex);
      s.recorder.pending.insert(s.recorder.pending.end(), s.interleaved.begin(),
                                s.interleaved.end());
      if (underruns) {
        s.recorder.gaps_pending += std::format("{}\t{:.4f}\t{:#x}\t{}\n", period,
                                               f64(period) * kSamples / kAudioSampleRate,
                                               info.underrun_mask, info.open_streams);
      }
    }
  }
}

void Shutdown() {
  State& s = S();
  if (s.stats && s.window.periods) {
    LogWindow(s);
  }
  std::lock_guard lock(s.record_mutex);
  if (s.recording) {
    s.recording = false;
    s.recorder.Close();
    REXAPU_INFO("FCP_RECORD_AUDIO: closed {} ({} periods, {} underruns in {} periods)",
                s.recorder.path, s.period, s.total_underruns, s.total_underrun_periods);
  }
}

}  // namespace rex::apu::fcp
