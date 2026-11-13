#include "fcp/frame_coupling.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <random>
#include <string>
#include <unordered_map>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/framerate.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/guest_call.h"
#include "fcp/log.h"
#include "fcp/state_trace.h"
#include "fcp_addresses.h"

REX_EXTERN(__imp__Player_WaterBob);
REX_EXTERN(__imp__Game_ScriptGlobals);
REX_EXTERN(__imp__Entity_CallScriptEvent);
REX_EXTERN(__imp__Lua_PushNumber);
REX_EXTERN(__imp__Lua_SetGlobal);

namespace fcp::frame_coupling {
namespace {

// Player_Land (0x82263A68, landing event of a player or AI; r3 = player object, entity at +4): sends the script
// event 0x17 with fEntitySpeed = -[player +0xB24] (the vertical speed the player update stores each frame),
// fFallSpeedMod and fFallHitMod (Lua jump_fall_speed_mod / jump_fall_hit_mod). Same role as FCI's Game_PlayerLand
// (Far Cry Instincts Recomp, src/xdk/game.cpp, +0xB34 there).
constexpr uint32_t kPlayerFallSpeed = 0xB24;
// Frame timer +0x30: the frame time of this frame (double, also while paused).
constexpr uint32_t kTimerFrameTime = 0x30;

// Fall speed at the console's sampling (FCP_FRAME_FALL=1: the game's own value). The player update
// Player_UpdateMovement (0x8226F0B8, each frame) stores the living entity's vertical speed at +0xB24; at the
// landing event that value is several frames old: measured with FCP_TRACE_LAND on the same jump (m01, 1.2 m):
// fEntitySpeed 4.09 at 30, 4.49 at 60, 4.60 at 120 Bildern/s (takeoff speed 4.87 m/s), i.e. the sample lies
// k = 2.4 frames before the impact at every rate. Shorter frames therefore send a larger speed, and the Lua fall
// damage rises steeply with it (FCI: 9 m fatal at 120). For frames shorter than 30 ms the speed is set for the call
// to the console's sample, B24 + k x (1/30 s - frame time) x g, then put back; g is the player's own fall rate from
// the last two B24 samples (free fall), only after a real fall and only towards a smaller speed, never past 0.
// Same approach as FCI's Game_PlayerLand (FCI, src/xdk/game.cpp, k = 1.5 there).
// FCP_FALL_SAMPLE_FRAMES=<k> overrides the measured k.
constexpr double kConsoleStep = 1.0 / 30.0;
constexpr double kShortFrame = 0.030;
bool g_frame_fall = false;
double g_fall_frames = 2.4;
struct FallState {
  float b24 = 0.0f;       // B24 after the last update
  float b24_prev = 0.0f;  // and the one before
  double dt = 0.0;        // frame time between them
};
std::unordered_map<uint32_t, FallState> g_fall;  // player object -> samples (main thread)

// Water bob at any frame rate (FCP_FRAME_BOB=1: the original). Player_WaterBob (0x8226F5E8, virtual, under water
// only: +0x39C) advances two phases by p_waterbob_pitchspeed/rollspeed x frame time (time based), but then adds
// p_waterbob_pitch x depth factor x sin(phase) to the view angles every frame, on top of the angles of the frames
// before: the sway is a sum over frames and grows with the frame rate. Measured (01_beach, under water, pitch over
// 20 s): span 3.3 degrees at 30, 13.8 at 84 Bildern/s. The amplitudes (cvar block [player +0x1B8]: +0xE8 pitch,
// +0xF0 roll; value at [[cvar +0x14]]) are scaled to frame time x 30 for the call, then put back, so the sum is the
// console's. Same approach as FCI's Game_PlayerAngles (FCI, src/xdk/game.cpp).
constexpr uint32_t kPlayerBobCvars = 0x1B8;
constexpr uint32_t kBobAmplitudes[2] = {0xE8, 0xF0};
constexpr uint32_t kCvarValue = 0x14;
// Frame timer +0x68: the frame time FrameTimer_GetFrameTime hands out (0 while paused).
constexpr uint32_t kTimerGameFrameTime = 0x68;
bool g_frame_bob = false;

// Entity timers with the console's _frametime (FCP_FRAME_TIMERS=1: the original). An entity timer (Lua
// self:SetTimer, binding 0x8232C760: entity +0x3FC period, +0x400 start, ms) fires in the entity update
// (0x820D20D0) through Entity_CallScriptEvent(entity, 4, period) = Server_/Client_OnTimer. BasicPlayer's
// Server_OnTimer deals max_health / drown_time x _frametime per call while under water without breath: the frame
// time, not the timer's. A timer fires on the first frame after its period, so it fires every 66.7 ms at 30 and
// about every period at higher rates while each call deals a quarter of the damage at 120. Measured (01_beach,
// drowning): 7.3 health/s at 30, 2.9 at 84 Bildern/s (dead after 11.2 s against 33.7 s). For frames shorter than
// 30 ms _frametime is set during the timer call to 1/30 s x (the timer's interval at this rate: game time since the
// entity's last timer call, or the period rounded up to whole frames) / (its interval at 30: the period rounded up
// to whole 1/30 s), then back to the frame's value. Same approach as FCI's Game_EntityTimer
// (FCI, src/xdk/game.cpp).
constexpr uint32_t kEventTimer = 4;
bool g_frame_timers = false;
bool g_trace_timers = false;
// timer ticks on the console's frames (OnEntityScriptEvent): experiment, on with FCP_FRAME_TIMER_TICK=0
bool g_frame_timer_tick = false;
uint64_t g_timer_ticks_held = 0;
constexpr uint32_t kEntityTimerPeriod = 0x3FC, kEntityTimerStart = 0x400, kEntitySystemClock = 0xF8;
double g_lua_frametime = 0.0;                       // what Game_ScriptGlobals set this frame
std::unordered_map<uint32_t, double> g_timer_last;  // entity -> game time of its last timer call (main thread)

bool g_trace_land = false;
bool g_trace_ai = false;
std::atomic<uint64_t> g_ai_turns{0};
std::atomic<uint64_t> g_ai_updates{0};

// Boats at the console's 50 water-model steps per second (FCP_FRAME_WATER=1: the original). The rigid body step
// (0x825455E8) calls the post-step of the entity's container (vtable +0x3C, physics flag 0x800); for a vehicle that
// is 0x821E63C8, which runs the boat water model Boat_WaterModel (0x821E64E8, r3 = CVehicle, f1 = step time) in the
// boat modes 1 and 2 (CVehicle +0xAC). The model's horizontal drag is an impulse of -(Dampv x forward speed +
// Dampvs x sideways speed) x mass per call (boat params from Lua, setter 0x82118C98: +0xC0 Dampv, +0xC4 Dampvs,
// +0xF8 TiltSpdA), without the step time. Same code as FCI's CBoat model (Far Cry Instincts Recomp,
// src/xdk/game.cpp, Game_BoatStep; offsets there 4 bytes lower). With one physics step per frame above 50 fps the
// boats lose more speed to it. Here the model runs at most 50 times per second of simulated time, over the time since
// its last call (the step that comes closest to 1/50 s, at most 0.1 s); shorter steps only collect their time. When
// the collected time is not 1/50 s the per-call factors are converted for the call: drag 1 - (1 - c)^(t x 50),
// TiltSpdA x t x 50. Only above the console rate (FCP_FPS != 30).
constexpr float kBoatStep = 0.02f;
constexpr uint32_t kBoatMode = 0xAC, kBoatSpeed = 0x10, kDampv = 0xC0, kDampvs = 0xC4, kTiltSpdA = 0xF8;
bool g_frame_water = false;
bool g_trace_boat = false;
struct BoatState {
  float pending = 0.0f;
  // trace window
  double window_start = -1.0;
  uint32_t steps = 0, calls = 0;
  float step_lo = 1e9f, step_hi = 0.0f;
};
std::unordered_map<uint32_t, BoatState> g_boats;  // CVehicle -> state (main thread: physics runs in System_Update)

// Wheeled vehicles (diagnostics only, FCP_TRACE_WHEELS=1): the wheel force step of CWheeledVehicleEntity
// (vtable 0x8207E228 +0xD8 = Vehicle_WheelForces 0x82556AA8, r3 = entity, f1 = step time, r5 = contact mask; called
// by the rigid body step 0x825455E8 like FCI's 0x003CC9A0). Counts calls and step times per second.
bool g_trace_wheels = false;
struct WheelState {
  double window_start = -1.0;
  uint32_t calls = 0;
  float lo = 1e9f, hi = 0.0f;
};
std::unordered_map<uint32_t, WheelState> g_wheels;
bool g_trace_drowning = false;  // FCP_TRACE_DROWNING (OnPlayerIsUnderWater)
// living entities (OnLivingAction/Step/Move): originals FCP_FRAME_SWIM=1, FCP_FRAME_EDGE=1, FCP_FRAME_LANDING=1
bool g_frame_swim = false, g_frame_edge = false, g_frame_landing = false;
bool g_trace_living = false;      // FCP_TRACE_LIVING=<x>,<y>
// AI path pursuit (OnAiPathPursuit): originals FCP_FRAME_PATH=1, FCP_FRAME_PATH_RELEASE=1; FCP_TRACE_PATH=1
bool g_frame_path = false, g_frame_path_release = false, g_trace_path = false;
bool g_frame_turn = false;  // stick turn ramp (OnLookStickRamp): original FCP_FRAME_TURN=1
bool g_frame_dive = false;  // water entry at the console's frame (OnLivingStep/Move): original FCP_FRAME_DIVE=1
bool g_frame_fire = false, g_trace_fire = false;  // fire interval (OnWeaponTryFire): original FCP_FRAME_FIRE=1
double g_trace_events_from = 0.0, g_trace_events_to = 0.0;  // FCP_TRACE_EVENTS=<from>-<to> (level s): script events
// step edges (OnLivingMove, OnCrtTan): the game's angle, and while a Living_Move with the correction runs on this thread the
// tangent the console's move length gives (0: none); swaps counted for the trace
double g_edge_angle = 0.0;
thread_local double g_edge_tan = 0.0;
thread_local uint32_t g_edge_swaps = 0;
float g_trace_living_at[2] = {};
bool g_trace_living_player = false;  // FCP_TRACE_LIVING=player

// AI think rate at the console's cadence (experiment FCP_AI_CONSOLE_TICK=1, default off). AI_SystemUpdate
// (0x822A79B0) advances its phase +0x74 by one per call (mod 4); the AI list of that phase is "due" (AI_UpdateEntity
// r5 = 0), so every AI decides once per 4 frames: 7.5 times per second at 30, 30 at 120. With the switch the phase
// moves on only once per 1/30 s of game time; on the frames in between +0x74 is set to -2 for the call (phase -1:
// no list due) and put back.
constexpr uint32_t kAiPhase = 0x74;
bool g_ai_console_tick = false;
double g_ai_tick_time = 0.0;

float F32(uint8_t* base, uint32_t address) {
  return std::bit_cast<float>(fcp::guest::U32(base, address));
}

void SetF32(uint8_t* base, uint32_t address, float value) {
  const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(value));
  std::memcpy(fcp::guest::Ptr(base, address), &be, 4);
}

// a factor applied once per 1/50 s call (a share of the speed taken per call), for a call over `steps` x 1/50 s
float PerCall(float c, double steps) {
  if (!(c > 0.0f) || c >= 1.0f) {
    return c;
  }
  return float(1.0 - std::pow(1.0 - double(c), steps));
}

double F64(uint8_t* base, uint32_t address) {
  uint64_t v;
  std::memcpy(&v, fcp::guest::Ptr(base, address), 8);
  return std::bit_cast<double>(__builtin_bswap64(v));
}

}  // namespace

