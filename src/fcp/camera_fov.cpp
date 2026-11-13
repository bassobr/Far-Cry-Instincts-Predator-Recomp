// Field of view multiplier (PC comfort): [graphics] fov / FCP_RENDER_FOV, default 1.
//
// The idea is from Far Cry Instincts Recomp (FCI; src/xdk/game.cpp, Game_CameraUpdate; MIT, same
// author): widen the camera's own field of view for its update, so the projection, the culling frustum and
// everything derived from the camera follow it consistently, and leave the first person view (weapon, hands) with its
// own projection. Rewritten for the Xbox 360 title.
//
// The camera update (CCamera_Update in config/predator/default.toml; roles read from the image, statically): r3 = the
// camera (the entity's camera object [entity +0x11C] plus 4). It builds the projection at +0x23C with
// PerspectiveFovRH(fov / aspect, aspect, near +0x34, far +0x38) from fov +0x3C (horizontal, radians; 90 degrees
// normally) and aspect +0x1B4 when ortho +0x32C is 0, and the culling frustum from tan(fov / 2) in the same call.
// A second projection at +0x2BC uses its own angle +0x40 (near 0.001, far 8; presumably the weapon and hands) and
// stays as it is.
//
// Player_LookInput (the stick's zoom scaling), pc_input (the mouse's zoom scaling, fcp/pc_input.cpp) and Lua
// Game:SetCameraFov read the entity's camera (aspect 1), which is never widened. The view cameras keep the widened
// values after the call (the water reads them later; FCP_FOV_RESTORE=1 puts the game's back, as before 2026-11-13).
//
// Hor+ ([graphics] aspect): with an output wider than 16:9 the renderer makes the
// screen-sized targets `wide` times wider on the host (fcp::render::HorPlusWide). The projection's vertical angle is
// fov / aspect, so fov and aspect are both multiplied by `wide` for the call: the same vertical view, `wide` times
// the width, and the frustum from tan(fov / 2) wider with it. Where fov * wide would pass kMaxFov (32:9 at 90
// degrees), fov stops there for the frustum and the projection's two scale terms are set afterwards from the vertical
// angle (only after the matrix at +0x23C was found to be the PerspectiveFovRH of the values the call saw).
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"
#include "fcp/render_bridge.h"

REX_EXTERN(__imp__CCamera_Update);

