#include "fcp/gametime.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <thread>

#include <windows.h>
// windows.h first
#include <timeapi.h>

#include "fcp/devstart.h"
#include "fcp/engine_hooks.h"
#include "fcp/env.h"
#include "fcp/frame_coupling.h"
#include "fcp/framerate.h"
#include "fcp/guest.h"
#include "fcp/imports.h"
#include "fcp/input_script.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp/render_bridge.h"
#include "fcp/state_trace.h"
#include "fcp/thread_profile.h"
#include "fcp/watchdog.h"
#include "fcp_addresses.h"

namespace fcp::gametime {
namespace {

using Clock = std::chrono::steady_clock;

// Frame timer object (fcp::addr::frame_timer, FrameTimer_Update 0x823175F0): +0 frame counter,
// +0x28 accumulated game time (double, only while the game is not paused), +0x68 frame time (double,
// what FrameTimer_GetFrameTime returns while not paused).
constexpr uint32_t kTimerFrames = 0x00;
constexpr uint32_t kTimerGameTime = 0x28;
constexpr uint32_t kTimerFrameTime = 0x68;
Clock::time_point g_t0;
std::atomic<uint64_t> g_frame{0};
std::atomic<double> g_game{0.0};
std::atomic<double> g_dt{0.0};
std::atomic<bool> g_in_level{false};
std::atomic<double> g_level_t0{0.0};
bool g_level_pending = false;  // a StartLevel was dispatched, the next timer update starts the level
bool g_dispatching_start = false;
std::mutex g_level_mutex;
std::string g_level_name;

// FCP_TRACE_HITCHES
double g_hitch_s = 0.0;
Clock::time_point g_last_frame_start;
bool g_have_last_frame = false;
struct Counters {
  uint64_t reads = 0, opens = 0, shaders = 0, allocs = 0, waits = 0;
};
Counters g_frame_counters;
std::string g_frame_commands;  // commands dispatched since the last frame start (hitch reason)
uint64_t g_hitches = 0;
uint64_t g_main_cycles = 0;       // QueryThreadCycleTime of the main thread at the last frame start
std::FILE* g_frames_csv = nullptr;  // FCP_TRACE_FRAMES

// FCP_TRACE_COMMANDS
std::FILE* g_commands_csv = nullptr;
std::mutex g_commands_mutex;

// clock line every 5 s
bool g_stats = true;
Clock::time_point g_stats_t;
uint64_t g_stats_frame = 0;
double g_stats_game = 0.0;
double g_stats_dt_min = 1e9, g_stats_dt_max = 0.0;

double ReadDouble(uint8_t* base, uint32_t address) {
  uint64_t v;
  std::memcpy(&v, fcp::guest::Ptr(base, address), 8);
  return std::bit_cast<double>(__builtin_bswap64(v));
}

void WriteDouble(uint8_t* base, uint32_t address, double d) {
  const uint64_t v = __builtin_bswap64(std::bit_cast<uint64_t>(d));
  std::memcpy(fcp::guest::Ptr(base, address), &v, 8);
}

// ---- even frame time (FCP_FRAME_DT_SMOOTH=0: the game's whole-ms frame time, as before) ----
// The frame timer's clock gives whole ms (0x823177E8, docs/phase0/engine-abgleich.md), so at 60 frames/s the frame
// time the game moves the world by is 15-18 ms (measured, 06_cliffs: sd 0.7 ms, 1.0 ms against the wall interval)
// while the frames reach the display every 16.7 ms: a turn moves by 4 % more or less from frame to frame (GitHub
// issue #2, the camera's micro stutter while turning). Above 30 frames/s the frame time handed out (+0x68) is the
// target frame time plus a tenth of the time still owed (an average over about ten frames); a frame off the target by
// more than a quarter (a hitch, a slower rate) is handed out whole, with all that is owed. The sum over many frames
// stays the clock's (owed is bounded), the game time +0x28 follows if the update itself added the frame time.
// At 30 frames/s (the console's rate) nothing changes.
// The game logic corrections that model the console's whole-ms frame clock (fire rate on the console's frames, the
// entity timers' _frametime in fcp/frame_coupling.cpp) must not see the evened values: they take the unsmoothed ones
// (RawGameSeconds, RawFrameDt, RawClockScope; FCP_FRAME_DT_RAW_CLOCK=0: the evened ones, as in staging-v060).
struct DtSmooth {
  bool on = true;
  bool raw_clock = true;    // FCP_FRAME_DT_RAW_CLOCK: corrections on the unsmoothed clock
  double owed = 0.0;        // game time the clock has and the game has not been given yet (s)
  double last_game = -1.0;  // +0x28 after the previous update
  double offset = 0.0;      // unsmoothed game time minus +0x28 (s; what the evening has held back so far)
  double raw_dt = 0.0;      // this frame's frame time as the clock measured it (s)
  uint64_t frames = 0, whole = 0;
};
DtSmooth g_dts;

void SmoothFrameTime(uint8_t* base, uint32_t t) {
  constexpr uint32_t kGameTime = 0x28, kFrameTime = 0x68;
  const double raw = ReadDouble(base, t + kFrameTime);
  const double game = ReadDouble(base, t + kGameTime);
  const bool added_here = g_dts.last_game >= 0.0 && std::fabs((game - g_dts.last_game) - raw) < 1e-7;
  // a game time set anew (backwards) or an offset no evening can build up: the unsmoothed clock starts from here
  if ((g_dts.last_game >= 0.0 && game < g_dts.last_game) || std::fabs(g_dts.offset) > 0.5) g_dts.offset = 0.0;
  g_dts.raw_dt = raw;
  const int fps = fcp::framerate::TargetFps();
  if (!g_dts.on || fps <= 30 || !(raw > 0.0) || raw > 0.1) {
    g_dts.owed = 0.0;
    g_dts.last_game = game;
    return;
  }
  const double nominal = 1.0 / fps;
  g_dts.owed += raw;
  double used;
  if (std::fabs(raw - nominal) <= 0.25 * nominal && g_dts.owed < 4.0 * nominal) {
    used = std::clamp(nominal + 0.1 * (g_dts.owed - nominal), 0.75 * nominal, 1.25 * nominal);
    ++g_dts.frames;
  } else {
    used = g_dts.owed;  // a hitch or another rate: everything owed at once
    ++g_dts.whole;
  }
  g_dts.owed -= used;
  WriteDouble(base, t + kFrameTime, used);
  double game_now = game;
  if (added_here) {
    WriteDouble(base, t + kGameTime, game_now = game + (used - raw));
    g_dts.offset += raw - used;
  }
  g_dts.last_game = game_now;
}

bool RawClockDiffers() {
  return g_dts.raw_clock && g_dts.on && (g_dts.offset != 0.0 || g_dts.raw_dt != g_dt.load(std::memory_order_relaxed));
}

double Since(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

std::string CsvName(const char* stem) {
  const int gen = fcp::relaunch::Generation();
  return gen > 0 ? std::format("{}.gen{}.csv", stem, gen) : std::format("{}.csv", stem);
}

std::string CsvText(std::string s) {
  for (char& c : s) {
    if (c == '"') {
      c = '\'';
    }
  }
  return "\"" + s + "\"";
}

Counters ReadCounters() {
  Counters c;
  c.reads = fcp::imports::CountOf("NtReadFile");
  c.opens = fcp::imports::CountOf("NtCreateFile") + fcp::imports::CountOf("NtOpenFile");
  c.shaders = fcp::engine::CountOf("Engine_CompileShader");
  c.allocs = fcp::imports::CountOf("MmAllocatePhysicalMemoryEx");
  c.waits = fcp::imports::CountOf("KeWaitForSingleObject");
  return c;
}

void Hitches() {
  const Clock::time_point now = Clock::now();
  if (g_hitch_s <= 0.0 && !g_frames_csv) {
    return;
  }
  ULONG64 cycles = 0;
  QueryThreadCycleTime(GetCurrentThread(), &cycles);
  if (g_have_last_frame) {
    const double frame_s = std::chrono::duration<double>(now - g_last_frame_start).count();
    const bool hitch = g_hitch_s > 0.0 && frame_s > g_hitch_s;
    // Main thread: CPU time in the frame (thread cycles, TSC rate) and time inside imports by category.
    const double cpu_ms = fcp::imports::TicksToMs(cycles - g_main_cycles);
    const fcp::imports::ThreadFrame im = fcp::imports::TakeThreadFrame(hitch);
    if (hitch) {
      ++g_hitches;
      const Counters c = ReadCounters();
      std::string main;
      if (fcp::imports::Timing()) {
        main = std::format(" | main cpu {:.1f} ms, in imports {:.1f} ms (wait {:.1f} file {:.1f} mem {:.1f} audio "
                           "{:.1f} sync {:.1f} video {:.1f} other {:.1f}), off-cpu outside imports >= {:.1f} ms | "
                           "longest {} {:.1f} ms lr {:08X} r3 {:08X} owner {:08X} | top {}",
                           cpu_ms, im.total_ms, im.cat_ms[0], im.cat_ms[1], im.cat_ms[2], im.cat_ms[3], im.cat_ms[4],
                           im.cat_ms[5], im.cat_ms[6],
                           // wall - cpu - import time: a lower bound (the import time holds CPU time of its own)
                           std::max(0.0, frame_s * 1000.0 - cpu_ms - im.total_ms), im.longest, im.longest_ms,
                           im.longest_lr, im.longest_r3, im.longest_owner, im.top);
      }
      FCPLOG("hitch: frame {} took {:.1f} ms (game dt {:.1f} ms, game {:.3f} s, level {:.3f} s) | reads +{} "
             "opens +{} shader compiles +{} phys allocs +{} waits +{}{}{}",
             g_frame.load(), frame_s * 1000.0, g_dt.load() * 1000.0, g_game.load(), LevelSeconds(),
             c.reads - g_frame_counters.reads, c.opens - g_frame_counters.opens,
             c.shaders - g_frame_counters.shaders, c.allocs - g_frame_counters.allocs,
             c.waits - g_frame_counters.waits, main,
             g_frame_commands.empty() ? std::string() : " | commands: " + g_frame_commands);
      fcp::profile::OnHitch(g_last_frame_start, now, g_frame.load());
    }
    if (g_frames_csv) {
      std::fprintf(g_frames_csv, "%llu,%.4f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%s,%.3f,%08X\n",
                   static_cast<unsigned long long>(g_frame.load()), WallSeconds(), LevelSeconds(), frame_s * 1000.0,
                   g_dt.load() * 1000.0, cpu_ms, im.total_ms, im.cat_ms[0], im.cat_ms[1], im.cat_ms[2], im.cat_ms[3],
                   im.cat_ms[4], im.cat_ms[5] + im.cat_ms[6], im.longest, im.longest_ms, im.longest_lr);
    }
  } else {
    fcp::imports::TakeThreadFrame(false);
  }
  if (g_hitch_s > 0.0) {
    g_frame_counters = ReadCounters();
  }
  g_frame_commands.clear();
  g_main_cycles = cycles;
  g_last_frame_start = now;
  g_have_last_frame = true;
}

void Stats() {
  if (!g_stats) {
    return;
  }
  const double dt = g_dt.load();
  if (dt > 0.0) {
    g_stats_dt_min = std::min(g_stats_dt_min, dt);
    g_stats_dt_max = std::max(g_stats_dt_max, dt);
  }
  const double wall = Since(g_stats_t);
  if (wall < 5.0) {
    return;
  }
  const uint64_t frames = g_frame.load() - g_stats_frame;
  const double game = g_game.load() - g_stats_game;
  FCPLOG("clock: wall +{:.2f} s, frames +{} = {:.1f} fps, game time +{:.3f} s (game/wall {:.3f}), engine dt "
         "{:.1f}..{:.1f} ms, level {} {:.2f} s",
         wall, frames, frames / wall, game, game / wall, g_stats_dt_min * 1000.0, g_stats_dt_max * 1000.0,
         InLevel() ? LevelName() : std::string("-"), LevelSeconds());
  g_stats_t = Clock::now();
  g_stats_frame = g_frame.load();
  g_stats_game = g_game.load();
  g_stats_dt_min = 1e9;
  g_stats_dt_max = 0.0;
}

}  // namespace

void Configure() {
  g_t0 = Clock::now();
  g_stats_t = g_t0;
  g_stats = fcp::env::Flag("FCP_GAMETIME_STATS", true);
  fcp::framerate::Configure();  // FCP_FPS, FCP_FPS_LIMIT, FCP_PRESENT_INTERVAL, FCP_FPS_STATS
  if (const std::string hitch = fcp::env::Get("FCP_TRACE_HITCHES"); !hitch.empty()) {
    int64_t ms = fcp::env::Int("FCP_TRACE_HITCHES", 50);
    if (ms <= 1) {
      ms = 50;
    }
    g_hitch_s = double(ms) / 1000.0;
    FCPLOG("FCP_TRACE_HITCHES: frames over {} ms", ms);
  }
  if (fcp::env::Flag("FCP_TRACE_FRAMES") && !g_frames_csv) {  // Configure may run twice: open once
    std::error_code ec;
    std::filesystem::create_directories(fcp::log::Folder(), ec);
    const auto path = fcp::log::Folder() / CsvName("frames");
    g_frames_csv = std::fopen(path.string().c_str(), "w");
    if (g_frames_csv) {
      std::setvbuf(g_frames_csv, nullptr, _IOFBF, 1 << 16);
      std::fprintf(g_frames_csv, "frame,wall_s,level_s,ms,game_dt_ms,cpu_ms,import_ms,wait_ms,file_ms,mem_ms,"
                                 "audio_ms,sync_ms,other_ms,longest,longest_ms,longest_lr\n");
      FCPLOG("FCP_TRACE_FRAMES: {}", path.string());
    }
  }
  if (fcp::env::Flag("FCP_TRACE_COMMANDS")) {
    std::error_code ec;
    std::filesystem::create_directories(fcp::log::Folder(), ec);
    const auto path = fcp::log::Folder() / CsvName("commands");
    g_commands_csv = std::fopen(path.string().c_str(), "w");
    if (g_commands_csv) {
      std::fprintf(g_commands_csv, "wall_s,frame,game_s,level_s,event,lr,command\n");
      FCPLOG("FCP_TRACE_COMMANDS: {}", path.string());
    } else {
      FCPLOG("FCP_TRACE_COMMANDS: cannot write {}", path.string());
    }
  }
  fcp::state::Configure();
  fcp::frame_coupling::Configure();  // FCP_TRACE_LAND, FCP_TRACE_AI, corrections
}

void OnMainLoop(PPCContext& ctx, uint8_t* base) {
  fcp::framerate::OnMainLoop(base, g_game.load(std::memory_order_relaxed));  // limiter, r_vsynch, frame stats
  Hitches();
  g_frame.fetch_add(1, std::memory_order_relaxed);
  fcp::state::OnFrame(ctx, base);
  Stats();
}

void OnFrameTimer(uint8_t* base) {
  const uint32_t t = fcp::addr::frame_timer;
  if (!fcp::guest::Readable(base, t, 0x70)) {
    return;
  }
  static const bool dts_init = [] {
    g_dts.on = fcp::env::Flag("FCP_FRAME_DT_SMOOTH", true);
    g_dts.raw_clock = fcp::env::Flag("FCP_FRAME_DT_RAW_CLOCK", true);
    FCPLOG("frame time: {} above 30 frames/s (FCP_FRAME_DT_SMOOTH=0: the game's whole-ms frame times)",
           g_dts.on ? "even (target frame time plus a tenth of the time owed)" : "whole ms as the game measures it");
    if (g_dts.on)
      FCPLOG("frame time: fire rate and entity timer corrections on the {} clock (FCP_FRAME_DT_RAW_CLOCK=0: the even one)",
             g_dts.raw_clock ? "unsmoothed whole-ms" : "even");
    return true;
  }();
  (void)dts_init;
  SmoothFrameTime(base, t);
  const double game = ReadDouble(base, t + kTimerGameTime);
  g_game.store(game, std::memory_order_relaxed);
  g_dt.store(ReadDouble(base, t + kTimerFrameTime), std::memory_order_relaxed);
  if (g_level_pending) {
    g_level_pending = false;
    g_level_t0.store(game, std::memory_order_relaxed);
    const bool first = !g_in_level.exchange(true);
    FCPLOG("gametime: level '{}' started at frame {}, wall {:.3f} s, game {:.3f} s{}", LevelName(), g_frame.load(),
           WallSeconds(), game, first ? "" : " (again)");
    fcp::input_script::OnLevelStart();
  }
}

void OnPushCommand(uint8_t* base, uint32_t command, uint32_t lr) {
  if (!g_commands_csv) {
    return;
  }
  const std::string text = fcp::guest::CString(base, command, 200);
  std::lock_guard lock(g_commands_mutex);
  std::fprintf(g_commands_csv, "%.3f,%llu,%.3f,%.3f,push,%08X,%s\n", WallSeconds(),
               static_cast<unsigned long long>(g_frame.load()), g_game.load(), LevelSeconds(), lr,
               CsvText(text).c_str());
}

void OnDispatchBegin(uint8_t* base, uint32_t command) {
  const std::string text = fcp::guest::CString(base, command, 200);
  if (text.starts_with("StartLevel")) {
    g_dispatching_start = true;
    std::string level;
    if (const size_t at = text.find("level="); at != std::string::npos) {
      level = text.substr(at + 6);
      level = level.substr(0, level.find(' '));
    }
    {
      std::lock_guard lock(g_level_mutex);
      g_level_name = level;
    }
    fcp::render::OnLevelLoading(level);  // pipeline prewarm: this level's pipelines first
  }
  if (g_hitch_s > 0.0) {
    if (!g_frame_commands.empty()) {
      g_frame_commands += "; ";
    }
    g_frame_commands += text.substr(0, 60);
  }
  if (g_commands_csv) {
    std::lock_guard lock(g_commands_mutex);
    std::fprintf(g_commands_csv, "%.3f,%llu,%.3f,%.3f,dispatch,,%s\n", WallSeconds(),
                 static_cast<unsigned long long>(g_frame.load()), g_game.load(), LevelSeconds(),
                 CsvText(text).c_str());
  }
}

void OnDispatchEnd() {
  if (g_dispatching_start) {
    g_dispatching_start = false;
    g_level_pending = true;
    if (g_commands_csv) {
      std::lock_guard lock(g_commands_mutex);
      std::fprintf(g_commands_csv, "%.3f,%llu,%.3f,%.3f,level_loaded,,%s\n", WallSeconds(),
                   static_cast<unsigned long long>(g_frame.load()), g_game.load(), LevelSeconds(),
                   CsvText(LevelName()).c_str());
    }
  }
}

void Shutdown() {
  fcp::state::Shutdown();
  if (g_frames_csv) {
    std::fclose(g_frames_csv);
    g_frames_csv = nullptr;
  }
  if (g_commands_csv) {
    std::lock_guard lock(g_commands_mutex);
    std::fclose(g_commands_csv);
    g_commands_csv = nullptr;
  }
  if (g_hitch_s > 0.0) {
    FCPLOG("hitches: {} frames over {:.0f} ms in {} frames", g_hitches, g_hitch_s * 1000.0, g_frame.load());
  }
  fcp::framerate::Shutdown();
}

double WallSeconds() {
  return Since(g_t0);
}

uint64_t Frame() {
  return g_frame.load(std::memory_order_relaxed);
}

double GameSeconds() {
  return g_game.load(std::memory_order_relaxed);
}

double FrameDt() {
  return g_dt.load(std::memory_order_relaxed);
}

double RawGameSeconds() {
  return RawClockDiffers() ? GameSeconds() + g_dts.offset : GameSeconds();
}

double RawFrameDt() {
  return RawClockDiffers() ? g_dts.raw_dt : FrameDt();
}

RawClockScope::RawClockScope(uint8_t* base) : base_(base) {
  const uint32_t t = fcp::addr::frame_timer;
  if (!RawClockDiffers() || !fcp::guest::Readable(base, t, 0x70)) return;
  game_ = ReadDouble(base, t + kTimerGameTime);
  dt_ = ReadDouble(base, t + kTimerFrameTime);
  WriteDouble(base, t + kTimerGameTime, game_ + g_dts.offset);
  WriteDouble(base, t + kTimerFrameTime, g_dts.raw_dt);
  active_ = true;
}

RawClockScope::~RawClockScope() {
  if (!active_) return;
  const uint32_t t = fcp::addr::frame_timer;
  WriteDouble(base_, t + kTimerGameTime, game_);
  WriteDouble(base_, t + kTimerFrameTime, dt_);
}

bool InLevel() {
  return g_in_level.load(std::memory_order_relaxed);
}

double LevelSeconds() {
  return InLevel() ? GameSeconds() - g_level_t0.load(std::memory_order_relaxed) : 0.0;
}

std::string LevelName() {
  std::lock_guard lock(g_level_mutex);
  return g_level_name;
}

}  // namespace fcp::gametime