void Configure() {
  g_trace_land = fcp::env::Flag("FCP_TRACE_LAND");
  g_trace_ai = fcp::env::Flag("FCP_TRACE_AI");
  g_frame_fall = fcp::env::Flag("FCP_FRAME_FALL");
  if (const std::string k = fcp::env::Get("FCP_FALL_SAMPLE_FRAMES"); !k.empty()) {
    g_fall_frames = std::clamp(std::atof(k.c_str()), 0.0, 6.0);
  }
  g_frame_bob = fcp::env::Flag("FCP_FRAME_BOB");
  g_frame_timers = fcp::env::Flag("FCP_FRAME_TIMERS");
  g_trace_timers = fcp::env::Flag("FCP_TRACE_TIMERS");
  // experiment, off unless FCP_FRAME_TIMER_TICK=0: measured (05_mines) it does not move the AI timeline (audit 3)
  g_frame_timer_tick = fcp::env::Get("FCP_FRAME_TIMER_TICK") != "0";
  FCPLOG("frame coupling: entity timer ticks on the console's frames {} (experiment: FCP_FRAME_TIMER_TICK=0 turns it on)",
         g_frame_timer_tick ? "off" : "on");
  g_frame_water = fcp::env::Flag("FCP_FRAME_WATER");
  g_trace_boat = fcp::env::Flag("FCP_TRACE_BOAT");
  g_trace_wheels = fcp::env::Flag("FCP_TRACE_WHEELS");
  g_trace_drowning = fcp::env::Flag("FCP_TRACE_DROWNING");
  g_frame_swim = fcp::env::Flag("FCP_FRAME_SWIM");
  // step edges: no coupling measured in Predator yet (05_mines stairs: the landing part alone restores the console's
  // flight), so the edge part is off unless FCP_FRAME_EDGE=0 (experiment)
  g_frame_edge = fcp::env::Get("FCP_FRAME_EDGE") != "0";
  g_frame_landing = fcp::env::Flag("FCP_FRAME_LANDING");
  if (const std::string at = fcp::env::Get("FCP_TRACE_LIVING"); !at.empty()) {
    g_trace_living_player = at == "player";
    g_trace_living = g_trace_living_player ||
                     std::sscanf(at.c_str(), "%f,%f", &g_trace_living_at[0], &g_trace_living_at[1]) == 2;
  }
  FCPLOG("frame coupling: living entities: swimming at the console's rate {}, step edges {}, first landing {} "
         "(originals: FCP_FRAME_SWIM=1, FCP_FRAME_EDGE=1, FCP_FRAME_LANDING=1)",
         g_frame_swim ? "off" : "on", g_frame_edge ? "off" : "on", g_frame_landing ? "off" : "on");
  g_frame_path = fcp::env::Flag("FCP_FRAME_PATH");
  g_frame_path_release = fcp::env::Flag("FCP_FRAME_PATH_RELEASE");
  g_trace_path = fcp::env::Flag("FCP_TRACE_PATH");
  FCPLOG("frame coupling: AI path look-ahead at the console's frame {}, corner release {} (originals: "
         "FCP_FRAME_PATH=1, FCP_FRAME_PATH_RELEASE=1)",
         g_frame_path ? "off" : "on", (g_frame_path || g_frame_path_release) ? "off" : "on");
  g_frame_dive = fcp::env::Flag("FCP_FRAME_DIVE");
  FCPLOG("frame coupling: water entry at the console's frame {} (original: FCP_FRAME_DIVE=1)", g_frame_dive ? "off" : "on");
  g_frame_turn = fcp::env::Flag("FCP_FRAME_TURN");
  FCPLOG("frame coupling: stick turn ramp at the console's sampling {} (original: FCP_FRAME_TURN=1)",
         g_frame_turn ? "off" : "on");
  g_frame_fire = fcp::env::Flag("FCP_FRAME_FIRE");
  g_trace_fire = fcp::env::Flag("FCP_TRACE_FIRE");
  if (const std::string ev = fcp::env::Get("FCP_TRACE_EVENTS"); !ev.empty()) {
    double a = 0.0, b = 1e9;
    std::sscanf(ev.c_str(), "%lf-%lf", &a, &b);
    g_trace_events_from = a;
    g_trace_events_to = b > a ? b : 1e9;
    FCPLOG("FCP_TRACE_EVENTS: every entity script event from level {:.1f} s to {:.1f} s", a, g_trace_events_to);
  }
  FCPLOG("frame coupling: weapon fire interval at the console's frames {} (original: FCP_FRAME_FIRE=1)",
         g_frame_fire ? "off" : "on");
  g_ai_console_tick = fcp::env::Flag("FCP_AI_CONSOLE_TICK");
  if (g_ai_console_tick) {
    FCPLOG("FCP_AI_CONSOLE_TICK: AI decisions at the console's 7.5 per second (experiment)");
  }
  const bool above = fcp::framerate::TargetFps() != 30;
  FCPLOG("frame coupling ({}): fall speed at the console's sampling {} (k {:.2f} frames), water bob scaled {}, timer "
         "_frametime {}, boat water model at 50 Hz {} (originals: FCP_FRAME_FALL=1, FCP_FRAME_BOB=1, "
         "FCP_FRAME_TIMERS=1, FCP_FRAME_WATER=1)",
         above ? "above 30 fps" : "30 fps: all corrections idle", g_frame_fall ? "off" : "on", g_fall_frames,
         g_frame_bob ? "off" : "on", g_frame_timers ? "off" : "on", g_frame_water ? "off" : "on");
  if (g_trace_land) {
    FCPLOG("FCP_TRACE_LAND: every landing event is logged");
  }
}

uint64_t AiTurns() {
  return g_ai_turns.load(std::memory_order_relaxed);
}

uint64_t AiUpdates() {
  return g_ai_updates.load(std::memory_order_relaxed);
}

void OnPlayerUpdateMovement(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t player = ctx.r3.u32;
  original(ctx, base);
  if (g_frame_fall || !fcp::guest::Plausible(player)) {
    return;
  }
  FallState& s = g_fall[player];
  s.b24_prev = s.b24;
  s.b24 = F32(base, player + kPlayerFallSpeed);
  s.dt = F64(base, fcp::addr::frame_timer + kTimerFrameTime);
}

