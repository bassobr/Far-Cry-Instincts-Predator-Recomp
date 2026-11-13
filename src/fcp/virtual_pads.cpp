#include "fcp/virtual_pads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>

#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/log.h"

namespace fcp::virtual_pads {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int kMaxPads = 4;

struct Pad {
  int index = 0;
  SDL_JoystickID id = 0;
  SDL_Joystick* joystick = nullptr;
  std::atomic<uint64_t> rumbles{0};
  std::atomic<uint64_t> rumble_nonzero{0};
  std::atomic<int> rumble_lines{0};
};

enum class Kind { kButton, kAxis, kPlug, kUnplug };

struct Step {
  double at;
  int pad;
  Kind kind;
  int target;    // SDL_GamepadButton or SDL_GamepadAxis
  Sint16 value;  // axis value
  std::string text;
};

std::array<Pad, kMaxPads> g_pads;
int g_count = 0;
std::vector<Step> g_steps;
double g_hold = 0.15;
Uint16 g_vendor = 0, g_product = 0;
std::thread g_thread;
std::atomic<bool> g_stop{false};
Clock::time_point g_t0;
int g_rumble_lines = 50;  // FCP_VIRTUAL_PAD_RUMBLE_LINES

bool SDLCALL OnRumble(void* userdata, Uint16 low, Uint16 high) {
  Pad& pad = *static_cast<Pad*>(userdata);
  pad.rumbles.fetch_add(1, std::memory_order_relaxed);
  if (low || high) {
    pad.rumble_nonzero.fetch_add(1, std::memory_order_relaxed);
  }
  if (pad.rumble_lines.fetch_add(1, std::memory_order_relaxed) < g_rumble_lines) {
    const double t = std::chrono::duration<double>(Clock::now() - g_t0).count();
    FCPLOG("virtual pad {}: rumble reached SDL at {:.2f} s: low {} high {}", pad.index, t, low, high);
  }
  return true;
}

bool Attach(Pad& pad) {
  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.vendor_id = g_vendor;
  desc.product_id = g_product;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  const std::string name = "FCP virtual pad " + std::to_string(pad.index);
  desc.name = name.c_str();
  desc.userdata = &pad;
  desc.Rumble = &OnRumble;
  pad.id = SDL_AttachVirtualJoystick(&desc);
  if (!pad.id) {
    FCPLOG("virtual pad {}: SDL_AttachVirtualJoystick failed: {}", pad.index, SDL_GetError());
    return false;
  }
  pad.joystick = SDL_OpenJoystick(pad.id);
  if (!pad.joystick) {
    FCPLOG("virtual pad {}: SDL_OpenJoystick failed: {}", pad.index, SDL_GetError());
    SDL_DetachVirtualJoystick(pad.id);
    pad.id = 0;
    return false;
  }
  // Released triggers sit at the bottom of the joystick range.
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
  SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
  FCPLOG("virtual pad {}: attached as SDL joystick {} (gamepad: {}, type {}, \"{}\")", pad.index,
         pad.id, SDL_IsGamepad(pad.id), static_cast<int>(SDL_GetGamepadTypeForID(pad.id)),
         SDL_GetJoystickNameForID(pad.id) ? SDL_GetJoystickNameForID(pad.id) : "?");
  return true;
}

void Detach(Pad& pad) {
  if (pad.joystick) {
    SDL_CloseJoystick(pad.joystick);
    pad.joystick = nullptr;
  }
  if (pad.id) {
    SDL_DetachVirtualJoystick(pad.id);
    FCPLOG("virtual pad {}: detached (SDL joystick {})", pad.index, pad.id);
    pad.id = 0;
  }
}

int ButtonOf(std::string_view n) {
  struct B {
    const char* name;
    SDL_GamepadButton button;
  };
  static constexpr B kButtons[] = {
      {"a", SDL_GAMEPAD_BUTTON_SOUTH},          {"b", SDL_GAMEPAD_BUTTON_EAST},
      {"x", SDL_GAMEPAD_BUTTON_WEST},           {"y", SDL_GAMEPAD_BUTTON_NORTH},
      {"back", SDL_GAMEPAD_BUTTON_BACK},        {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
      {"start", SDL_GAMEPAD_BUTTON_START},      {"ls", SDL_GAMEPAD_BUTTON_LEFT_STICK},
      {"rs", SDL_GAMEPAD_BUTTON_RIGHT_STICK},   {"lb", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {"rb", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
      {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},   {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
      {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT}};
  for (const B& b : kButtons) {
    if (n == b.name) {
      return b.button;
    }
  }
  return -1;
}

int AxisOf(std::string_view n) {
  if (n == "lx") return SDL_GAMEPAD_AXIS_LEFTX;
  if (n == "ly") return SDL_GAMEPAD_AXIS_LEFTY;
  if (n == "rx") return SDL_GAMEPAD_AXIS_RIGHTX;
  if (n == "ry") return SDL_GAMEPAD_AXIS_RIGHTY;
  if (n == "lt") return SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
  if (n == "rt") return SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
  return -1;
}

void Parse(const std::string& text) {
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(',', pos);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string item = text.substr(pos, end - pos);
    pos = end + 1;
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const double at = std::strtod(item.c_str(), nullptr);
    std::string rest = item.substr(colon + 1);
    int pad = 0;
    if (rest.size() > 3 && rest[0] == 'p' && rest[2] == '.') {
      pad = rest[1] - '0';
      rest = rest.substr(3);
    }
    for (size_t p = 0; p <= rest.size();) {
      size_t plus = rest.find('+', p);
      if (plus == std::string::npos) {
        plus = rest.size();
      }
      const std::string step = rest.substr(p, plus - p);
      p = plus + 1;
      Step s{at, pad, Kind::kButton, -1, 0, item};
      const size_t eq = step.find('=');
      if (step == "plug") {
        s.kind = Kind::kPlug;
      } else if (step == "unplug") {
        s.kind = Kind::kUnplug;
      } else if (eq != std::string::npos) {
        s.kind = Kind::kAxis;
        s.target = AxisOf(step.substr(0, eq));
        double v = std::strtod(step.c_str() + eq + 1, nullptr);
        if (s.target == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || s.target == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
          v = std::clamp(v, 0.0, 1.0);
          s.value = static_cast<Sint16>(SDL_JOYSTICK_AXIS_MIN + v * 65535.0);
        } else {
          v = std::clamp(v, -1.0, 1.0);
          if (s.target == SDL_GAMEPAD_AXIS_LEFTY || s.target == SDL_GAMEPAD_AXIS_RIGHTY) {
            v = -v;  // SDL: positive y is down; the script uses XInput's up positive
          }
          s.value = static_cast<Sint16>(v * 32767.0);
        }
      } else {
        s.target = ButtonOf(step);
      }
      if ((s.kind == Kind::kButton || s.kind == Kind::kAxis) && s.target < 0) {
        FCPLOG("FCP_VIRTUAL_PAD_SCRIPT: step '{}' in '{}' ignored (unknown)", step, item);
        continue;
      }
      if (pad < 0 || pad >= g_count) {
        FCPLOG("FCP_VIRTUAL_PAD_SCRIPT: step '{}' ignored (pad {} not created)", item, pad);
        continue;
      }
      g_steps.push_back(s);
    }
  }
  std::stable_sort(g_steps.begin(), g_steps.end(),
                   [](const Step& a, const Step& b) { return a.at < b.at; });
}

void Run() {
  // The SDK input driver initialises SDL's gamepad subsystem on the UI thread at start.
  for (int i = 0; i < 500 && !g_stop && !SDL_WasInit(SDL_INIT_GAMEPAD); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
    FCPLOG("FCP_VIRTUAL_PADS: SDL gamepad subsystem not initialised (hid.drivers without sdl3?)");
    return;
  }
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  for (int i = 0; i < g_count; ++i) {
    g_pads[i].index = i;
    Attach(g_pads[i]);
  }
  struct Release {
    double at;
    int pad;
    int button;
  };
  std::vector<Release> releases;
  size_t next = 0;
  while (!g_stop) {
    const double now = std::chrono::duration<double>(Clock::now() - g_t0).count();
    while (next < g_steps.size() && g_steps[next].at <= now) {
      const Step& s = g_steps[next++];
      Pad& pad = g_pads[s.pad];
      switch (s.kind) {
        case Kind::kPlug:
          if (!pad.id) {
            Attach(pad);
          }
          break;
        case Kind::kUnplug:
          Detach(pad);
          break;
        case Kind::kAxis:
          if (pad.joystick) {
            SDL_SetJoystickVirtualAxis(pad.joystick, s.target, s.value);
          }
          break;
        case Kind::kButton:
          if (pad.joystick) {
            SDL_SetJoystickVirtualButton(pad.joystick, s.target, true);
            releases.push_back({now + g_hold, s.pad, s.target});
          }
          break;
      }
      FCPLOG("virtual pad {}: step '{}' at {:.2f} s{}", s.pad, s.text, now,
             pad.joystick || s.kind == Kind::kUnplug ? "" : " (pad not attached)");
    }
    for (auto it = releases.begin(); it != releases.end();) {
      if (it->at <= now) {
        if (g_pads[it->pad].joystick) {
          SDL_SetJoystickVirtualButton(g_pads[it->pad].joystick, it->button, false);
        }
        it = releases.erase(it);
      } else {
        ++it;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

}  // namespace

void Start() {
  g_count = static_cast<int>(std::clamp<int64_t>(fcp::env::Int("FCP_VIRTUAL_PADS", 0), 0, kMaxPads));
  if (g_count == 0) {
    return;
  }
  g_t0 = Clock::now();
  g_hold = double(fcp::env::Int("FCP_VIRTUAL_PAD_HOLD_MS", 150)) / 1000.0;
  g_rumble_lines = int(std::clamp<int64_t>(fcp::env::Int("FCP_VIRTUAL_PAD_RUMBLE_LINES", 50), 0, 1000000));
  if (const std::string id = fcp::env::Get("FCP_VIRTUAL_PAD_ID"); !id.empty()) {
    g_vendor = static_cast<Uint16>(std::strtoul(id.c_str(), nullptr, 16));
    if (const size_t c = id.find(':'); c != std::string::npos) {
      g_product = static_cast<Uint16>(std::strtoul(id.c_str() + c + 1, nullptr, 16));
    }
  }
  if (const std::string script = fcp::env::Get("FCP_VIRTUAL_PAD_SCRIPT"); !script.empty()) {
    if (fcp::devstart::ActiveInGeneration("FCP_INPUT_SCRIPT_GENS")) {
      Parse(script);
    } else {
      FCPLOG("FCP_VIRTUAL_PAD_SCRIPT: not applied in this relaunch generation (FCP_INPUT_SCRIPT_GENS)");
    }
  }
  FCPLOG("FCP_VIRTUAL_PADS: {} virtual SDL pads, {} script steps, hold {} ms, id {:04X}:{:04X}", g_count,
         g_steps.size(), int(g_hold * 1000), g_vendor, g_product);
  g_thread = std::thread(Run);
}

void Stop() {
  if (!g_thread.joinable()) {
    return;
  }
  g_stop = true;
  g_thread.join();
  for (int i = 0; i < g_count; ++i) {
    FCPLOG("virtual pad {}: rumble calls that reached SDL: {} (non-zero {})", i, g_pads[i].rumbles.load(),
           g_pads[i].rumble_nonzero.load());
  }
  // The pads stay attached: the SDK input driver shuts SDL's gamepad subsystem down on its own thread.
}

}  // namespace fcp::virtual_pads
