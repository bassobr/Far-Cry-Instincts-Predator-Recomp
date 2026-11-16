// PC level of detail (PC comfort): [graphics] lod_distance / FCP_LOD_DISTANCE and view_distance /
// FCP_VIEW_DISTANCE, default 1 = as the console.
//
// The idea is from Far Cry Instincts Recomp (FCI; src/xdk/lod.cpp, MIT, same author): multiply the 3D engine's three
// global detail ratios right after they are registered. Rewritten for the Xbox 360 title; the block, its offsets and
// the console values were read from this image, not taken over.
//
// Game_EngineCVars (config/predator/default.toml): r3 = the 3D engine's console variable block (0x104 bytes,
// allocated by the renderer init 0x825D0270, which keeps it at [engine +0x380] and in a global). It registers every
// e_* variable bound to its field (Register(float) 0x82319000 stores the value). Three floats decide how far objects
// keep their detail and how far they are drawn:
//   +0xF4  e_obj_lod_ratio = 20                 mesh LOD of vegetation, brushes and entities
//   +0xF8  e_obj_view_dist_ratio = 120          how far they are drawn (ratio x object's view distance ratio x radius)
//   +0xFC  e_obj_sprite_switch_dist_ratio = 14.4  where vegetation turns into a sprite
// (+0x100 e_max_view_distance = 0 is "do not change" and stays.) The per object factors come from the level; every use
// computes from the global ratios anew (per object and frame), so a change shows from the next frame. The level's fog
// and far plane still end the view.
//
// lod_distance scales the LOD and the sprite ratios, view_distance the view distance ratio: once after the registration
// (the game restarts the program between levels and the hook runs again), and when the settings menu changes them. 1
// writes nothing. A field whose value is no longer the one written last (the game or its console changed it) counts as
// the new console value.
//
// FCP_TRACE_LOD=1: the three fields every 5 s (to see whether anything else writes them).
#include "fcp/detail_distance.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>

#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"

REX_EXTERN(__imp__Game_EngineCVars);

namespace fcp::detail {
namespace {

constexpr float kMin = 0.5f, kMax = 8.0f;

struct Field {
  uint32_t offset;
  const char* name;
  bool view;           // view_distance, else lod_distance
  float console = 0;   // the value as registered (or as the game set it since)
  float written = 0;   // what was last written
};
Field g_fields[] = {{0xF4, "e_obj_lod_ratio", false},
                    {0xF8, "e_obj_view_dist_ratio", true},
                    {0xFC, "e_obj_sprite_switch_dist_ratio", false}};
constexpr uint32_t kBlockSize = 0x104;

std::mutex g_mutex;
uint8_t* g_base = nullptr;
uint32_t g_block = 0;  // 0 until registered
float g_lod = 1.0f, g_view = 1.0f;
bool g_configured = false;  // factors read from the environment or set by the menu

float GetF(uint8_t* base, uint32_t a) {
  const uint32_t u = fcp::guest::U32(base, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

void SetF(uint8_t* base, uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  u = __builtin_bswap32(u);
  std::memcpy(fcp::guest::Ptr(base, a), &u, 4);
}

// "1", "2", "1.5", "150%", "2x"; empty = 1
float Parse(const char* env) {
  std::string v = fcp::env::Get(env);
  for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  while (!v.empty() && (v.back() == ' ' || v.back() == 'x')) v.pop_back();
  if (v.empty()) return 1.0f;
  const bool percent = v.back() == '%';
  if (percent) v.pop_back();
  char* end = nullptr;
  double f = std::strtod(v.c_str(), &end);
  if (!end || end == v.c_str() || *end || !(f > 0)) {
    FCPLOG("lod: {}={}: not a factor (1, 2, 1.5, 150%), 1 taken", env, v);
    return 1.0f;
  }
  if (percent) f /= 100.0;
  const double c = std::clamp(f, double(kMin), double(kMax));
  if (c != f) FCPLOG("lod: {}={} out of {}..{}, {} taken", env, f, kMin, kMax, c);
  return float(c);
}

// under g_mutex
void Apply(const char* why) {
  if (!g_block) return;
  std::string text;
  for (Field& f : g_fields) {
    const uint32_t a = g_block + f.offset;
    const float now = GetF(g_base, a);
    if (now != f.written) f.console = now;  // changed since by someone else: that is the console's value now
    const float k = f.view ? g_view : g_lod;
    const float want = k == 1.0f ? f.console : f.console * k;
    if (want != now) SetF(g_base, a, want);
    f.written = want;
    text += std::format("{} {} {:g}", text.empty() ? "" : ",", f.name, want);
  }
  FCPLOG("lod: lod distance x{:.2f}, view distance x{:.2f} ({}, variables at {:08X}):{}", g_lod, g_view, why, g_block,
         text);
}

void StartTrace() {
  if (!fcp::env::Flag("FCP_TRACE_LOD")) return;
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      for (;;) {
        Sleep(5000);
        std::lock_guard lock(g_mutex);
        std::string text;
        for (const Field& f : g_fields)
          text += std::format(" {} {:g}{}", f.name, GetF(g_base, g_block + f.offset),
                              GetF(g_base, g_block + f.offset) != f.written ? " (changed by the game)" : "");
        FCPLOG("lod: trace:{}", text);
      }
    }).detach();
  });
}

}  // namespace

void SetLodDistance(float factor) {
  std::lock_guard lock(g_mutex);
  g_lod = std::clamp(factor, kMin, kMax);
  g_configured = true;
  Apply("settings menu");
}

void SetViewDistance(float factor) {
  std::lock_guard lock(g_mutex);
  g_view = std::clamp(factor, kMin, kMax);
  g_configured = true;
  Apply("settings menu");
}

void OnEngineCVars(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const uint32_t block = ctx.r3.u32;
  original(ctx, base);
  if (!fcp::guest::Readable(base, block, kBlockSize)) {
    FCPLOG("lod: variable block {:08X} not readable, level of detail left as the console", block);
    return;
  }
  std::lock_guard lock(g_mutex);
  if (!g_configured) {
    g_configured = true;
    g_lod = Parse("FCP_LOD_DISTANCE");
    g_view = Parse("FCP_VIEW_DISTANCE");
  }
  g_base = base;
  g_block = block;
  for (Field& f : g_fields) f.console = f.written = GetF(base, block + f.offset);
  Apply("engine start");
  StartTrace();
}

}  // namespace fcp::detail

REX_HOOK_RAW(Game_EngineCVars) {
  ::fcp::detail::OnEngineCVars(ctx, base, __imp__Game_EngineCVars);
}