void OnPlayerLand(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t player = ctx.r3.u32;
  if (!fcp::guest::Plausible(player)) {
    original(ctx, base);
    return;
  }
  const uint32_t at = player + kPlayerFallSpeed;
  const float keep = F32(base, at);
  const double dt = F64(base, fcp::addr::frame_timer + kTimerFrameTime);
  float sent = keep;
  double g = 0.0;
  // Only above the console rate: at FCP_FPS=30 the frame time jitters around 33 ms and stays untouched.
  if (!g_frame_fall && fcp::framerate::TargetFps() != 30 && dt > 0.0 && dt < kShortFrame) {
    if (const auto it = g_fall.find(player); it != g_fall.end() && it->second.dt > 0.0) {
      g = (it->second.b24_prev - it->second.b24) / it->second.dt;  // free fall: B24 falls by g x dt per frame
    }
    // only a plausible fall rate, only after a real fall (longer than k console frames), towards 0, never past it
    if (g > 4.0 && g < 40.0 && keep < -g_fall_frames * kConsoleStep * g) {
      sent = std::min(0.0f, float(keep + g_fall_frames * (kConsoleStep - dt) * g));
      const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(sent));
      std::memcpy(fcp::guest::Ptr(base, at), &be, 4);
    }
  }
  if (g_trace_land) {
    FCPLOG("land: {:08X}{} at level {:.3f} s (frame time {:.1f} ms): fEntitySpeed {:.3f}, sent {:.3f} (g {:.2f})",
           player, player == fcp::state::PlayerObject() ? " (local player)" : "", fcp::gametime::LevelSeconds(),
           dt * 1000.0, -keep, -sent, g);
  }
  original(ctx, base);
  if (sent != keep) {
    const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(keep));
    std::memcpy(fcp::guest::Ptr(base, at), &be, 4);
  }
}

void OnPlayerWaterBob(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t player = ctx.r3.u32;
  uint32_t value_at[2] = {};
  uint32_t keep[2] = {};
  if (!g_frame_bob && fcp::framerate::TargetFps() != 30 && fcp::guest::Plausible(player)) {
    const uint32_t cvars = fcp::guest::U32(base, player + kPlayerBobCvars);
    const double dt = F64(base, fcp::addr::frame_timer + kTimerGameFrameTime);
    const float scale = float(std::clamp(dt, 0.0, 0.1) * 30.0);
    for (int i = 0; fcp::guest::Plausible(cvars) && i < 2; ++i) {
      const uint32_t cvar = fcp::guest::U32(base, cvars + kBobAmplitudes[i]);
      const uint32_t at = fcp::guest::Plausible(cvar) ? fcp::guest::U32(base, cvar + kCvarValue) : 0;
      if (fcp::guest::Plausible(at)) {
        value_at[i] = at;
        keep[i] = fcp::guest::U32(base, at);
        const uint32_t be = __builtin_bswap32(std::bit_cast<uint32_t>(std::bit_cast<float>(keep[i]) * scale));
        std::memcpy(fcp::guest::Ptr(base, at), &be, 4);
      }
    }
  }
  original(ctx, base);
  for (int i = 0; i < 2; ++i) {
    if (value_at[i]) {
      const uint32_t be = __builtin_bswap32(keep[i]);
      std::memcpy(fcp::guest::Ptr(base, value_at[i]), &be, 4);
    }
  }
}

void OnScriptGlobals(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  g_lua_frametime = ctx.f2.f64;  // _frametime of this frame
  original(ctx, base);
}

void SetLuaFrameTime(PPCContext& ctx, uint8_t* base, double value) {
  const uint32_t system = fcp::guest::U32(base, fcp::addr::script_system_ptr);
  if (!fcp::guest::Plausible(system)) {
    return;
  }
  const uint32_t lua = fcp::guest::U32(base, system);
  if (!fcp::guest::Plausible(lua)) {
    return;
  }
  fcp::guest::Call(ctx, base, __imp__Lua_PushNumber, {.r3 = lua, .f1 = &value});
  fcp::guest::Call(ctx, base, __imp__Lua_SetGlobal, {.r3 = lua, .r4 = fcp::addr::lua_name_frametime});
}

void OnEntityScriptEvent(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t entity = ctx.r3.u32;
  const double dt = g_lua_frametime;
  if (g_trace_events_to > 0.0) {
    const double level = fcp::gametime::LevelSeconds();
    if (level >= g_trace_events_from && level < g_trace_events_to) {
      // guest call chain: back chain at [sp], the caller's return address saved at [back chain - 8]
      std::string chain;
      uint32_t sp = ctx.r1.u32;
      for (int k = 0; k < 8 && fcp::guest::Plausible(sp); ++k) {
        const uint32_t next = fcp::guest::U32(base, sp);
        if (!fcp::guest::Plausible(next) || next <= sp) {
          break;
        }
        chain += std::format(" {:08X}", fcp::guest::U32(base, next - 8));
        sp = next;
      }
      FCPLOG("event: level {:.4f} s entity {:08X} event {} arg {} from {:08X}{}", level, entity, ctx.r4.u32, ctx.r5.u32,
             uint32_t(ctx.lr), chain);
    }
  }
  if (ctx.r4.u32 != kEventTimer || g_frame_timers || fcp::framerate::TargetFps() == 30 || dt <= 0.0 ||
      dt >= kShortFrame) {
    original(ctx, base);
    return;
  }
  const uint32_t period_ms = ctx.r5.u32;
  // Timer ticks on the console's frames (experiment, FCP_FRAME_TIMER_TICK=0 turns it on; only with FCP_FRAME_TIMERS on). The entity update (call at
  // fcp::addr::entity_timer_return - 4) fires a one-shot timer on the first frame with game time x 1000 - start
  // (+0x400) >= period (+0x3FC), sets +0x3FC to -1 and calls the event; scripts re-arm it in OnTimer. On the
  // console the tick therefore comes after the period rounded up to whole 1/30 s (a 50 ms timer every 66.7 ms), at
  // 120 fps after about the period (measured 50-58 ms): every chain of timers runs faster. Above the console rate a
  // tick that the console would not have fired yet (elapsed + half a frame below the period rounded up to 1/30 s)
  // is put back: +0x3FC = period, start unchanged, the event is not called; the next frames check again.
  if (!g_frame_timer_tick && uint32_t(ctx.lr) == fcp::addr::entity_timer_return && period_ms > 0 &&
      period_ms < 100000) {
    const uint32_t system = fcp::guest::U32(base, fcp::addr::entity_system_ptr);
    const uint32_t clock = fcp::guest::Plausible(system) ? fcp::guest::U32(base, system + kEntitySystemClock) : 0;
    if (fcp::guest::Plausible(clock)) {
      const double now_ms = double(float(F64(base, clock + 0x28 /* frame timer: game time */))) * 1000.0;  // as the game reads it
      const double elapsed = now_ms - double(int32_t(fcp::guest::U32(base, entity + kEntityTimerStart)));
      const double console_ms = std::ceil(period_ms / (1000.0 * kConsoleStep) - 1e-6) * 1000.0 * kConsoleStep;
      if (elapsed + 500.0 * dt < console_ms) {
        const uint32_t be = __builtin_bswap32(period_ms);
        std::memcpy(fcp::guest::Ptr(base, entity + kEntityTimerPeriod), &be, 4);
        if (g_trace_timers) {
          FCPLOG("timer: entity {:08X} period {} ms at level {:.3f} s held ({:.1f} ms of {:.1f}, held so far {})", entity,
                 period_ms, fcp::gametime::LevelSeconds(), elapsed, console_ms, ++g_timer_ticks_held);
        }
        return;
      }
    }
  }
  const double now = fcp::gametime::GameSeconds();
  double& last = g_timer_last[entity];
  const double since = now - last;
  const double period = period_ms / 1000.0;
  // interval at this rate: the measured one when it fits the period (a repeating timer), else the period
  // rounded up to whole frames of this rate; at 30: the period rounded up to whole console frames
  const double here = (last > 0.0 && since >= period && since < period + 4 * dt + 0.05)
                          ? since
                          : std::max(1.0, std::ceil(period / dt - 1e-6)) * dt;
  const double console = std::max(1.0, std::ceil(period / kConsoleStep - 1e-6)) * kConsoleStep;
  last = now;
  const double value = kConsoleStep * here / console;
  if (g_trace_timers) {
    FCPLOG("timer: entity {:08X} period {} ms at level {:.3f} s: interval {:.1f} ms (console {:.1f} ms), _frametime "
           "{:.2f} -> {:.2f} ms",
           entity, period_ms, fcp::gametime::LevelSeconds(), here * 1000.0, console * 1000.0, dt * 1000.0,
           value * 1000.0);
  }
  const uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
  SetLuaFrameTime(ctx, base, value);
  ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5;
  original(ctx, base);
  SetLuaFrameTime(ctx, base, dt);
}