namespace fcp::camera_fov {
namespace {

// ---- guest layout (CCamera, read from the image) ----
constexpr uint32_t kCamNear = 0x34, kCamFar = 0x38, kCamFov = 0x3C, kCamFovSecond = 0x40, kCamAspect = 0x1B4;
constexpr uint32_t kCamOrtho = 0x32C;
constexpr uint32_t kCamProjection = 0x23C;  // 4x4 floats, row-major: [0] x scale, [5] y scale (PerspectiveFovRH)
// The frustum's sides come from tan(fov / 2): the horizontal angle has to stay below pi (FCI: 3.05).
constexpr float kMaxFov = 3.05f;

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

float ParseFactor() {
  std::string v = fcp::env::Get("FCP_RENDER_FOV");
  for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  while (!v.empty() && (v.back() == ' ' || v.back() == 'x')) v.pop_back();
  if (v.empty()) return 1.0f;
  const bool percent = v.back() == '%';
  if (percent) v.pop_back();
  char* end = nullptr;
  double f = std::strtod(v.c_str(), &end);
  if (!end || end == v.c_str() || *end || !(f > 0)) {
    FCPLOG("fov: FCP_RENDER_FOV={}: not a factor (1, 1.2, 120%), 1 taken", v);
    return 1.0f;
  }
  if (percent) f /= 100.0;
  const double c = std::clamp(f, 0.5, 2.0);
  if (c != f) FCPLOG("fov: FCP_RENDER_FOV={} out of 0.5..2, {} taken", f, c);
  return float(c);
}

struct State {
  float factor = 1.0f;
  bool trace = false;
  std::mutex mutex;
  std::vector<uint64_t> seen;  // each camera shape and caller once
  std::atomic<uint64_t> calls{0}, widened{0}, copies{0};
  // Values written recently (widened -> the game's): the render thread copies the camera (0x820FD350) and updates
  // the copy itself (0x825CE7A4, measured). A copy taken while the main thread's update ran holds the widened value;
  // it must not be widened twice.
  struct Written {
    float wide = 0, game = 0;               // fov
    float wide_aspect = 0, game_aspect = 0;  // aspect (Hor+; equal without it)
  };
  std::array<Written, 16> written{};
  uint32_t next = 0;
  std::atomic<uint64_t> capped{0}, fixed{0}, layout_misses{0}, layout_logs{0};
};

State& Get() {
  static State* s = [] {
    auto* st = new State;
    st->factor = ParseFactor();
    st->trace = fcp::env::Flag("FCP_TRACE_CAMERA");
    FCPLOG("fov: factor {:.3f}{} (FCP_RENDER_FOV, [graphics] fov; the weapon's projection stays)", st->factor,
           st->factor == 1.0f ? " = as the console" : "");
    return st;
  }();
  return *s;
}

}  // namespace

void OnCameraUpdate(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  State& s = Get();
  const uint32_t cam = ctx.r3.u32;
  const uint32_t from = uint32_t(ctx.lr);
  if (!fcp::guest::Readable(base, cam, kCamOrtho + 4)) {
    original(ctx, base);
    return;
  }
  float fov = GetF(base, cam + kCamFov);
  float aspect = GetF(base, cam + kCamAspect);
  const float hor = fcp::render::HorPlusWide();  // Hor+: 1 = off
  if (s.factor != 1.0f || hor != 1.0f) {
    std::lock_guard lock(s.mutex);
    for (const State::Written& w : s.written)
      if (w.wide != 0 && fov == w.wide && aspect == w.wide_aspect && (w.wide != w.game || w.wide_aspect != w.game_aspect)) {
        // a copy of a camera while it was widened: the game's values are the base
        fov = w.game, aspect = w.game_aspect;
        if (s.copies.fetch_add(1, std::memory_order_relaxed) == 0)
          FCPLOG("fov: camera {:08X} holds a widened copy ({:.4f}/{:.4f}), the game's {:.4f}/{:.4f} taken as the base", cam, w.wide,
                 w.wide_aspect, w.game, w.game_aspect);
        break;
      }
  }
  const uint32_t ortho = fcp::guest::U32(base, cam + kCamOrtho);
  // The screen's perspective cameras (16:9 and the like), not square render-to-texture views or ortho ones (FCI's rule).
  const bool screen = ortho == 0 && aspect > 1.2f && aspect < 3.6f && fov > 0.05f && fov < 3.0f;
  const bool patch = screen && (s.factor != 1.0f || hor != 1.0f);
  float wide = fov, wide_aspect = aspect;
  bool capped = false;
  if (patch) {
    const float want = fov * s.factor * hor;
    wide = std::min(want, kMaxFov);
    capped = hor != 1.0f && want > kMaxFov;
    wide_aspect = aspect * hor;
  }
  s.calls.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(s.mutex);
    const uint64_t key = (uint64_t(from) << 32) ^ (uint64_t(uint32_t(aspect * 1000)) << 12) ^ uint32_t(fov * 1000) ^
                         (uint64_t(uint32_t(hor * 1000)) << 44);
    if (s.trace || (s.seen.size() < 32 && std::find(s.seen.begin(), s.seen.end(), key) == s.seen.end())) {
      if (!s.trace) s.seen.push_back(key);
      FCPLOG("fov: camera {:08X} from {:08X}: fov {:.4f} ({:.1f} deg) aspect {:.4f} near {:g} far {:g} second angle {:.4f} "
             "ortho {}{}",
             cam, from, fov, fov * 57.29578f, aspect, GetF(base, cam + kCamNear), GetF(base, cam + kCamFar),
             GetF(base, cam + kCamFovSecond), ortho,
             patch ? std::format(" -> {:.4f} ({:.1f} deg) aspect {:.4f}{}", wide, wide * 57.29578f, wide_aspect, capped ? " (capped)" : "")
                   : "");
    }
  }
  if (!patch) {
    original(ctx, base);
    return;
  }
  s.widened.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(s.mutex);
    bool known = false;
    for (const State::Written& w : s.written)
      known = known || (w.wide == wide && w.game == fov && w.wide_aspect == wide_aspect && w.game_aspect == aspect);
    if (!known) s.written[s.next++ % s.written.size()] = {wide, fov, wide_aspect, aspect};
  }
  SetF(base, cam + kCamFov, wide);
  if (wide_aspect != aspect) SetF(base, cam + kCamAspect, wide_aspect);
  original(ctx, base);
  if (hor != 1.0f && s.layout_logs.fetch_add(1, std::memory_order_relaxed) < 4) {
    // Diagnostics: the projection the call built, against PerspectiveFovRH(wide / wide_aspect, wide_aspect).
    const float ys_call = 1.0f / std::tan(wide / wide_aspect * 0.5f);
    FCPLOG("fov: Hor+ {:.4f}: projection x {:.4f} y {:.4f} (expected {:.4f} {:.4f}), vertical {:.1f} deg (game {:.1f}){}", hor,
           GetF(base, cam + kCamProjection), GetF(base, cam + kCamProjection + 20), ys_call / wide_aspect, ys_call,
           wide / wide_aspect * 57.29578f, fov * s.factor / aspect * 57.29578f, capped ? ", set below" : "");
  }
  if (capped && fcp::guest::U32(base, cam + kCamOrtho) == 0) {
    // Hor+ past kMaxFov: the frustum came from the capped fov; the projection gets the vertical angle the uncapped
    // values give (fov * factor / aspect) and the wide aspect. Only if the matrix is the one expected from the call's
    // values (layout check: y scale = cot(wide / wide_aspect / 2), x scale = y scale / wide_aspect).
    const float ys_call = 1.0f / std::tan(wide / wide_aspect * 0.5f);
    const float m00 = GetF(base, cam + kCamProjection), m11 = GetF(base, cam + kCamProjection + 20);
    s.capped.fetch_add(1, std::memory_order_relaxed);
    if (std::fabs(m11 - ys_call) <= 1e-3f * ys_call && std::fabs(m00 - ys_call / wide_aspect) <= 1e-3f * ys_call) {
      const float ys = 1.0f / std::tan(fov * s.factor / aspect * 0.5f);
      SetF(base, cam + kCamProjection, ys / wide_aspect);
      SetF(base, cam + kCamProjection + 20, ys);
      if (s.fixed.fetch_add(1, std::memory_order_relaxed) == 0)
        FCPLOG("fov: Hor+ past {:.2f} rad: projection set from the vertical angle (x {:.4f} y {:.4f}, was {:.4f} {:.4f})", kMaxFov,
               ys / wide_aspect, ys, m00, m11);
    } else if (s.layout_misses.fetch_add(1, std::memory_order_relaxed) == 0) {
      FCPLOG("fov: Hor+ past {:.2f} rad: projection at +0x{:X} not as expected (x {:.4f} y {:.4f}, expected {:.4f} {:.4f}); left "
             "as is (vertical view narrower)",
             kMaxFov, kCamProjection, m00, m11, ys_call / wide_aspect, ys_call);
    }
  }
  // The widened values stay in the camera (measured in 00_training): the water's grid is built later
  // from the camera's fov and aspect; with the game's values put back it covered only the 16:9 middle and its edge
  // showed at the sides (with Hor+ and with the fov factor alike). The game writes both fields again before every
  // update (measured: every call sees its own values), and a copy that still holds widened values is recognised
  // above. FCP_FOV_RESTORE=1: back to the game's values after the call, as before.
  static const bool restore = fcp::env::Flag("FCP_FOV_RESTORE", false);
  if (!restore) return;
  if (GetF(base, cam + kCamFov) == wide) SetF(base, cam + kCamFov, fov);
  if (wide_aspect != aspect && GetF(base, cam + kCamAspect) == wide_aspect) SetF(base, cam + kCamAspect, aspect);
}

}  // namespace fcp::camera_fov

REX_HOOK_RAW(CCamera_Update) {
  ::fcp::camera_fov::OnCameraUpdate(ctx, base, __imp__CCamera_Update);
}
