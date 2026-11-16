// PC shadows: [graphics] shadow_distance / FCP_SHADOW_DISTANCE (1 to 4), default 1 = as the console (nothing written).
//
// The idea is from Far Cry Instincts Recomp (FCI; src/xdk/shadows.cpp, MIT, same author): scale the radius of the
// sun's shadow square. Rewritten for the Xbox 360 title; the layout below was read from this image.
//
// Game_ShadowPass (config/predator/default.toml) renders the shadow maps of the frame (one 1344x1344 map, or four
// 672x672 ones; r5 = the view's environment [engine +0x1424]). Its caller copies the eased radius (env +0x424, level
// value DynamicShadowRadius, 10 m) to env +0x34 every frame just before the call (0 with r_DynamicShadow off). For the
// sun the pass puts an orthographic camera over a square of side max(R, 2 R sin(fov / 2)) ahead of the camera
// (0x825D2220); casters and the receivers' shadow matrix come from that camera. shadow_distance multiplies env +0x34
// at the start of the pass; the caller writes the game's value again the next frame, the eased value stays the game's.
// The renderer raises the shadow map's resolution by the same factor (fcp/render_bridge.cpp), so a shadow texel keeps
// its size. Measured: at 4 the pass draws more casters (02_native: 19 -> 65 draws into the
// map) and objects 20-30 m ahead get the shadows of the trees above them.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>

#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/log.h"

REX_EXTERN(__imp__Game_ShadowPass);

namespace fcp::shadow_distance {
namespace {

constexpr uint32_t kEnvRadius = 0x34;

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

float Factor() {
  static const float k = [] {
    std::string v = fcp::env::Get("FCP_SHADOW_DISTANCE");
    while (!v.empty() && (v.back() == ' ' || v.back() == 'x' || v.back() == 'X')) v.pop_back();
    if (v.empty()) return 1.0f;
    char* end = nullptr;
    const double f = std::strtod(v.c_str(), &end);
    if (!end || end == v.c_str() || *end || !(f > 0)) {
      FCPLOG("shadows: FCP_SHADOW_DISTANCE={}: not a factor, 1 taken", v);
      return 1.0f;
    }
    const float c = float(std::clamp(f, 1.0, 4.0));
    FCPLOG("shadows: shadow distance x{:.2f}{}", c, c != f ? " (clamped to 1..4)" : "");
    return c;
  }();
  return k;
}

std::atomic<uint64_t> g_passes{0};

// FCP_SHADOW_DISTANCE_FLIP=<s> (test): the factor alternates between the console's (1) and shadow_distance every s seconds
// (the map keeps the larger size), for A/B pictures of the same frame with the game paused.
float FlipFactor(float k) {
  static const double period = double(fcp::env::Int("FCP_SHADOW_DISTANCE_FLIP", 0));
  if (period <= 0) return k;
  static int last = -1;
  const double t = fcp::gametime::WallSeconds();  // the clock of FCP_TEST_MENU
  const int phase = int(t / period) & 1;
  if (phase != last) {
    last = phase;
    FCPLOG("shadows: FCP_SHADOW_DISTANCE_FLIP: factor {:g}", phase ? 1.0f : k);
  }
  return phase ? 1.0f : k;
}
float g_written = -1.0f;  // last value written (the pass may run twice in a frame: split screen)

}  // namespace

void OnShadowPass(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  const float k = FlipFactor(Factor());
  const uint32_t env = ctx.r5.u32;
  if (k != 1.0f && fcp::guest::Readable(base, env + kEnvRadius, 4)) {
    const float r = GetF(base, env + kEnvRadius);
    if (r > 0.0f && r != g_written) {
      g_written = r * k;
      SetF(base, env + kEnvRadius, g_written);
      if (g_passes.fetch_add(1, std::memory_order_relaxed) < 3)
        FCPLOG("shadows: pass radius {:g} -> {:g} m (environment {:08X})", r, g_written, env);
    }
  }
  original(ctx, base);
}

}  // namespace fcp::shadow_distance

REX_HOOK_RAW(Game_ShadowPass) {
  ::fcp::shadow_distance::OnShadowPass(ctx, base, __imp__Game_ShadowPass);
}