void TraceBoat(uint8_t* base, uint32_t veh, BoatState& s, float dt, float call_step, bool called) {
  const double now = fcp::gametime::LevelSeconds();
  if (s.window_start < 0.0) {
    s.window_start = now;
    FCPLOG("boat: {:08X} mode {} Dampv {} Dampvs {} TiltSpdA {}", veh, fcp::guest::U32(base, veh + kBoatMode),
           F32(base, veh + kDampv), F32(base, veh + kDampvs), F32(base, veh + kTiltSpdA));
  }
  s.steps++;
  if (called) {
    s.calls++;
    s.step_lo = std::min(s.step_lo, call_step);
    s.step_hi = std::max(s.step_hi, call_step);
  }
  (void)dt;
  if (now - s.window_start >= 0.5) {
    FCPLOG("boat: {:08X} at level {:.2f} s: {} physics steps, {} model calls ({:.4f}..{:.4f} s) in {:.2f} s, speed "
           "{:.3f}",
           veh, now, s.steps, s.calls, s.calls ? s.step_lo : 0.0f, s.step_hi, now - s.window_start,
           F32(base, veh + kBoatSpeed));
    s.window_start = now;
    s.steps = s.calls = 0;
    s.step_lo = 1e9f;
    s.step_hi = 0.0f;
  }
}

void OnBoatWaterModel(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t veh = ctx.r3.u32;
  const float dt = float(ctx.f1.f64);
  const uint32_t mode = fcp::guest::Plausible(veh) ? fcp::guest::U32(base, veh + kBoatMode) : 0;
  if (g_frame_water || fcp::framerate::TargetFps() == 30 || !(dt > 0.0f) || (mode != 1 && mode != 2)) {
    if (g_trace_boat && mode) {
      TraceBoat(base, veh, g_boats[veh], dt, dt, true);
    }
    original(ctx, base);
    return;
  }
  BoatState& s = g_boats[veh];
  s.pending += dt;
  if (s.pending + 0.5f * dt < kBoatStep) {  // the step that comes closest to 1/50 s is still to come
    if (g_trace_boat) {
      TraceBoat(base, veh, s, dt, 0.0f, false);
    }
    return;
  }
  const float step = std::min(s.pending, 0.1f);
  s.pending = 0.0f;
  const bool scale = step != kBoatStep;
  const float keep[3] = {F32(base, veh + kDampv), F32(base, veh + kDampvs), F32(base, veh + kTiltSpdA)};
  if (scale) {
    const double steps = double(step) / kBoatStep;
    SetF32(base, veh + kDampv, PerCall(keep[0], steps));
    SetF32(base, veh + kDampvs, PerCall(keep[1], steps));
    SetF32(base, veh + kTiltSpdA, float(keep[2] * steps));
  }
  if (g_trace_boat) {
    TraceBoat(base, veh, s, dt, step, true);
  }
  ctx.f1.f64 = step;
  original(ctx, base);
  if (scale) {
    SetF32(base, veh + kDampv, keep[0]);
    SetF32(base, veh + kDampvs, keep[1]);
    SetF32(base, veh + kTiltSpdA, keep[2]);
  }
}

void OnWheelForces(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  if (g_trace_wheels) {
    const uint32_t ent = ctx.r3.u32;
    const float dt = float(ctx.f1.f64);
    WheelState& s = g_wheels[ent];
    const double now = fcp::gametime::LevelSeconds();
    if (s.window_start < 0.0) {
      s.window_start = now;
    }
    s.calls++;
    s.lo = std::min(s.lo, dt);
    s.hi = std::max(s.hi, dt);
    if (now - s.window_start >= 1.0) {
      FCPLOG("wheels: {:08X} at level {:.2f} s: {} calls ({:.4f}..{:.4f} s) in {:.2f} s", ent, now, s.calls, s.lo,
             s.hi, now - s.window_start);
      s.window_start = now;
      s.calls = 0;
      s.lo = 1e9f;
      s.hi = 0.0f;
    }
  }
  original(ctx, base);
}

// FCP_TRACE_DROWNING=1 (diagnostics): every Lua Player:IsUnderWater call (0x82108EB8; script object r3, CPlayer at
// [+0x10], answer = CPlayer +0x39C) that answers "under water": CPlayer, entity (+4), the entity's bound flag
// (entity +0x2D bit 0x80, Lua Entity:IsBound 0x82323D80), level time. FCI's AI drowning softlock (#22) came from
// such an answer for an AI bound to a helicopter in the 13_Black_Forest opening.

void OnPlayerIsUnderWater(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  if (g_trace_drowning) {
    const uint32_t player = fcp::guest::Plausible(ctx.r3.u32) ? fcp::guest::U32(base, ctx.r3.u32 + 0x10) : 0;
    if (fcp::guest::Plausible(player) && *fcp::guest::Ptr(base, player + 0x39C)) {
      const uint32_t entity = fcp::guest::U32(base, player + 4);
      const unsigned flags = fcp::guest::Plausible(entity) ? *fcp::guest::Ptr(base, entity + 0x2D) : 0u;
      static uint32_t n = 0;
      if (++n <= 400) {
        FCPLOG("drowning: player {:08X}{} entity {:08X} bound {} under water at level {:.3f} s", player,
               player == fcp::state::PlayerObject() ? " (local)" : "", entity, (flags & 0x80) ? 1 : 0,
               fcp::gametime::LevelSeconds());
      }
    }
  }
  original(ctx, base);
}

// ---- living entities (player and AI): swimming, step edges, first landing ----
// CLivingEntity (vtable fcp::addr::living_entity_vtable) has the layout of FCI's (Far Cry Instincts Recomp,
// src/xdk/game.cpp, CLivingEntity 0x00625C40), read from Living_Action 0x8254DC60 and Living_Move 0x82551E88:
// +0x118 position, +0x124 velocity, +0x130 requested velocity, +0x13C gravity, +0x164 air control, +0x1A8 flying,
// +0x1B4 swimming, +0x1C4 flight time, +0x1E0 (Action ignored when set), +0x724 velocity change of a
// pe_action_set_velocity (type 0x0A), moved to +0x730 at the next step, +0x73C (read in the move only by the
// landing decision).
//
// Swimming at the console's rate (FCP_FRAME_SWIM=1: the original). In the swimming mode (air control exactly 1)
// pe_action_move (type 1) sets the velocity to the swim direction plus +0x730, the velocity change that scripts
// (water currents: SetPhysSpeed) made in the frame before; the buoyancy (velocity += gravity x step time) lasts
// one step. Both drift speeds are therefore proportional to the frame time, and the move skips its sweep below
// 1 mm (|move|^2 <= 1e-6 at 0x82552410). Measured (13_black_forest, the player after the opening jump): rises
// 0.37 m/s to the surface at 30, 0.18 at 60, and stays under water at 120 until he drowns. Fix as FCI's
// (Game_LivingAction, Game_LivingStep, Game_LivingMove): +0x730 scaled to 1/30 s for the pe_action_move call, the
// buoyancy scaled to 1/30 s for a step after the action reset the velocity, moves below 1 mm that the console
// would have made collected until they reach 1 mm. Only for frames shorter than 30 ms and above the console rate.
//
// Step edges and the first landing (FCP_FRAME_EDGE=0 turns the edge part on, FCP_FRAME_LANDING=1: the original), as FCI's
// Game_LivingMove: the step-down test in the move compares drop / this step's horizontal move with
// tan(0.7156 rad) (the double at fcp::addr::living_edge_angle, read only there); for steps shorter than 30 ms the
// tangent is the console's for the call (OnCrtTan), so that the test sees the console's move at the same speed. The first landing check
// after a take-off comes one console frame later on the console; for the first 1/30 s of a flight +0x73C is set
// for the call (no landing yet).
constexpr uint32_t kLivPos = 0x118, kLivVel = 0x124, kLivReqVel = 0x130, kLivGravity = 0x13C, kLivAirControl = 0x164,
                   kLivInertia = 0x160, kLivFlying = 0x1A8, kLivSwimming = 0x1B4, kLivFlightTime = 0x1C4, kLivNoAction = 0x1E0,
                   kLivVelChange = 0x730, kLivHoldLanding = 0x73C;
constexpr uint32_t kActionMove = 1, kActionSetVelocity = 0x0A;

bool IsLiving(uint8_t* base, uint32_t living) {
  return fcp::guest::Plausible(living) && fcp::guest::U32(base, living) == fcp::addr::living_entity_vtable;
}

bool Swimming(uint8_t* base, uint32_t living) {
  return IsLiving(base, living) && fcp::guest::U32(base, living + kLivSwimming) != 0 &&
         fcp::guest::U32(base, living + kLivFlying) != 0 && F32(base, living + kLivAirControl) == 1.0f;
}

bool Above30() {
  return fcp::framerate::TargetFps() != 30;
}

struct SwimState {
  uint32_t set_frame[2] = {0, 0};  // the last two frames with a pe_action_set_velocity (frame timer +0), newest first
  float set_dt[2] = {0, 0};        // and their frame times
  float pending[3] = {0, 0, 0};    // moves below 1 mm, collected
  uint32_t frame = 0;              // the frame they were collected in
};
std::unordered_map<uint32_t, SwimState> g_swim;  // living entity -> state (main thread: physics in System_Update)
double g_living_step_dt = 0.0;  // the step time of the living step running now (main thread)
// water entry (OnLivingStep, OnLivingMove): per living entity the last step's swimming state and vertical speed
struct DiveState {
  bool swimming = false;
  float vz = 0.0f;
  uint32_t frame = 0;
};
std::unordered_map<uint32_t, DiveState> g_dive;
uint32_t g_dive_living = 0;  // the entity whose move gets g_dive_dz (during its first swimming step)
float g_dive_dz = 0.0f;
uint32_t g_traced_living = 0;   // FCP_TRACE_LIVING entity

