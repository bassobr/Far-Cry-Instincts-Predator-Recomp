#include "fcp/input_trace.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"
#include "fcp/watchdog.h"

namespace fcp::input_trace {
namespace {

constexpr uint32_t kUsers = 4;
constexpr uint32_t kNotConnected = 0x48F;

struct Pad {
  uint16_t buttons;
  uint8_t lt, rt;
  int16_t lx, ly, rx, ry;
  bool operator==(const Pad&) const = default;
};

struct User {
  uint64_t get_calls = 0, get_ok = 0, get_not_connected = 0, get_other = 0;
  uint64_t packet_changes = 0, button_samples = 0, stick_samples = 0, trigger_samples = 0;
  uint64_t state_changes = 0;
  uint32_t last_packet = 0;
  bool have_last = false;
  Pad last{};
  uint64_t set_calls = 0, set_ok = 0, set_nonzero = 0, set_changes = 0;
  uint16_t rumble_left = 0, rumble_right = 0;
  bool connected_logged = false, disconnected_logged = false;
};

bool g_enabled = true;
int64_t g_lines_max = 200;
std::atomic<int64_t> g_lines{0};
std::mutex g_mutex;  // the title polls from several threads
std::array<User, kUsers> g_users;
std::chrono::steady_clock::time_point g_t0;

double Now() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
}

bool Line() {
  return g_lines.fetch_add(1, std::memory_order_relaxed) < g_lines_max;
}

int16_t S16(uint8_t* base, uint32_t a) {
  return static_cast<int16_t>(fcp::guest::U16(base, a));
}

}  // namespace

void Configure() {
  g_enabled = fcp::env::Flag("FCP_INPUT_TRACE", true);
  g_lines_max = fcp::env::Int("FCP_INPUT_TRACE_LINES", 200);
  g_t0 = std::chrono::steady_clock::now();
}

bool Enabled() {
  return g_enabled;
}

void OnGetState(uint32_t user_index, uint32_t result, uint8_t* base, uint32_t state) {
  const uint32_t u = user_index & 0xFF;
  if (u >= kUsers) {
    return;
  }
  std::lock_guard lock(g_mutex);
  User& user = g_users[u];
  ++user.get_calls;
  if (result == kNotConnected) {
    ++user.get_not_connected;
    if (!user.disconnected_logged && user.have_last && Line()) {
      FCPLOG("input trace: user {} not connected at {:.2f} s", u, Now());
    }
    user.disconnected_logged = true;
    user.have_last = false;
    return;
  }
  if (result != 0 || !fcp::guest::Plausible(state)) {
    ++user.get_other;
    return;
  }
  ++user.get_ok;
  user.disconnected_logged = false;
  // X_INPUT_STATE: u32 packet; X_INPUT_GAMEPAD { u16 buttons; u8 lt, rt; i16 lx, ly, rx, ry } (BE).
  const uint32_t packet = fcp::guest::U32(base, state);
  Pad pad;
  pad.buttons = fcp::guest::U16(base, state + 4);
  pad.lt = *fcp::guest::Ptr(base, state + 6);
  pad.rt = *fcp::guest::Ptr(base, state + 7);
  pad.lx = S16(base, state + 8);
  pad.ly = S16(base, state + 10);
  pad.rx = S16(base, state + 12);
  pad.ry = S16(base, state + 14);
  if (!user.connected_logged) {
    user.connected_logged = true;
    if (Line()) {
      FCPLOG("input trace: user {} first state at {:.2f} s (packet {})", u, Now(), packet);
    }
  }
  user.packet_changes += (user.have_last && packet != user.last_packet) ? 1 : 0;
  user.button_samples += pad.buttons ? 1 : 0;
  auto big = [](int16_t v) { return v > 8000 || v < -8000; };
  user.stick_samples += (big(pad.lx) || big(pad.ly) || big(pad.rx) || big(pad.ry)) ? 1 : 0;
  user.trigger_samples += (pad.lt > 30 || pad.rt > 30) ? 1 : 0;
  if (!user.have_last || !(pad == user.last)) {
    ++user.state_changes;
    if (Line()) {
      FCPLOG("input trace: user {} at {:.2f} s (swaps={}) packet {} buttons {:04X} lt {} rt {} "
             "lx {} ly {} rx {} ry {}",
             u, Now(), fcp::watch::SwapCount(), packet, pad.buttons, pad.lt, pad.rt, pad.lx, pad.ly,
             pad.rx, pad.ry);
    }
  }
  user.last = pad;
  user.last_packet = packet;
  user.have_last = true;
}

void OnSetState(uint32_t user_index, uint32_t result, uint8_t* base, uint32_t vibration) {
  const uint32_t u = (user_index & 0xFF) == 0xFF ? 0 : user_index;
  if (u >= kUsers || !fcp::guest::Plausible(vibration)) {
    return;
  }
  std::lock_guard lock(g_mutex);
  User& user = g_users[u];
  ++user.set_calls;
  user.set_ok += result == 0 ? 1 : 0;
  // X_INPUT_VIBRATION: u16 left_motor, u16 right_motor (BE).
  const uint16_t left = fcp::guest::U16(base, vibration);
  const uint16_t right = fcp::guest::U16(base, vibration + 2);
  user.set_nonzero += (left || right) ? 1 : 0;
  if (left != user.rumble_left || right != user.rumble_right || user.set_calls == 1) {
    ++user.set_changes;
    if (Line()) {
      FCPLOG("input trace: user {} vibration at {:.2f} s left {} right {} (result {:X})", u, Now(),
             left, right, result);
    }
  }
  user.rumble_left = left;
  user.rumble_right = right;
}

void WriteSummary() {
  if (!g_enabled) {
    return;
  }
  std::lock_guard lock(g_mutex);
  for (uint32_t u = 0; u < kUsers; ++u) {
    const User& s = g_users[u];
    FCPLOG("input summary: user {} GetState calls={} ok={} not_connected={} other={} | packet "
           "changes={} state changes={} samples with buttons={} sticks>8000={} triggers>30={} | "
           "SetState calls={} ok={} nonzero={} value changes={}",
           u, s.get_calls, s.get_ok, s.get_not_connected, s.get_other, s.packet_changes,
           s.state_changes, s.button_samples, s.stick_samples, s.trigger_samples, s.set_calls,
           s.set_ok, s.set_nonzero, s.set_changes);
  }
}

}  // namespace fcp::input_trace
