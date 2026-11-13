#include "fcp/input_script.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp/watchdog.h"

namespace fcp::input_script {
namespace {

constexpr int kUsers = 4;
constexpr uint32_t kErrorSuccess = 0;

enum Axis : int { kLX, kLY, kRX, kRY, kLT, kRT, kAxes };

struct Entry {
  double at = 0;
  double until = 0;
  int user = 0;
  uint16_t buttons = 0;
  std::array<float, kAxes> axis{};  // stick axes -1..1, triggers 0..1
  std::array<bool, kAxes> has{};
  std::string text;
  bool logged = false;
};

// One pad's script overlay for the current call.
struct Overlay {
  uint16_t buttons = 0;
  std::array<float, kAxes> axis{};
  std::array<bool, kAxes> has{};
  bool any = false;
  bool operator==(const Overlay&) const = default;
};

std::vector<Entry> g_entries;
bool g_active = false;
bool g_level_clock = false;
double g_hold_s = 0.15;
std::array<bool, kUsers> g_users{};      // users named in the script (virtual pad when the SDK has none)
std::array<Overlay, kUsers> g_last{};    // last overlay per user, for the packet number
std::array<uint32_t, kUsers> g_serial{};  // overlay changes per user
std::array<bool, kUsers> g_virtual_logged{};

uint16_t ButtonMask(std::string_view name) {
  struct B {
    const char* n;
    uint16_t m;
  };
  static constexpr B kButtons[] = {{"up", 0x0001},    {"down", 0x0002}, {"left", 0x0004},
                                   {"right", 0x0008}, {"start", 0x0010}, {"back", 0x0020},
                                   {"ls", 0x0040},    {"rs", 0x0080},   {"lb", 0x0100},
                                   {"rb", 0x0200},    {"a", 0x1000},    {"b", 0x2000},
                                   {"x", 0x4000},     {"y", 0x8000}};
  for (const B& b : kButtons) {
    if (name == b.n) {
      return b.m;
    }
  }
  return 0;
}

// Analog control: axis and sign (stick directions), or trigger. Returns false for unknown names.
bool AnalogControl(std::string_view name, Axis& axis, float& sign) {
  struct A {
    const char* n;
    Axis axis;
    float sign;
  };
  static constexpr A kAnalog[] = {{"lup", kLY, 1},    {"ldown", kLY, -1}, {"lleft", kLX, -1}, {"lright", kLX, 1},
                                  {"rup", kRY, 1},    {"rdown", kRY, -1}, {"rleft", kRX, -1}, {"rright", kRX, 1},
                                  {"lt", kLT, 1},     {"rt", kRT, 1}};
  for (const A& a : kAnalog) {
    if (name == a.n) {
      axis = a.axis;
      sign = a.sign;
      return true;
    }
  }
  return false;
}

std::vector<std::string_view> Split(std::string_view text, char sep) {
  std::vector<std::string_view> out;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find(sep, pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    out.push_back(text.substr(pos, end - pos));
    pos = end + 1;
  }
  return out;
}

double Number(std::string_view s, bool& ok) {
  const std::string copy(s);
  char* end = nullptr;
  const double v = std::strtod(copy.c_str(), &end);
  ok = end && end != copy.c_str() && *end == 0;
  return v;
}

bool ParseEntry(std::string_view item, Entry& e) {
  e.text = std::string(item);
  if (const size_t at = item.rfind('@'); at != std::string_view::npos) {
    bool ok = false;
    const double player = Number(item.substr(at + 1), ok);
    if (!ok || player < 1 || player > kUsers) {
      return false;
    }
    e.user = int(player) - 1;
    item = item.substr(0, at);
  }
  const std::vector<std::string_view> parts = Split(item, ':');
  if (parts.size() < 2 || parts.size() > 3) {
    return false;
  }
  // Time or time range.
  bool ok = false;
  const std::string_view times = parts[0];
  if (const size_t dash = times.find('-', 1); dash != std::string_view::npos) {
    e.at = Number(times.substr(0, dash), ok);
    if (!ok) {
      return false;
    }
    e.until = Number(times.substr(dash + 1), ok);
    if (!ok || e.until <= e.at) {
      return false;
    }
  } else {
    e.at = Number(times, ok);
    if (!ok) {
      return false;
    }
    e.until = e.at + g_hold_s;
  }
  float common = 1.0f;
  if (parts.size() == 3) {
    common = float(Number(parts[2], ok));
    if (!ok) {
      return false;
    }
  }
  for (std::string_view control : Split(parts[1], '+')) {
    float value = common;
    if (const size_t eq = control.find('='); eq != std::string_view::npos) {
      value = float(Number(control.substr(eq + 1), ok));
      if (!ok) {
        return false;
      }
      control = control.substr(0, eq);
    }
    Axis axis;
    float sign = 1;
    if (const uint16_t mask = ButtonMask(control)) {
      e.buttons |= mask;
    } else if (AnalogControl(control, axis, sign)) {
      e.axis[axis] = std::clamp(value, 0.0f, 1.0f) * sign;
      e.has[axis] = true;
    } else {
      return false;
    }
  }
  return true;
}

double Now() {
  return g_level_clock ? (fcp::gametime::InLevel() ? fcp::gametime::LevelSeconds() : -1.0)
                       : fcp::gametime::WallSeconds();
}

Overlay Collect(uint32_t user) {
  Overlay o;
  const double now = Now();
  if (now < 0) {
    return o;
  }
  for (Entry& e : g_entries) {
    if (e.user != int(user) || now < e.at || now >= e.until) {
      continue;
    }
    o.buttons |= e.buttons;
    for (int a = 0; a < kAxes; ++a) {
      if (e.has[a]) {
        o.axis[a] = e.axis[a];
        o.has[a] = true;
      }
    }
    o.any = true;
    if (!e.logged) {
      e.logged = true;
      FCPLOG("input script: '{}' active at {:.3f} s ({} clock; wall {:.2f} s, frame {}, swaps={})", e.text, now,
             g_level_clock ? "level" : "wall", fcp::gametime::WallSeconds(), fcp::gametime::Frame(),
             fcp::watch::SwapCount());
    }
  }
  return o;
}

void PutU16(uint8_t* p, uint16_t v) {
  const uint16_t be = __builtin_bswap16(v);
  std::memcpy(p, &be, 2);
}

int16_t Stick(float v) {
  return int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
}

}  // namespace

void Configure() {
  const std::string script = fcp::env::Get("FCP_INPUT_SCRIPT");
  if (script.empty()) {
    return;
  }
  if (!fcp::devstart::ActiveInGeneration("FCP_INPUT_SCRIPT_GENS")) {
    FCPLOG("FCP_INPUT_SCRIPT: not applied in relaunch generation {} (FCP_INPUT_SCRIPT_GENS)",
           fcp::relaunch::Generation());
    return;
  }
  g_hold_s = double(fcp::env::Int("FCP_INPUT_HOLD_MS", 150)) / 1000.0;
  const std::string clock = fcp::env::Get("FCP_INPUT_SCRIPT_CLOCK");
  g_level_clock = clock == "level";
  if (!clock.empty() && clock != "level" && clock != "wall") {
    FCPLOG("FCP_INPUT_SCRIPT_CLOCK: '{}' unknown, using wall", clock);
  }
  for (std::string_view item : Split(script, ',')) {
    if (item.empty()) {
      continue;
    }
    Entry e;
    if (ParseEntry(item, e)) {
      g_users[e.user] = true;
      g_entries.push_back(std::move(e));
    } else {
      FCPLOG("FCP_INPUT_SCRIPT: entry '{}' ignored (<t>[-<t_end>]:<control>[+...][:<value>][@<player>])",
             std::string(item));
    }
  }
  g_active = !g_entries.empty();
  FCPLOG("FCP_INPUT_SCRIPT: {} entries, clock {}, hold {} ms, players {}{}{}{}", g_entries.size(),
         g_level_clock ? "level (game time since the level start)" : "wall (seconds since title launch)",
         int(g_hold_s * 1000), g_users[0] ? "1" : "", g_users[1] ? " 2" : "", g_users[2] ? " 3" : "",
         g_users[3] ? " 4" : "");
}

bool Active() {
  return g_active;
}

void OnLevelStart() {
  if (g_active && g_level_clock) {
    FCPLOG("input script: level clock starts (level '{}')", fcp::gametime::LevelName());
  }
}

void OnGetState(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state) {
  if (!g_active || user >= kUsers || !g_users[user] || !fcp::guest::Readable(base, state, 16)) {
    return;
  }
  const bool sdk_ok = ctx.r3.u32 == kErrorSuccess;
  const Overlay o = Collect(user);
  if (!(o == g_last[user])) {
    g_last[user] = o;
    ++g_serial[user];
  }
  if (!o.any && sdk_ok) {
    return;  // the SDK's own state, untouched
  }
  // X_INPUT_STATE (big endian): u32 packet, u16 buttons, u8 lt, u8 rt, s16 lx, ly, rx, ry.
  uint8_t* s = fcp::guest::Ptr(base, state);
  if (!sdk_ok) {
    std::memset(s, 0, 16);  // virtual pad: neutral state
    if (!g_virtual_logged[user]) {
      g_virtual_logged[user] = true;
      FCPLOG("input script: player {} has no pad in the SDK (result {:08X}), the script provides one", user + 1,
             ctx.r3.u32);
    }
  }
  const uint32_t packet = fcp::guest::U32(base, state) + g_serial[user];
  const uint32_t packet_be = __builtin_bswap32(packet);
  std::memcpy(s, &packet_be, 4);
  PutU16(s + 4, uint16_t(fcp::guest::U16(base, state + 4) | o.buttons));
  if (o.has[kLT]) {
    s[6] = uint8_t(std::lround(std::clamp(o.axis[kLT], 0.0f, 1.0f) * 255.0f));
  }
  if (o.has[kRT]) {
    s[7] = uint8_t(std::lround(std::clamp(o.axis[kRT], 0.0f, 1.0f) * 255.0f));
  }
  static constexpr Axis kSticks[4] = {kLX, kLY, kRX, kRY};
  for (int i = 0; i < 4; ++i) {
    if (o.has[kSticks[i]]) {
      PutU16(s + 8 + 2 * i, uint16_t(Stick(o.axis[kSticks[i]])));
    }
  }
  ctx.r3.u64 = kErrorSuccess;
}

void OnGetCapabilities(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t caps) {
  if (!g_active || user >= kUsers || !g_users[user] || ctx.r3.u32 == kErrorSuccess ||
      !fcp::guest::Readable(base, caps, 20)) {
    return;
  }
  // X_INPUT_CAPABILITIES: u8 type (1 gamepad), u8 subtype (1 gamepad), u16 flags, X_INPUT_GAMEPAD (all
  // controls supported), X_INPUT_VIBRATION.
  uint8_t* c = fcp::guest::Ptr(base, caps);
  std::memset(c, 0, 20);
  c[0] = 1;
  c[1] = 1;
  PutU16(c + 4, 0xFFFF);
  c[6] = 0xFF;
  c[7] = 0xFF;
  for (int i = 0; i < 4; ++i) {
    PutU16(c + 8 + 2 * i, 0x7FFF);
  }
  PutU16(c + 16, 0xFFFF);
  PutU16(c + 18, 0xFFFF);
  ctx.r3.u64 = kErrorSuccess;
}

}  // namespace fcp::input_script