uint32_t FrameNumber(uint8_t* base) {
  return fcp::guest::U32(base, fcp::addr::frame_timer);
}

void OnLivingAction(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t living = ctx.r3.u32, act = ctx.r4.u32;
  float keep[3];
  bool scaled = false;
  if (!g_frame_swim && Above30() && IsLiving(base, living) && fcp::guest::Plausible(act)) {
    const uint32_t type = fcp::guest::U32(base, act);
    if (type == kActionSetVelocity) {
      SwimState& s = g_swim[living];
      s.set_frame[1] = s.set_frame[0], s.set_dt[1] = s.set_dt[0];
      s.set_frame[0] = FrameNumber(base);
      s.set_dt[0] = float(F64(base, fcp::addr::frame_timer + kTimerGameFrameTime));
    } else if (type == kActionMove && fcp::guest::U32(base, living + kLivNoAction) == 0 && Swimming(base, living)) {
      // steps first, then scripts (set velocity), then the movement: +0x730 is the change of the frame before
      const auto it = g_swim.find(living);
      const uint32_t before = FrameNumber(base) - 1;
      float last = 0.0f;
      if (it != g_swim.end()) {
        last = it->second.set_frame[0] == before ? it->second.set_dt[0]
               : it->second.set_frame[1] == before ? it->second.set_dt[1] : 0.0f;
      }
      if (last > 0.0f && last < kShortFrame) {
        for (int i = 0; i < 3; ++i) {
          keep[i] = F32(base, living + kLivVelChange + 4 * i);
          SetF32(base, living + kLivVelChange + 4 * i, keep[i] * float(kConsoleStep / last));
        }
        scaled = true;
      }
    }
  }
  original(ctx, base);
  if (scaled) {
    for (int i = 0; i < 3; ++i) SetF32(base, living + kLivVelChange + 4 * i, keep[i]);
  }
}

void OnLivingStep(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t living = ctx.r3.u32;
  const float dt = float(ctx.f1.f64);
  float keep[3];
  bool scaled = false;
  if (!g_frame_swim && Above30() && dt > 0.0f && dt < kShortFrame && Swimming(base, living) &&
      std::memcmp(fcp::guest::Ptr(base, living + kLivVel), fcp::guest::Ptr(base, living + kLivReqVel), 12) == 0) {
    const float scale = float(kConsoleStep / dt);
    for (int i = 0; i < 3; ++i) {
      keep[i] = F32(base, living + kLivGravity + 4 * i);
      SetF32(base, living + kLivGravity + 4 * i, keep[i] * scale);
    }
    scaled = true;
  }
  // FCP_TRACE_LIVING=<x>,<y>: the first living entity seen within 2 m of (x, y), one line per step;
  // FCP_TRACE_LIVING=player: the living entity at the local player's position (needs FCP_TRACE_STATE)
  uint32_t& traced = g_traced_living;
  if (g_trace_living_player && IsLiving(base, living) && traced != living) {
    float p[3];
    if (fcp::state::PlayerPosition(p) && std::fabs(F32(base, living + kLivPos) - p[0]) < 0.5f &&
        std::fabs(F32(base, living + kLivPos + 4) - p[1]) < 0.5f &&
        std::fabs(F32(base, living + kLivPos + 8) - p[2]) < 2.0f) {
      traced = living;
      FCPLOG("living: {:08X} traced (local player)", living);
    }
  } else if (g_trace_living && !g_trace_living_player && !traced && IsLiving(base, living) &&
             std::fabs(F32(base, living + kLivPos) - g_trace_living_at[0]) < 2.0f &&
             std::fabs(F32(base, living + kLivPos + 4) - g_trace_living_at[1]) < 2.0f) {
    traced = living;
    FCPLOG("living: {:08X} traced", living);
  }
  const bool trace = g_trace_living && living == traced;
  const float z0 = trace ? F32(base, living + kLivPos + 8) : 0.0f;
  const float vz0 = trace ? F32(base, living + kLivVel + 8) : 0.0f;
  // water entry at the console's frame (OnLivingMove): the first swimming step after a fall
  const bool living_ok = IsLiving(base, living);
  const bool swim_now = living_ok && Swimming(base, living);
  if (living_ok && !g_frame_dive && Above30() && dt > 0.0f && dt < kShortFrame && swim_now) {
    const auto it = g_dive.find(living);
    if (it != g_dive.end() && !it->second.swimming && it->second.frame + 1 == FrameNumber(base) &&
        it->second.vz < -2.0f) {
      // The swimming mode starts one step after the step that crossed the water line (measured: the in-water flag
      // follows the position at the start of a step): the fall goes on for the crossing step and one more. That
      // second step ended at game time g - dt, the crossing step at g - 2 dt, the line was crossed about half a
      // step before. The console's crossing step ends at the next 1/30 s after that (its frames on whole 1/30 s
      // of game time), its next step 1/30 s later: so much longer the console falls.
      const double end = fcp::gametime::GameSeconds() - dt;
      const double cross = end - 1.5 * dt;
      const double console_end = std::ceil(cross * 30.0 - 1e-6) / 30.0 + kConsoleStep;
      const double extra = std::clamp(console_end - end, 0.0, 2.0 * kConsoleStep);
      g_dive_living = living;
      g_dive_dz = float(it->second.vz * extra);
      if (g_trace_living && living == g_traced_living) {
        FCPLOG("living: {:08X} water entry at level {:.4f} s: fall {:.3f} m/s, console's fall {:.4f} s longer: {:.3f} m",
               living, fcp::gametime::LevelSeconds(), it->second.vz, extra, g_dive_dz);
      }
    }
  }
  const double outer_dt = g_living_step_dt;
  g_living_step_dt = dt;
  original(ctx, base);
  g_living_step_dt = outer_dt;
  g_dive_living = 0;
  if (living_ok && Above30()) {
    DiveState& d = g_dive[living];
    d.swimming = swim_now;
    d.vz = F32(base, living + kLivVel + 8);
    d.frame = FrameNumber(base);
  }
  if (scaled) {
    for (int i = 0; i < 3; ++i) SetF32(base, living + kLivGravity + 4 * i, keep[i]);
  }
  if (trace) {
    FCPLOG("living: {:08X} at level {:.4f} s dt {:.4f}: pos {:.3f} {:.3f} {:.3f} (z0 {:.3f}) v {:.3f} {:.3f} {:.3f} "
           "fly {} swim {} t_fly {:.3f}{} | vz0 {:.3f} req {:.3f} {:.3f} {:.3f} dv {:.3f} {:.3f} {:.3f} g {:.3f} air {:.3f} "
           "inertia {:.3f} edge swaps {}",
           living, fcp::gametime::LevelSeconds(), dt, F32(base, living + kLivPos), F32(base, living + kLivPos + 4),
           F32(base, living + kLivPos + 8), z0, F32(base, living + kLivVel), F32(base, living + kLivVel + 4),
           F32(base, living + kLivVel + 8), fcp::guest::U32(base, living + kLivFlying),
           fcp::guest::U32(base, living + kLivSwimming), F32(base, living + kLivFlightTime), scaled ? " buoyancy x" : "",
           vz0, F32(base, living + kLivReqVel), F32(base, living + kLivReqVel + 4), F32(base, living + kLivReqVel + 8),
           F32(base, living + kLivVelChange), F32(base, living + kLivVelChange + 4),
           F32(base, living + kLivVelChange + 8), F32(base, living + kLivGravity + 8), F32(base, living + kLivAirControl),
           F32(base, living + kLivInertia), g_edge_swaps);
  }
}

void OnLivingMove(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t living = ctx.r3.u32, pdt = ctx.r4.u32, pmove = ctx.r5.u32;
  if (!Above30() || !IsLiving(base, living) || !fcp::guest::Plausible(pdt) || !fcp::guest::Plausible(pmove)) {
    original(ctx, base);
    return;
  }
  const float dt = F32(base, pdt);
  const bool short_step = dt > 0.0f && dt < kShortFrame;
  // step edges: the console's move length in the step-down test
  // (the angle is read from fcp::addr::living_edge_angle, never written: CRT_tan swaps its argument during this call)
  if (g_edge_angle == 0.0 && !g_frame_edge) {
    g_edge_angle = F64(base, fcp::addr::living_edge_angle);
    FCPLOG("frame coupling: step edges: angle {:.4f} rad at {:08X}, tan of it per call at the console's move length",
           g_edge_angle, fcp::addr::living_edge_angle);
  }
  const bool edge = !g_frame_edge && short_step && g_edge_angle > 0.1 && g_edge_angle < 1.4;
  const double outer_tan = g_edge_tan;
  if (edge) {
    const double steps = kConsoleStep / std::max(double(dt), 1.0 / 240.0);
    g_edge_tan = std::tan(g_edge_angle) * steps;
  }  // the first 1/30 s of a flight: no landing yet (not for swimmers)
  const bool hold = !g_frame_landing && short_step && fcp::guest::U32(base, living + kLivFlying) != 0 &&
                    fcp::guest::U32(base, living + kLivSwimming) == 0 &&
                    fcp::guest::U32(base, living + kLivHoldLanding) == 0 &&
                    double(F32(base, living + kLivFlightTime)) + 1.5 * dt < kConsoleStep;
  if (hold) {
    const uint32_t one = __builtin_bswap32(1u);
    std::memcpy(fcp::guest::Ptr(base, living + kLivHoldLanding), &one, 4);
  }
  // water entry: the rest of the fall the console's last frame would have made, in this move (the sweep collides)
  float dive_keep = 0.0f;
  const bool dive = g_dive_living == living && g_dive_dz < 0.0f;
  if (dive) {
    dive_keep = F32(base, pmove + 8);
    SetF32(base, pmove + 8, dive_keep + g_dive_dz);
    g_dive_living = 0;
  }
  // swimming: moves below 1 mm that the console would have made are collected
  const float* keep = nullptr;
  float m[3] = {}, sum[3];
  if (!g_frame_swim && short_step && Swimming(base, living)) {
    SwimState& s = g_swim[living];
    const uint32_t frame = FrameNumber(base);
    if (frame - s.frame > 2) {  // left the water or a pause in between: what was collected is gone
      s.pending[0] = s.pending[1] = s.pending[2] = 0.0f;
    }
    s.frame = frame;
    for (int i = 0; i < 3; ++i) m[i] = F32(base, pmove + 4 * i);
    const float len2 = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
    const float k = float(kConsoleStep / dt);
    const float console2 = len2 * k * k;  // the same speed over 1/30 s
    if (len2 > 1e-6f || console2 > 1e-6f) {
      for (int i = 0; i < 3; ++i) sum[i] = s.pending[i] + m[i];
      if (len2 > 1e-6f || sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2] > 1e-6f) {
        for (int i = 0; i < 3; ++i) SetF32(base, pmove + 4 * i, sum[i]), s.pending[i] = 0.0f;
        keep = m;
      } else {
        for (int i = 0; i < 3; ++i) s.pending[i] = sum[i];
      }
    } else {
      s.pending[0] = s.pending[1] = s.pending[2] = 0.0f;  // slower than the console moves: stays, as there
    }
  }
  original(ctx, base);
  if (keep) {
    for (int i = 0; i < 3; ++i) SetF32(base, pmove + 4 * i, keep[i]);
  }
  if (hold) {
    std::memset(fcp::guest::Ptr(base, living + kLivHoldLanding), 0, 4);
  }
  if (dive) {
    SetF32(base, pmove + 8, dive_keep);
  }
  g_edge_tan = outer_tan;
}
// ---- end living entities ----

// CRT_tan (0x8262BAF8, f1 -> f1, 17 call sites): Living_Move's step-down test calls it with the edge angle it loads from
// fcp::addr::living_edge_angle (0x82552278 lfd f30, then 0x825522A4 tan). During a corrected Living_Move on this
// thread a call with exactly that angle gets the angle whose tangent is the console's (tan x (1/30 s) / step time);
// the cosine gate on the ground normal before it (0x82628528) keeps the game's angle (it does not depend on the step
// length). Nothing is written to the title's read-only constants (audit 2 changed the double itself).
void OnCrtTan(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  if (g_edge_tan > 0.0 && ctx.f1.f64 == g_edge_angle) {
    ctx.f1.f64 = std::atan(g_edge_tan);
    ++g_edge_swaps;
  }
  original(ctx, base);
}
// ---- AI path following with the console's look-ahead (FCP_FRAME_PATH=1: the original) ----
// The living entity's movement step 0x825530B8 (from Living_Step) builds the move = velocity x step time and, for an
// entity whose owner has a path object (vcall +0x24 -> +0x2F8 -> +0x114, flags +0x74 bit 0x80 and +0x54 bit 0x80),
// calls AI_PathPursuit (0x822F2470; r3 = path, r4 = &position (+0x118), r5 = &move) before Living_Move. It is a
// pursuit with the move's horizontal length L as look-ahead (read from the image, the same code as FCI's 0x001E50B0,
// FCI, src/xdk/game.cpp, Game_PathFollow): it skips the path points (16 bytes at [+0x14, +0x18))
// within L, takes the crossing of the circle of radius L with the next segment (0x822B1288), and turns the move toward
// it with length L; the index +0x20 (halfword) moves on with it. On the console L is one 1/30 s frame's move, at 120
// fps a quarter of it: the agent keeps closer to the path and its corners (FCI: ShaftMan1 in 05_Mines stays in the
// corner of a landing). Port of FCI's fix: for steps shorter than 30 ms the look-ahead is the console's (the length
// of the entity's move over the last complete 1/30 s cell of game time), the move is scaled to it for the call and
// the returned move scaled back to this step's length. The console's release from such a corner (FCI, measured at 30
// fps: the run cycle's root motion aliases with the 30 Hz frames, every 8.3 s a 0.6 s window with every third frame
// 7.35 / 6.06 times longer) is given to an agent that stands still on its path (same index, within 5 mm for two
// console frames). FCP_FRAME_PATH_RELEASE=1: no release. FCP_TRACE_PATH=1: each call for the FCP_TRACE_LIVING entity
// and each release of any other agent.
constexpr uint32_t kPathPoints = 0x14, kPathIndex = 0x20;
constexpr double kAliasLongFrame = 7.35 / 6.06, kAliasPeriod = 8.3, kAliasWindow = 0.6;
struct ConsoleFrameMoves {
  int64_t cell = INT64_MIN;
  double x = 0, y = 0, last = 0;  // the move so far in this cell, the length of the last complete one (0: none)
  double last_call = -1;
  uint32_t index = 0xFFFFFFFFu;  // release from a corner: where the entity stands still, since when
  float px = 0, py = 0;
  double still_since = -1, window_end = -1;
  double phase = -1;  // the run cycle's aliasing phase of this entity
};
std::unordered_map<uint32_t, ConsoleFrameMoves> g_path;  // living entity -> state (main thread)

uint32_t U16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, fcp::guest::Ptr(base, address), 2);
  return __builtin_bswap16(v);
}

void OnAiPathPursuit(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t path = ctx.r3.u32, ppos = ctx.r4.u32, pmove = ctx.r5.u32;
  const uint32_t living = ppos - kLivPos;
  const double dt = g_living_step_dt;
  if (!fcp::guest::Plausible(path) || !fcp::guest::Plausible(pmove) || !IsLiving(base, living)) {
    original(ctx, base);
    return;
  }
  const float in[2] = {F32(base, pmove), F32(base, pmove + 4)};
  const double len = std::sqrt(double(in[0]) * in[0] + double(in[1]) * in[1]);
  ConsoleFrameMoves& m = g_path[living];
  const double now = fcp::gametime::GameSeconds();
  const int64_t cell = int64_t(std::floor(now * 30.0 + 1e-6));
  if (now - m.last_call > 0.1 || now < m.last_call) {  // a new walk: no old steps
    const double phase = m.phase;
    m = ConsoleFrameMoves{};
    m.phase = phase;
  }
  m.last_call = now;
  if (cell != m.cell) {
    m.last = m.cell == cell - 1 ? std::sqrt(m.x * m.x + m.y * m.y) : 0.0;
    m.cell = cell, m.x = m.y = 0;
  }
  m.x += in[0], m.y += in[1];
  double look = len;
  const uint32_t index0 = U16(base, path + kPathIndex);
  bool released = false;
  if (!g_frame_path && Above30() && dt > 0.0 && dt < kShortFrame) {
    look = m.last;
    if (look <= 0) {
      look = len * kConsoleStep / std::max(dt, 1.0 / 240.0);  // no complete console frame yet
    }
    if (!g_frame_path_release && len > 1e-4) {
      const float px = F32(base, ppos), py = F32(base, ppos + 4);
      if (index0 != m.index || std::fabs(px - m.px) > 0.005f || std::fabs(py - m.py) > 0.005f || m.still_since < 0) {
        m.index = index0, m.px = px, m.py = py, m.still_since = now;  // moving on: a new place to watch
      }
      if (m.phase < 0) {  // seeded with the entity, as FCI
        std::minstd_rand rng(living * 2654435761u + 1);
        m.phase = std::uniform_real_distribution<double>(0, kAliasPeriod)(rng);
      }
      const double in_cycle = std::fmod(now - m.phase + 100 * kAliasPeriod, kAliasPeriod);
      const bool in_window = in_cycle < kAliasWindow;
      if (in_window && cell % 3 == 0 && (now - m.still_since >= 2 * kConsoleStep || now < m.window_end)) {
        look *= kAliasLongFrame, released = true;
        if (now >= m.window_end) {
          m.window_end = now + (kAliasWindow - in_cycle);
        }
      }
    }
  }
  const bool scale = look != len && len > 1e-6;
  if (scale) {
    SetF32(base, pmove, float(in[0] * look / len));
    SetF32(base, pmove + 4, float(in[1] * look / len));
  }
  original(ctx, base);
  const float out[2] = {F32(base, pmove), F32(base, pmove + 4)};
  if (scale) {
    SetF32(base, pmove, float(out[0] * len / look));
    SetF32(base, pmove + 4, float(out[1] * len / look));
  }
  if (g_trace_path && living == g_traced_living) {
    FCPLOG("path: {:08X} at level {:.4f} s: pos {:.3f} {:.3f} index {} -> {} of {}, move {:.4f} {:.4f} -> {:.4f} {:.4f} "
           "(look-ahead {:.3f} m, step {:.3f} m){}",
           living, fcp::gametime::LevelSeconds(), F32(base, ppos), F32(base, ppos + 4), index0,
           U16(base, path + kPathIndex),
           (fcp::guest::U32(base, path + kPathPoints + 4) - fcp::guest::U32(base, path + kPathPoints)) / 16, in[0],
           in[1], F32(base, pmove), F32(base, pmove + 4), look, len, released ? ", released" : "");
  } else if (g_trace_path && released) {
    FCPLOG("path: release of {:08X} at level {:.3f} s: pos {:.3f} {:.3f} {:.3f} index {}", living,
           fcp::gametime::LevelSeconds(), F32(base, ppos), F32(base, ppos + 4), F32(base, ppos + 8), index0);
  }
}
// ---- end AI path following ----

// ---- stick turn ramp at the console's sampling (FCP_FRAME_TURN=1: the original) ----
// Look_StickRamp (0x8228AB20, only caller Player_LookInput; r3 = aim controller): while the stick is held out, the
// turn rate +0xAC (yaw) / +0xB0 (pitch) is a quadratic in the time since the ramp started, t = game time (frame
// timer +0x28) - start (+0xBC yaw, +0xC0 pitch; set to the game time on the first frame, 0 when let go), clamped,
// times the stick. The look input turns by rate x frame time: a left Riemann sum over the frames, so the turn of the
// ramp's first second grows with the frame rate (audit 1, m01: 241.8 / 249.3 / 254.5 degrees at 30 / 60 / 120).
// For frames shorter than 30 ms the start is moved for the call so that t is cut to whole 1/30 s (the rate of the
// console frame this frame lies in), then put back: the sum is the console's.
constexpr uint32_t kRampStart[2] = {0xBC, 0xC0};
constexpr uint32_t kTimerGameTime = 0x28;

void OnLookStickRamp(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t aim = ctx.r3.u32;
  const double dt = F64(base, fcp::addr::frame_timer + kTimerGameFrameTime);
  float keep[2] = {}, moved[2] = {};
  bool shift[2] = {false, false};
  if (!g_frame_turn && Above30() && dt > 0.0 && dt < kShortFrame && fcp::guest::Plausible(aim)) {
    const double now = float(F64(base, fcp::addr::frame_timer + kTimerGameTime));  // the game reads it as float
    for (int i = 0; i < 2; ++i) {
      keep[i] = F32(base, aim + kRampStart[i]);
      const double t = now - keep[i];
      if (keep[i] != 0.0f && t > 0.0 && t < 10.0) {
        // the frame timer rounds frame times to whole ms and the game time is a float here: a frame within a quarter
        // of its own time before a 1/30 s boundary counts as on it
        const double q = std::floor((t + 0.25 * dt) * 30.0) / 30.0;
        moved[i] = float(keep[i] + (t - q));
        shift[i] = moved[i] != keep[i];
        if (shift[i]) {
          SetF32(base, aim + kRampStart[i], moved[i]);
        }
      }
    }
  }
  original(ctx, base);
  for (int i = 0; i < 2; ++i) {
    if (shift[i] && F32(base, aim + kRampStart[i]) == moved[i]) {  // still the same ramp: its start back
      SetF32(base, aim + kRampStart[i], keep[i]);
    }
  }
}
// ---- end stick turn ramp ----

// ---- fire rate at the console's frames (FCP_FRAME_FIRE=1: the original) ----
// Weapon_TryFire (0x82283B00; r3 weapon class, r6 shooter, r7 weapon state, r8 &fire flags, r9 fire mode index;
// callers: the player's shoot 0x82257950 and 0x8228FF40): for fire mode index 0 each set flag is gated by
// rate = [game +0x404] x fire mode +4 (Lua fire_rate; fire mode = [weapon class +0x240][0]): flag 2 against the last
// shot +0x20, flag 1 against +0x1C. With now = float(frame timer +0x28): blocked when now > last and
// now - last < rate (single precision); a block clears all flags, a pass stores now as the last shot. The test runs
// once per frame, so the interval is the rate rounded up to whole frames; for the assault rifle (0.1 s) that is
// 3 console frames when the float difference of 100 ms reaches 0.1f and 4 otherwise, at 120 fps 12 or 13 frames
// (measured, audit 3: 8.8 shots/s at 30, 9.6 at 120). For frames shorter than 30 ms the test is taken on the
// console's frames, a 1/30 s grid counted from the last shot: while the trigger is held (flag set on the frame
// before) a shot is only possible on the frame nearest to a console frame; on a frame within 2.5 ms of the grid (the
// frame rate a multiple of 30) the game's own test decides with this frame's clock, so the ms rounding and jitter of
// the frame clock act as at 30; off the grid the console's test decides (ConsoleFireInterval: the first console frame
// whose float test passes, frames of whole ms). A new press is decided as on the console's next frame. Only within
// 0.25 s after the interval. The last-shot time is moved for the call so that the original test passes or blocks
// accordingly, then put back when no shot was stored.
constexpr uint32_t kGameFireRateScale = 0x404;
constexpr uint32_t kWeaponFireModes = 0x240;
constexpr uint32_t kFireModeRate = 0x04;
constexpr uint32_t kFireLast[2] = {0x20, 0x1C};  // for the flags 2 and 1, in the order the game tests them
constexpr uint32_t kFireFlag[2] = {2, 1};
// The frame timer's clock (0x823177E8) is float(whole ms) x 0.001f in single precision; +8 holds the clock's start,
// +0x10 the clock minus the start of this frame, so the game time between two frames is the difference of two such
// floats (100 ms over three console frames come out as 0.0999985 or 0.1000023 s at a clock of 16-32 s).
constexpr uint32_t kTimerClockStart = 0x08, kTimerClockSince = 0x10;
struct FireLast {
  double game = -1.0;   // double game time of the last shot this correction saw stored
  float stored = 0.0f;  // the float the game stored with it
  int64_t clock_ms = 0;  // the frame clock of that frame in whole ms
};
std::unordered_map<uint64_t, uint64_t> g_fire_seen;  // same key -> last frame with the flag set
// a frame within this of a console frame (1/30 s grid from the last shot) counts as on it: the frame clock gives
// whole ms and the limiter jitters, so frames at 60/120 fps land within 1-2 ms of the grid
constexpr double kFireOnGrid = 0.0025;
std::unordered_map<uint64_t, FireLast> g_fire_last;  // (weapon state << 1 | flag index) -> last shot (main thread)
uint64_t g_fire_forced_pass = 0, g_fire_forced_hold = 0;

// the game's test: true when it blocks
bool FireBlocked(float now, float last, float rate) {
  return now > last && float(now - last) < rate;
}

double ClockAt(int64_t ms) {
  return double(float(float(ms) * 0.001f));
}

// The console's interval after a shot: the first console frame j at which the game's test passes. Console frames end
// on whole ms of a 1/30 s grid (j x 33.33 ms rounded); the game time over j frames is the clock's float difference.
double ConsoleFireInterval(const FireLast& last, float rate) {
  for (int j = 1; j <= 300; ++j) {
    const int64_t ms = std::llround(j * 100.0 / 3.0);
    const double elapsed = ClockAt(last.clock_ms + ms) - ClockAt(last.clock_ms);
    if (!FireBlocked(float(last.game + elapsed), last.stored, rate)) {
      return elapsed;
    }
  }
  return -1.0;
}

void OnWeaponTryFire(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t weapon = ctx.r3.u32, shooter = ctx.r6.u32, state = ctx.r7.u32, pflags = ctx.r8.u32;
  const int32_t mode = int32_t(ctx.r9.u32);
  const double dt = F64(base, fcp::addr::frame_timer + kTimerGameFrameTime);
  // FCP_TRACE_FIRE also runs the model without a correction (30 fps, FCP_FRAME_FIRE=1) to compare it with the game
  const bool correct = !g_frame_fire && Above30() && dt > 0.0 && dt < kShortFrame;
  if ((!correct && !g_trace_fire) || mode != 0 || !fcp::guest::Plausible(weapon) || !fcp::guest::Plausible(shooter) ||
      !fcp::guest::Plausible(state) || !fcp::guest::Plausible(pflags)) {
    original(ctx, base);
    return;
  }
  const uint32_t flags = fcp::guest::U32(base, pflags);
  const uint32_t game = fcp::guest::U32(base, fcp::addr::game_ptr);
  const uint32_t modes = fcp::guest::U32(base, weapon + kWeaponFireModes);
  const uint32_t mode0 = fcp::guest::Plausible(modes) ? fcp::guest::U32(base, modes) : 0;
  if (flags == 0 || !fcp::guest::Plausible(game) || !fcp::guest::Plausible(mode0)) {
    original(ctx, base);
    return;
  }
  const double now_d = F64(base, fcp::addr::frame_timer + kTimerGameTime);
  const float now = float(now_d);
  const float rate = F32(base, game + kGameFireRateScale) * F32(base, mode0 + kFireModeRate);
  float keep[2] = {}, moved[2] = {};
  bool tested[2] = {false, false}, shift[2] = {false, false};
  for (int i = 0; i < 2; ++i) {
    if (!(flags & kFireFlag[i])) {
      continue;
    }
    tested[i] = true;
    keep[i] = F32(base, state + kFireLast[i]);
    const uint64_t key = (uint64_t(state) << 1) | uint64_t(i);
    uint64_t& seen = g_fire_seen[key];
    const bool held = seen + 1 == fcp::gametime::Frame();  // the flag was set on the frame before: trigger held
    seen = fcp::gametime::Frame();
    const auto it = g_fire_last.find(key);
    // only a run of shots this correction saw: the last shot is the one it recorded
    if (it == g_fire_last.end() || it->second.stored != keep[i] || !(rate > 0.0f)) {
      continue;
    }
    const double console = ConsoleFireInterval(it->second, rate);
    if (console < 0.0) {
      continue;
    }
    const double elapsed = now_d - it->second.game;
    const bool blocked = FireBlocked(now, keep[i], rate);
    if (elapsed > std::max<double>(console, rate) + 0.25) {
      continue;  // a new burst long after the last shot: nothing to line up
    }
    const double j = std::round(elapsed * 30.0);
    const double off = elapsed - j * kConsoleStep;  // from the console frame j after the last shot
    bool allow;
    if (!held) {
      // a new press: the console tests it on its next frame
      allow = std::ceil(elapsed * 30.0 - 1e-3) * kConsoleStep >= console - 1e-4;
    } else if (off < -0.5 * dt || off >= 0.5 * dt || j < 1.0) {
      allow = false;  // between console frames
    } else if (std::abs(off) <= kFireOnGrid) {
      allow = !blocked;  // a frame on the console's grid (frame rate a multiple of 30): the game's own test
    } else {
      allow = j * kConsoleStep >= console - 1e-4;  // nearest frame off the grid: the console's test
    }
    if (!correct) {
      // trace only
    } else if (allow && blocked) {
      moved[i] = now - std::max(1.0f, 2.0f * rate);  // the test passes
      shift[i] = true;
      ++g_fire_forced_pass;
    } else if (!allow && !blocked) {
      moved[i] = std::nextafter(now, -1e30f);  // now > last, now - last below the rate: the test blocks
      shift[i] = true;
      ++g_fire_forced_hold;
    }
    if (shift[i]) {
      SetF32(base, state + kFireLast[i], moved[i]);
    }
    if (g_trace_fire && shooter == fcp::state::PlayerObject() && (shift[i] || !blocked || !correct)) {
      FCPLOG("fire: level {:.3f} s flag {} elapsed {:.4f} ms console {:.4f} ms (clock {} ms) rate {:.4f}: game {}, "
             "console {}{} (passes {} holds {}) console frame {} {:+.2f} ms{}",
             fcp::gametime::LevelSeconds(), kFireFlag[i], elapsed * 1000.0, console * 1000.0, it->second.clock_ms,
             rate, blocked ? "holds" : "passes", allow ? "passes" : "holds", shift[i] ? " -> moved" : "",
             g_fire_forced_pass, g_fire_forced_hold, int(j), off * 1000.0, held ? "" : " (new press)");
    }
  }
  original(ctx, base);
  for (int i = 0; i < 2; ++i) {
    if (!tested[i]) {
      continue;
    }
    const float v = F32(base, state + kFireLast[i]);
    if (v == now) {  // the test passed and stored this frame's time
      const double clock = F64(base, fcp::addr::frame_timer + kTimerClockStart) +
                           F64(base, fcp::addr::frame_timer + kTimerClockSince);
      g_fire_last[(uint64_t(state) << 1) | uint64_t(i)] = {now_d, now, std::llround(clock * 1000.0)};
    } else if (shift[i] && v == moved[i]) {
      SetF32(base, state + kFireLast[i], keep[i]);
    }
  }
}
// ---- end fire rate ----

void OnAiSystemUpdate(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t system = ctx.r3.u32;
  const double dt = F64(base, fcp::addr::frame_timer + kTimerGameFrameTime);
  if (!g_ai_console_tick || fcp::framerate::TargetFps() == 30 || !fcp::guest::Plausible(system) || !(dt > 0.0) ||
      dt >= kShortFrame) {
    g_ai_tick_time = 0.0;
    original(ctx, base);
    return;
  }
  g_ai_tick_time += dt;
  if (g_ai_tick_time + 0.5 * dt >= kConsoleStep) {  // the frame closest to the next console frame: phase moves on
    g_ai_tick_time = std::max(0.0, std::min(g_ai_tick_time - kConsoleStep, kConsoleStep));
    original(ctx, base);
    return;
  }
  const uint32_t keep = fcp::guest::U32(base, system + kAiPhase);
  const uint32_t none = __builtin_bswap32(uint32_t(-2));
  std::memcpy(fcp::guest::Ptr(base, system + kAiPhase), &none, 4);
  original(ctx, base);
  const uint32_t be = __builtin_bswap32(keep);
  std::memcpy(fcp::guest::Ptr(base, system + kAiPhase), &be, 4);
}

void OnAiUpdateEntity(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  if (g_trace_ai) {
    g_ai_updates.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r5.u32 == 0) {
      g_ai_turns.fetch_add(1, std::memory_order_relaxed);
    }
  }
  original(ctx, base);
}

}  // namespace fcp::frame_coupling

REX_EXTERN(__imp__Player_Land);
REX_HOOK_RAW(Player_Land) {
  ::fcp::frame_coupling::OnPlayerLand(ctx, base, __imp__Player_Land);
}

REX_HOOK_RAW(Player_WaterBob) {
  ::fcp::frame_coupling::OnPlayerWaterBob(ctx, base, __imp__Player_WaterBob);
}

REX_HOOK_RAW(Game_ScriptGlobals) {
  ::fcp::frame_coupling::OnScriptGlobals(ctx, base, __imp__Game_ScriptGlobals);
}

REX_HOOK_RAW(Entity_CallScriptEvent) {
  ::fcp::frame_coupling::OnEntityScriptEvent(ctx, base, __imp__Entity_CallScriptEvent);
}

REX_EXTERN(__imp__Player_UpdateMovement);
REX_HOOK_RAW(Player_UpdateMovement) {
  ::fcp::frame_coupling::OnPlayerUpdateMovement(ctx, base, __imp__Player_UpdateMovement);
}

REX_EXTERN(__imp__AI_UpdateEntity);
REX_HOOK_RAW(AI_UpdateEntity) {
  ::fcp::frame_coupling::OnAiUpdateEntity(ctx, base, __imp__AI_UpdateEntity);
}

REX_EXTERN(__imp__Boat_WaterModel);
REX_HOOK_RAW(Boat_WaterModel) {
  ::fcp::frame_coupling::OnBoatWaterModel(ctx, base, __imp__Boat_WaterModel);
}

REX_EXTERN(__imp__Living_Action);
REX_HOOK_RAW(Living_Action) {
  ::fcp::frame_coupling::OnLivingAction(ctx, base, __imp__Living_Action);
}

REX_EXTERN(__imp__Living_Step);
REX_HOOK_RAW(Living_Step) {
  ::fcp::frame_coupling::OnLivingStep(ctx, base, __imp__Living_Step);
}

REX_EXTERN(__imp__Living_Move);
REX_HOOK_RAW(Living_Move) {
  ::fcp::frame_coupling::OnLivingMove(ctx, base, __imp__Living_Move);
}

REX_EXTERN(__imp__AI_PathPursuit);
REX_HOOK_RAW(AI_PathPursuit) {
  ::fcp::frame_coupling::OnAiPathPursuit(ctx, base, __imp__AI_PathPursuit);
}

REX_EXTERN(__imp__Look_StickRamp);
REX_HOOK_RAW(Look_StickRamp) {
  ::fcp::frame_coupling::OnLookStickRamp(ctx, base, __imp__Look_StickRamp);
}

REX_EXTERN(__imp__CRT_tan);
REX_HOOK_RAW(CRT_tan) {
  ::fcp::frame_coupling::OnCrtTan(ctx, base, __imp__CRT_tan);
}

REX_EXTERN(__imp__Weapon_TryFire);
REX_HOOK_RAW(Weapon_TryFire) {
  ::fcp::frame_coupling::OnWeaponTryFire(ctx, base, __imp__Weapon_TryFire);
}

REX_EXTERN(__imp__AI_SystemUpdate);
REX_HOOK_RAW(AI_SystemUpdate) {
  ::fcp::frame_coupling::OnAiSystemUpdate(ctx, base, __imp__AI_SystemUpdate);
}

REX_EXTERN(__imp__Player_IsUnderWater);
REX_HOOK_RAW(Player_IsUnderWater) {
  ::fcp::frame_coupling::OnPlayerIsUnderWater(ctx, base, __imp__Player_IsUnderWater);
}

REX_EXTERN(__imp__Vehicle_WheelForces);
REX_HOOK_RAW(Vehicle_WheelForces) {
  ::fcp::frame_coupling::OnWheelForces(ctx, base, __imp__Vehicle_WheelForces);
}
