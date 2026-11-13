// Keyboard and mouse as player 1's pad, direct mouse look, pad connection state (fcp/pc_input.h).
//
// Taken over from FCE (Far Cry Instincts Evolution Recomp, own MIT project code of the same author, src/hle/pc_input.cpp and
// src/hle/mouse_look.cpp): the binding targets and default keys, the key names, the wheel as a short press,
// the test script for keys and mouse counts, the per-frame yaw step limit with carry-over. Rewritten for the
// Xbox 360 title (PowerPC guest, ReXGlue input system instead of Win32 polling).
//
// The game's look input (Player_LookInput, 0x8228BC78 in config/predator/default.toml; role read from the
// image, the same function as FCE 0x00030CB2): r3 = aim controller, r4 = the client's processing command
// (client +0x38C), f1/f2 = the actions TURNLR/TURNUD (client +0x6C/+0x70, the right stick). It snaps nearly
// straight deflections to one axis, curves and accelerates them, scales them by the frame time
// (FrameTimer_GetFrameTime) and by the view's field of view / 90 degrees ([[entity +0x11C] +0x40] * 2/pi,
// entity = EntitySystem_GetEntity(aim +0xF8)), and stores the frame's turn in degrees to the command: +0xBC yaw,
// +0xC0 pitch (from aim +0xB4/+0xB8, the last instructions). The hook runs it unchanged and adds the mouse's
// degrees on top, so the stick, aim assist and other players stay as they were.
#include "fcp/pc_input.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/hid/input_system.h>
#include <rex/hid/pad.h>
#include <rex/input_keys.h>
#include <rex/kernel/object/notification_broadcaster.h>
#include <rex/kernel/title_runtime.h>
#include <rex/platform/file.h>
#include <rex/ppc/hook.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "fcp/button_prompts.h"
#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/guest_call.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp/settings_menu.h"
#include "fcp/toml_edit.h"
#include "fcp_addresses.h"

REX_EXTERN(__imp__EntitySystem_GetEntity);

namespace fcp::pc_input {
namespace {

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr int kUsers = 4;

// ---- guest layout (read from the image, see the file header) ----
constexpr uint32_t kClientCommand = 0x38C;  // client +0x38C = processing command (r4 of the look input)
constexpr uint32_t kCmdYaw = 0xBC, kCmdPitch = 0xC0;
constexpr uint32_t kAimEntityId = 0xF8;
constexpr uint32_t kEntityCamera = 0x11C, kCameraFov = 0x40;
constexpr uint32_t kGameFirstLocal = 0x30;  // game object: local player slots (clients) from +0x30
constexpr uint32_t kGameLocalCount = 0x6C;

// ---- pad buttons (XINPUT_GAMEPAD) ----
enum : uint16_t {
  kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
  kLs = 0x0040, kRs = 0x0080, kLb = 0x0100, kRb = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};

// ---- key codes: Windows virtual keys (the SDK's VirtualKey), mouse buttons and the wheel ----
constexpr int kWheelUp = 0x0E, kWheelDown = 0x0F;  // codes Windows leaves unassigned (as FCE / SCDA)
constexpr int kMaxCode = 0x100;

struct KeyName {
  const char* name;
  int vk;
};
// The SDK reports either Shift/Ctrl/Alt as one key (VirtualKey kShift/kControl/kMenu).
constexpr KeyName kKeyNames[] = {
    {"space", VK_SPACE},       {"enter", VK_RETURN},     {"return", VK_RETURN},    {"escape", VK_ESCAPE},
    {"esc", VK_ESCAPE},        {"tab", VK_TAB},          {"backspace", VK_BACK},   {"capslock", VK_CAPITAL},
    {"shift", VK_SHIFT},       {"lshift", VK_SHIFT},     {"rshift", VK_SHIFT},     {"ctrl", VK_CONTROL},
    {"lctrl", VK_CONTROL},     {"rctrl", VK_CONTROL},    {"alt", VK_MENU},         {"lalt", VK_MENU},
    {"ralt", VK_MENU},         {"up", VK_UP},            {"down", VK_DOWN},        {"left", VK_LEFT},
    {"right", VK_RIGHT},       {"insert", VK_INSERT},    {"delete", VK_DELETE},    {"home", VK_HOME},
    {"end", VK_END},           {"pageup", VK_PRIOR},     {"pagedown", VK_NEXT},    {"minus", VK_OEM_MINUS},
    {"equals", VK_OEM_PLUS},   {"comma", VK_OEM_COMMA},  {"period", VK_OEM_PERIOD}, {"slash", VK_OEM_2},
    {"semicolon", VK_OEM_1},   {"apostrophe", VK_OEM_7}, {"lbracket", VK_OEM_4},   {"rbracket", VK_OEM_6},
    {"backslash", VK_OEM_5},   {"grave", VK_OEM_3},      {"mouse1", VK_LBUTTON},   {"lmb", VK_LBUTTON},
    {"mouse2", VK_RBUTTON},    {"rmb", VK_RBUTTON},      {"mouse3", VK_MBUTTON},   {"mmb", VK_MBUTTON},
    {"mouse4", VK_XBUTTON1},   {"mouse5", VK_XBUTTON2},  {"wheelup", kWheelUp},    {"wheeldown", kWheelDown},
};

std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  return s;
}

std::string Trim(std::string_view s) {
  const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}

int ParseKey(std::string_view text) {
  const std::string name = Lower(Trim(text));
  if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) {
    return std::toupper(static_cast<unsigned char>(name[0]));
  }
  if (name.size() >= 2 && name[0] == 'f' && std::isdigit(static_cast<unsigned char>(name[1]))) {
    const int n = std::atoi(name.c_str() + 1);
    if (n >= 1 && n <= 24) return VK_F1 + n - 1;
  }
  if (name.size() == 4 && name.compare(0, 3, "num") == 0 && std::isdigit(static_cast<unsigned char>(name[3]))) {
    return VK_NUMPAD0 + (name[3] - '0');
  }
  if (name.size() > 2 && name.compare(0, 2, "0x") == 0) {
    const int vk = int(std::strtol(name.c_str() + 2, nullptr, 16));
    return vk > 0 && vk < kMaxCode ? vk : -1;
  }
  for (const KeyName& k : kKeyNames) {
    if (name == k.name) return k.vk;
  }
  return -1;
}

std::string KeyLabel(int code) {
  for (const KeyName& k : kKeyNames) {
    if (k.vk == code) return k.name;
  }
  if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, char(std::tolower(code)));
  if (code >= VK_F1 && code <= VK_F24) return std::format("f{}", code - VK_F1 + 1);
  if (code >= VK_NUMPAD0 && code <= VK_NUMPAD9) return std::format("num{}", code - VK_NUMPAD0);
  return std::format("0x{:02x}", code);
}

// ---- binding targets: pad buttons, triggers, stick directions (names as FCP_INPUT_SCRIPT) ----
// The actions in the comments are the game's default action map "default"/"classic" (Lua table of
// common.dat, BindAction calls; order of the constants read from the bytecode, so the pairing is presumed).
struct Target {
  const char* name;
  uint16_t button;
  int trigger;      // 0 left, 1 right, -1 none
  int axis, sign;   // 0 LX 1 LY 2 RX 3 RY, -1 none
  const char* keys;  // default
};
constexpr Target kTargets[] = {
    {"a", kA, -1, -1, 0, "space"},             // JUMP
    {"b", kB, -1, -1, 0, "c, backspace"},      // EXOFIST_KICK; back in menus
    {"x", kX, -1, -1, 0, "e, r"},              // RELOAD_USE
    {"y", kY, -1, -1, 0, "q"},                 // TOGGLE_EVOLUTION
    {"lb", kLb, -1, -1, 0, "f"},               // gp_white: TOGGLE_PROJECTILE
    {"rb", kRb, -1, -1, 0, "g"},               // gp_black: NEXT_SELECTOR
    {"lt", 0, 0, -1, 0, "mouse2"},             // SECONDARY_FIRE
    {"rt", 0, 1, -1, 0, "mouse1"},             // FIRE
    {"ls", kLs, -1, -1, 0, "shift, ctrl"},     // MOVEMODE (stance)
    {"rs", kRs, -1, -1, 0, "v, mouse3"},       // ZOOM_TOGGLE
    {"start", kStart, -1, -1, 0, "enter, escape"},
    {"back", kBack, -1, -1, 0, "tab"},         // TOGGLE_SCOREBOARD
    {"up", kUp, -1, -1, 0, "up, 4"},           // TOGGLE_BINOCULARS
    {"down", kDown, -1, -1, 0, "down, 3"},     // SPECIAL_WEAPON
    {"left", kLeft, -1, -1, 0, "left, 1"},     // PRIMARY_WEAPON
    {"right", kRight, -1, -1, 0, "right, 2"},  // SECONDARY_WEAPON
    {"lup", 0, -1, 1, 1, "w"},                 // MOVE_FB
    {"ldown", 0, -1, 1, -1, "s"},
    {"lleft", 0, -1, 0, -1, "a"},              // MOVE_LR
    {"lright", 0, -1, 0, 1, "d"},
    {"rup", 0, -1, 3, 1, "i"},                 // TURNUD (keyboard look through the stick)
    {"rdown", 0, -1, 3, -1, "k"},
    {"rleft", 0, -1, 2, -1, "j"},              // TURNLR
    {"rright", 0, -1, 2, 1, "l"},
};
constexpr int kTargetCount = int(sizeof kTargets / sizeof kTargets[0]);

std::vector<int> ParseKeys(std::string_view list, const char* target) {
  std::vector<int> keys;
  size_t at = 0;
  while (at <= list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string_view::npos) end = list.size();
    const std::string name = Trim(list.substr(at, end - at));
    at = end + 1;
    if (name.empty() || Lower(name) == "none") continue;
    const int vk = ParseKey(name);
    if (vk < 0) {
      FCPLOG("input: binding {}: unknown key '{}'", target, name);
    } else {
      keys.push_back(vk);
    }
  }
  return keys;
}

// ---- pad buttons for pad_bind_<button> (names as FCI's pad_bind_*, own project code): a button of the Xbox
// pad the title reads, and the controller's buttons by their place on an Xbox pad, with PlayStation and
// direction names as aliases. The triggers are analog: on a digital button they press from 30/255
// (XINPUT_GAMEPAD_TRIGGER_THRESHOLD), a digital button on a trigger gives 0 or 255. ----
constexpr int kPadLt = 0x10000, kPadRt = 0x20000;  // trigger codes beside the XINPUT button bits
struct PadName {
  const char* name;
  int code;
};
constexpr PadName kPadNames[] = {
    {"a", kA},          {"south", kA},      {"cross", kA},      {"b", kB},          {"east", kB},
    {"circle", kB},     {"x", kX},          {"west", kX},       {"square", kX},     {"y", kY},
    {"north", kY},      {"triangle", kY},   {"lb", kLb},        {"l1", kLb},        {"rb", kRb},
    {"r1", kRb},        {"lt", kPadLt},     {"l2", kPadLt},     {"rt", kPadRt},     {"r2", kPadRt},
    {"ls", kLs},        {"l3", kLs},        {"lthumb", kLs},    {"rs", kRs},        {"r3", kRs},
    {"rthumb", kRs},    {"start", kStart},  {"options", kStart}, {"menu", kStart},  {"back", kBack},
    {"select", kBack},  {"share", kBack},   {"create", kBack},  {"view", kBack},    {"up", kUp},
    {"down", kDown},    {"left", kLeft},    {"right", kRight},  {"dpup", kUp},      {"dpdown", kDown},
    {"dpleft", kLeft},  {"dpright", kRight},
};
constexpr int kPadTargets[] = {kA,  kB,  kX,     kY,    kLb, kRb,   kPadLt, kPadRt,
                               kLs, kRs, kStart, kBack, kUp, kDown, kLeft,  kRight};
constexpr int kPadTargetCount = int(sizeof kPadTargets / sizeof kPadTargets[0]);
constexpr uint16_t kPadTargetBits = 0xF3FF;  // every button above (the guide bit 0x0400 passes unchanged)

int ParsePadName(std::string_view text) {
  const std::string n = Lower(Trim(text));
  for (const PadName& p : kPadNames) {
    if (n == p.name) return p.code;
  }
  return -1;
}

std::string PadLabel(int code) {
  for (const PadName& p : kPadNames) {
    if (p.code == code) return p.name;
  }
  return std::format("0x{:x}", code);
}

int PadTargetIndex(int code) {
  for (int i = 0; i < kPadTargetCount; ++i) {
    if (kPadTargets[i] == code) return i;
  }
  return -1;
}

std::vector<int> ParsePadList(std::string_view list, const std::string& target) {
  std::vector<int> out;
  size_t at = 0;
  while (at <= list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string_view::npos) end = list.size();
    const std::string name = Trim(list.substr(at, end - at));
    at = end + 1;
    if (name.empty() || Lower(name) == "none") continue;
    const int code = ParsePadName(name);
    if (code < 0) {
      FCPLOG("input: pad_bind_{}: unknown controller button '{}'", target, name);
    } else {
      out.push_back(code);
    }
  }
  return out;
}

// ---- settings ----
enum class ZoomScale { kFov, kTan, kOff };
enum class Backend { kSdl, kXInput, kOff };

struct Settings {
  bool keyboard = true;
  bool mouse = true;
  bool idle_pad = false;
  double degrees_x = 0.1, degrees_y = 0.1;  // per count
  bool invert_y = false;
  ZoomScale zoom_scale = ZoomScale::kFov;   // while zoomed: fov / 90 degrees (as the game's stick), tan, none
  double zoom_sensitivity = 1.0;            // times this while zoomed
  double accel = 0.0;                       // extra gain per count/ms of mouse speed, 0 = off
  double accel_cap = 2.0;                   // the highest gain (a factor >= 1)
  double smoothing = 0.0;                   // seconds each motion is spread over, 0 = off
  double max_step = 25.0;                   // degrees per frame and axis, the rest in the next frames
  std::array<std::vector<int>, kTargetCount> binds;
  // pads
  Backend backend = Backend::kSdl;
  double dead_zone = 0.0;  // extra radial dead zone (share of full deflection), 0 = only the game's own
  int rumble = 100;        // percent, 0 = off
  std::array<std::vector<int>, kPadTargetCount> pad_binds;
  bool pad_identity = true;  // every button on itself: no remapping
  bool focus_pause = false;  // FCP_PAUSE_ON_FOCUS_LOSS / predator.toml [game] pause_on_focus_loss
};
Settings g_set;

const char* ZoomName(ZoomScale z) { return z == ZoomScale::kTan ? "tan" : z == ZoomScale::kOff ? "off" : "fov"; }
const char* BackendName(Backend b) { return b == Backend::kXInput ? "xinput" : b == Backend::kOff ? "off" : "sdl"; }

bool ParseZoom(const std::string& text, ZoomScale& out) {
  const std::string v = Lower(Trim(text));
  if (v == "fov" || v == "true" || v == "1" || v == "on") return out = ZoomScale::kFov, true;
  if (v == "tan") return out = ZoomScale::kTan, true;
  if (v == "off" || v == "false" || v == "0" || v == "none") return out = ZoomScale::kOff, true;
  return false;
}

bool ParseBackend(const std::string& text, Backend& out) {
  const std::string v = Lower(Trim(text));
  if (v == "sdl" || v == "sdl3") return out = Backend::kSdl, true;
  if (v == "xinput") return out = Backend::kXInput, true;
  if (v == "off" || v == "none") return out = Backend::kOff, true;
  return false;
}

// A small TOML subset: [section], key = "string" | number | true/false, # comments.
struct TomlValue {
  std::string section, key, text;
  bool quoted = false;
};

std::vector<TomlValue> ReadToml(const std::filesystem::path& path) {
  std::vector<TomlValue> out;
  std::ifstream in(path);
  std::string line, section;
  int number = 0;
  while (std::getline(in, line)) {
    ++number;
    // strip a comment outside quotes
    bool q = false;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') q = !q;
      if (line[i] == '#' && !q) {
        line.resize(i);
        break;
      }
    }
    const std::string t = Trim(line);
    if (t.empty()) continue;
    if (t.front() == '[' && t.back() == ']') {
      section = Lower(Trim(std::string_view(t).substr(1, t.size() - 2)));
      continue;
    }
    const size_t eq = t.find('=');
    if (eq == std::string::npos) {
      FCPLOG("input: {}:{}: no '=', line ignored", path.filename().string(), number);
      continue;
    }
    TomlValue v;
    v.section = section;
    v.key = Lower(Trim(std::string_view(t).substr(0, eq)));
    std::string value = Trim(std::string_view(t).substr(eq + 1));
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.size() - 2);
      v.quoted = true;
    }
    v.text = value;
    out.push_back(std::move(v));
  }
  return out;
}

std::string DefaultFile() {
  std::string s =
      "# Far Cry Instincts Predator (PC recompilation): keyboard and mouse.\n"
      "# Written with the defaults when missing; edit and restart. Switches: FCP_KEYBOARD=0, FCP_MOUSE=0.\n"
      "# Keys: a-z, 0-9, f1-f24, num0-num9, space, enter, escape, tab, backspace, shift, ctrl, alt, up, down,\n"
      "# left, right, insert, delete, home, end, pageup, pagedown, minus, equals, comma, period, slash,\n"
      "# semicolon, apostrophe, lbracket, rbracket, backslash, grave, mouse1-mouse5, wheelup, wheeldown,\n"
      "# 0x<virtual key>. Several keys per button separated by commas, \"none\" for no key.\n"
      "\n"
      "[keyboard]\n"
      "enabled = true\n"
      "\n"
      "[mouse]\n"
      "enabled = true\n"
      "# degrees per 10 counts (x and y can differ: sensitivity_x, sensitivity_y)\n"
      "sensitivity = 1.0\n"
      "invert_y = false\n"
      "# While zoomed (scope, binoculars, vehicle view): fov = slower by the field of view / 90 degrees, as the\n"
      "# game scales the stick; tan = by tan(fov / 2) (the same distance on screen near the centre); off = unchanged\n"
      "zoom_scale = \"fov\"           # [FCP_MOUSE_ZOOM_SCALE]\n"
      "# an extra factor while zoomed (0.05-10)\n"
      "zoom_sensitivity = 1.0       # [FCP_MOUSE_ZOOM_SENSITIVITY]\n"
      "# 0 = no acceleration; > 0 = extra gain per count/ms of mouse speed (0.05: 1.5 times at 10 counts/ms),\n"
      "# measured over time, so the same at any frame rate\n"
      "acceleration = 0.0           # [FCP_MOUSE_ACCEL]\n"
      "# the highest gain acceleration reaches (a factor, >= 1)\n"
      "acceleration_cap = 2.0       # [FCP_MOUSE_ACCEL_CAP]\n"
      "# 0 = off; else milliseconds (up to 200) each motion is spread over, by time, not by frames; the total\n"
      "# angle stays exact, the delay is half this on average\n"
      "smoothing = 0                # [FCP_MOUSE_SMOOTHING]\n"
      "\n"
      "[pad]\n"
      "# sdl = every controller through SDL3 (up to four players, hot plug); xinput = XInput pads only;\n"
      "# off = no controllers (keyboard, mouse and test scripts stay). Read at start.\n"
      "backend = \"sdl\"              # [FCP_PAD_BACKEND]\n"
      "# 0 = only the game's own dead zone; 0.05-0.9 = a stick inside this share of full deflection counts as\n"
      "# centred as well (drifting sticks)\n"
      "dead_zone = 0.0              # [FCP_PAD_DEAD_ZONE]\n"
      "# vibration strength in percent, 0 = off\n"
      "rumble = 100                 # [FCP_PAD_RUMBLE]\n"
      "# pad_bind_<button>: the controller buttons for a button of the Xbox pad (several with commas, \"none\" for\n"
      "# none). Buttons: a b x y lb rb lt rt ls rs start back up down left right (D-pad), by their place on an\n"
      "# Xbox pad; PlayStation names work too: cross circle square triangle l1 r1 l2 r2 l3 r3 options select\n"
      "# share create, and south east west north. All players. A button the file gives to another button leaves\n"
      "# the defaults (swap: set both).\n";
  for (int i = 0; i < kPadTargetCount; ++i) {
    s += std::format("pad_bind_{} = \"{}\"\n", PadLabel(kPadTargets[i]), PadLabel(kPadTargets[i]));
  }
  s +=
      "\n"
      "# Pad targets: a b x y lb rb lt rt ls rs start back up down left right (D-pad),\n"
      "# lup ldown lleft lright (left stick), rup rdown rleft rright (right stick).\n"
      "[bindings]\n";
  for (const Target& t : kTargets) {
    s += std::format("{} = \"{}\"\n", t.name, t.keys);
  }
  return s;
}

std::filesystem::path ConfigPath() {
  const std::string over = fcp::env::Get("FCP_INPUT_CONFIG");
  if (!over.empty()) return std::filesystem::path(over);
  return rex::platform::GetExecutableFolder() / "predator_input.toml";
}

bool ParseBool(const std::string& s, bool fallback) {
  const std::string l = Lower(s);
  if (l == "true" || l == "1" || l == "yes" || l == "on") return true;
  if (l == "false" || l == "0" || l == "no" || l == "off") return false;
  return fallback;
}

double ParseNumber(const std::string& s, double fallback) {
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  return end && end != s.c_str() ? v : fallback;
}

// An environment switch wins over the file (as predator.toml); logs what it replaced.
void EnvNumber(const char* name, double& value) {
  const std::string text = fcp::env::Get(name);
  if (text.empty()) return;
  const double v = ParseNumber(text, std::nan(""));
  if (std::isnan(v)) {
    FCPLOG("input: {}={} is no number, ignored", name, text);
    return;
  }
  FCPLOG("input: {}={} wins over the file ({})", name, text, value);
  value = v;
}

// [pad] pad_bind_<button> of the file (or the defaults), with the keyboard's rule for claimed buttons.
void FinishPadBinds(const std::array<bool, kPadTargetCount>& set) {
  std::vector<int> claimed;
  for (int i = 0; i < kPadTargetCount; ++i) {
    if (set[i]) claimed.insert(claimed.end(), g_set.pad_binds[i].begin(), g_set.pad_binds[i].end());
  }
  std::string changed;
  g_set.pad_identity = true;
  for (int i = 0; i < kPadTargetCount; ++i) {
    if (!set[i]) {
      std::erase_if(g_set.pad_binds[i],
                    [&](int c) { return std::find(claimed.begin(), claimed.end(), c) != claimed.end(); });
    }
    if (g_set.pad_binds[i] != std::vector<int>{kPadTargets[i]}) {
      g_set.pad_identity = false;
      changed += std::format(" {}=", PadLabel(kPadTargets[i]));
      for (size_t k = 0; k < g_set.pad_binds[i].size(); ++k) changed += (k ? "+" : "") + PadLabel(g_set.pad_binds[i][k]);
      if (g_set.pad_binds[i].empty()) changed += "none";
    }
  }
  FCPLOG("input: controller bindings{}", changed.empty() ? ": defaults (each button on itself)" : ":" + changed);
}

std::atomic<uint32_t> g_settings_generation{0};  // SettingsGeneration (button prompts redraw after a change)

void LoadSettings() {
  g_settings_generation.fetch_add(1, std::memory_order_relaxed);
  for (int i = 0; i < kTargetCount; ++i) {
    g_set.binds[i] = ParseKeys(kTargets[i].keys, kTargets[i].name);
  }
  for (int i = 0; i < kPadTargetCount; ++i) {
    g_set.pad_binds[i] = {kPadTargets[i]};
  }
  std::array<bool, kPadTargetCount> pad_set{};
  double dead_zone = 0.0, rumble = 100.0, zoom_sens = 1.0, accel = 0.0, accel_cap = 2.0, smoothing_ms = 0.0;
  const std::filesystem::path path = ConfigPath();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::ofstream out(path);
    out << DefaultFile();
    FCPLOG("input: {} missing, written with the defaults{}", path.string(), out ? "" : " (FAILED)");
  } else {
    double sens = 1.0, sx = -1, sy = -1;
    std::string changed;
    std::array<bool, kTargetCount> set{};
    std::vector<int> claimed;  // keys the file binds elsewhere leave the defaults of other targets
    for (const TomlValue& v : ReadToml(path)) {
      if (v.section == "keyboard" && v.key == "enabled") {
        g_set.keyboard = ParseBool(v.text, true);
      } else if (v.section == "mouse" && v.key == "enabled") {
        g_set.mouse = ParseBool(v.text, true);
      } else if (v.section == "mouse" && v.key == "sensitivity") {
        sens = ParseNumber(v.text, 1.0);
      } else if (v.section == "mouse" && v.key == "sensitivity_x") {
        sx = ParseNumber(v.text, -1);
      } else if (v.section == "mouse" && v.key == "sensitivity_y") {
        sy = ParseNumber(v.text, -1);
      } else if (v.section == "mouse" && v.key == "invert_y") {
        g_set.invert_y = ParseBool(v.text, false);
      } else if (v.section == "mouse" && v.key == "zoom_scale") {
        if (!ParseZoom(v.text, g_set.zoom_scale)) FCPLOG("input: [mouse] zoom_scale = {}: fov, tan or off", v.text);
      } else if (v.section == "mouse" && v.key == "zoom_sensitivity") {
        zoom_sens = ParseNumber(v.text, 1.0);
      } else if (v.section == "mouse" && v.key == "acceleration") {
        accel = ParseNumber(v.text, 0.0);
      } else if (v.section == "mouse" && v.key == "acceleration_cap") {
        accel_cap = ParseNumber(v.text, 2.0);
      } else if (v.section == "mouse" && v.key == "smoothing") {
        smoothing_ms = ParseNumber(v.text, 0.0);
      } else if (v.section == "pad" && v.key == "backend") {
        if (!ParseBackend(v.text, g_set.backend)) FCPLOG("input: [pad] backend = {}: sdl, xinput or off", v.text);
      } else if (v.section == "pad" && v.key == "dead_zone") {
        dead_zone = ParseNumber(v.text, 0.0);
      } else if (v.section == "pad" && v.key == "rumble") {
        rumble = ParseNumber(v.text, 100.0);
      } else if (v.section == "pad" && (v.key.starts_with("pad_bind_") || v.key.starts_with("bind_"))) {
        const std::string name = v.key.substr(v.key.find("bind_") + 5);
        const int target = PadTargetIndex(ParsePadName(name));
        if (target < 0) {
          FCPLOG("input: {}: unknown pad button in '{}'", path.filename().string(), v.key);
          continue;
        }
        g_set.pad_binds[target] = ParsePadList(v.text, name);
        pad_set[target] = true;
      } else if (v.section == "bindings") {
        int found = -1;
        for (int i = 0; i < kTargetCount; ++i) {
          if (v.key == kTargets[i].name) found = i;
        }
        if (found < 0) {
          FCPLOG("input: {}: unknown binding target '{}'", path.filename().string(), v.key);
          continue;
        }
        g_set.binds[found] = ParseKeys(v.text, kTargets[found].name);
        set[found] = true;
        claimed.insert(claimed.end(), g_set.binds[found].begin(), g_set.binds[found].end());
      } else {
        FCPLOG("input: {}: unknown key [{}] {}", path.filename().string(), v.section, v.key);
      }
    }
    for (int i = 0; i < kTargetCount; ++i) {
      if (!set[i]) {
        std::erase_if(g_set.binds[i],
                      [&](int vk) { return std::find(claimed.begin(), claimed.end(), vk) != claimed.end(); });
      }
      if (g_set.binds[i] != ParseKeys(kTargets[i].keys, kTargets[i].name)) {
        changed += std::format(" {}=", kTargets[i].name);
        for (size_t k = 0; k < g_set.binds[i].size(); ++k) changed += (k ? "+" : "") + KeyLabel(g_set.binds[i][k]);
        if (g_set.binds[i].empty()) changed += "none";
      }
    }
    sens = std::clamp(sens, 0.01, 100.0);
    g_set.degrees_x = std::clamp(sx > 0 ? sx : sens, 0.01, 100.0) * 0.1;
    g_set.degrees_y = std::clamp(sy > 0 ? sy : sens, 0.01, 100.0) * 0.1;
    FCPLOG("input: {} read; bindings{}", path.string(), changed.empty() ? ": defaults" : ":" + changed);
  }
  if (!fcp::env::Flag("FCP_KEYBOARD", true)) g_set.keyboard = false;
  if (!fcp::env::Flag("FCP_MOUSE", true)) g_set.mouse = false;
  g_set.idle_pad = fcp::env::Flag("FCP_IDLE_PAD");
  // environment switches win over the file
  if (const std::string z = fcp::env::Get("FCP_MOUSE_ZOOM_SCALE"); !z.empty() && !ParseZoom(z, g_set.zoom_scale)) {
    FCPLOG("input: FCP_MOUSE_ZOOM_SCALE={}: fov, tan or off", z);
  }
  if (const std::string b = fcp::env::Get("FCP_PAD_BACKEND"); !b.empty() && !ParseBackend(b, g_set.backend)) {
    FCPLOG("input: FCP_PAD_BACKEND={}: sdl, xinput or off", b);
  }
  EnvNumber("FCP_MOUSE_ZOOM_SENSITIVITY", zoom_sens);
  EnvNumber("FCP_MOUSE_ACCEL", accel);
  EnvNumber("FCP_MOUSE_ACCEL_CAP", accel_cap);
  EnvNumber("FCP_MOUSE_SMOOTHING", smoothing_ms);
  EnvNumber("FCP_PAD_DEAD_ZONE", dead_zone);
  EnvNumber("FCP_PAD_RUMBLE", rumble);
  g_set.zoom_sensitivity = std::clamp(zoom_sens, 0.05, 10.0);
  g_set.accel = std::clamp(accel, 0.0, 10.0);
  g_set.accel_cap = std::clamp(accel_cap, 1.0, 100.0);
  g_set.smoothing = std::clamp(smoothing_ms, 0.0, 200.0) / 1000.0;
  g_set.dead_zone = dead_zone < 0.01 ? 0.0 : std::clamp(dead_zone, 0.0, 0.9);
  g_set.rumble = int(std::lround(std::clamp(rumble, 0.0, 100.0)));
  g_set.focus_pause = fcp::env::Flag("FCP_PAUSE_ON_FOCUS_LOSS");
  FinishPadBinds(pad_set);
}

// ---- FCP_KEY_SCRIPT ----
struct Injected {
  double at = 0, until = 0;
  std::vector<int> keys;
  int axis = -1;  // mouse: 0 x, 1 y
  double counts = 0;
  std::string text;
  bool logged = false;
};
std::vector<Injected> g_script;
bool g_level_clock = false;

double ScriptNow() {
  return g_level_clock ? (fcp::gametime::InLevel() ? fcp::gametime::LevelSeconds() : -1.0)
                       : fcp::gametime::WallSeconds();
}

void ParseScript() {
  const std::string text = fcp::env::Get("FCP_KEY_SCRIPT");
  if (text.empty()) return;
  if (!fcp::devstart::ActiveInGeneration("FCP_KEY_SCRIPT_GENS")) {
    FCPLOG("FCP_KEY_SCRIPT: not applied in relaunch generation {}", fcp::relaunch::Generation());
    return;
  }
  g_level_clock = fcp::env::Get("FCP_INPUT_SCRIPT_CLOCK") == "level";
  const double hold = double(fcp::env::Int("FCP_INPUT_HOLD_MS", 150)) / 1000.0;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = Trim(item);
    if (item.empty()) continue;
    const size_t colon = item.find(':');
    bool ok = colon != std::string::npos;
    Injected e;
    e.text = item;
    if (ok) {
      const std::string times = item.substr(0, colon);
      const size_t dash = times.find('-', 1);
      e.at = std::atof(times.c_str());
      e.until = dash != std::string::npos ? std::atof(times.c_str() + dash + 1) : -1;
      const std::string what = Lower(item.substr(colon + 1));
      if (what.rfind("mx=", 0) == 0 || what.rfind("my=", 0) == 0) {
        e.axis = what[1] == 'y';
        e.counts = std::atof(what.c_str() + 3);
        if (e.until < e.at) e.until = e.at;  // at once
      } else {
        if (e.until <= e.at) e.until = e.at + hold;
        std::stringstream ks(what);
        std::string k;
        while (std::getline(ks, k, '+')) {
          const int vk = ParseKey(k);
          if (vk < 0) ok = false;
          else e.keys.push_back(vk);
        }
      }
    }
    if (!ok) {
      FCPLOG("FCP_KEY_SCRIPT: entry '{}' ignored (<t>[-<t_end>]:<key>[+<key>] | mx=<counts> | my=<counts>)", item);
      continue;
    }
    g_script.push_back(std::move(e));
  }
  FCPLOG("FCP_KEY_SCRIPT: {} entries, clock {}", g_script.size(), g_level_clock ? "level" : "wall");
}

// script mouse counts in (from, to] on the script clock
void ScriptCounts(double from, double to, double& x, double& y) {
  x = y = 0;
  if (to < 0 || to <= from) return;
  for (Injected& e : g_script) {
    if (e.axis < 0) continue;
    double c = 0;
    if (e.until <= e.at) {
      c = e.at > from && e.at <= to ? e.counts : 0;
    } else {
      const double a = std::clamp(from, e.at, e.until), b = std::clamp(to, e.at, e.until);
      c = e.counts * (b - a) / (e.until - e.at);
    }
    if (c != 0 && !e.logged) {
      e.logged = true;
      FCPLOG("key script: '{}' active at {:.3f} s (frame {})", e.text, to, fcp::gametime::Frame());
    }
    (e.axis ? y : x) += c;
  }
}

// ---- state ----
rex::hid::IInputSystem* g_input = nullptr;
bool g_active = false;
std::mutex g_mutex;

// connection of user 0
bool g_real_seen = false;      // a real pad was there before
bool g_pad_lost = false;       // it went away; keyboard/mouse take over only after they are used
std::array<int, kUsers> g_real{-1, -1, -1, -1};  // last known: 1 real pad, 0 none
uint64_t g_not_connected = 0, g_kbm_states = 0, g_merged = 0, g_kbm_changes = 0;

// keyboard overlay serial for the packet number
uint64_t g_last_overlay = 0;
uint32_t g_serial = 0;

// wheel taps (as FCE): a notch is a press of 0.1 s, then a release of 0.06 s
struct Wheel {
  int pending = 0;
  double down_until = -1, free_at = -1;
};
std::array<Wheel, 2> g_wheel;

// mouse look
std::atomic<double> g_last_look{-1.0};   // wall time of the last look input of player 1
bool g_locked = false;
double g_look_script_t = -1;   // script clock at the last look input
double g_carry[2] = {};        // degrees beyond the per-frame step
constexpr double kTwoOverPi = 0.63661977236758134;  // the game's constant (1 / 90 degrees in radians)
uint64_t g_looks = 0, g_looks_mouse = 0, g_looks_other = 0;
int64_t g_trace_look = 0;
bool g_logged_client = false;
// acceleration and smoothing (FCI src/hle/pc_input.cpp, own project code): by time, not by frames
double g_look_t = -1;  // look clock at the last look input
struct Smooth {
  double from, yaw, pitch;  // degrees of the motion of [from, ...], spread over [from, from + smoothing]
};
std::vector<Smooth> g_smooth;
double g_mouse_yaw_total = 0, g_mouse_pitch_total = 0;  // degrees the mouse added (summary)

// pads: remapping, dead zone, backend off
uint64_t g_remapped = 0, g_dead_zoned = 0, g_off_answers = 0;

// rumble: the game's values per user, what went to the pad (strength, pause)
struct Motors {
  uint16_t low = 0, high = 0;
};
std::array<Motors, kUsers> g_wanted{};
uint64_t g_rumble_calls = 0, g_rumble_changed = 0, g_rumble_lines = 0;
int64_t g_trace_rumble = 20;
thread_local uint32_t t_vib_addr = 0;
thread_local Motors t_vib_saved;

// focus pause
std::atomic<bool> g_paused{false};
uint64_t g_pauses = 0;
std::string g_mute_before;  // apu.mute before the pause
struct FocusStep {
  double at, wall_seconds;  // start on the script clock, length in wall seconds
  bool begun = false, ended = false;
  double end_wall = 0;
};
std::vector<FocusStep> g_focus_test;

double Wall() { return fcp::gametime::WallSeconds(); }

// The clock of acceleration and smoothing: the script's level clock in tests (FCP_INPUT_SCRIPT_CLOCK=level, so
// runs at different frame rates see the same counts at the same times), else wall time.
double LookClock() {
  if (g_level_clock && fcp::gametime::InLevel()) return fcp::gametime::LevelSeconds();
  return Wall();
}

uint16_t GetU16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}

// X_INPUT_STATE of a real pad: the controller bindings and the extra dead zone (before the keyboard overlay).
void PadComfort(uint8_t* s) {
  if (!g_set.pad_identity) {
    const uint16_t in = GetU16(s + 4);
    const uint8_t lt = s[6], rt = s[7];
    uint16_t out = uint16_t(in & ~kPadTargetBits);
    uint8_t trig[2] = {0, 0};
    for (int i = 0; i < kPadTargetCount; ++i) {
      for (int src : g_set.pad_binds[i]) {
        const int v = src == kPadLt ? lt : src == kPadRt ? rt : ((in & src) ? 255 : 0);
        const int target = kPadTargets[i];
        if (target == kPadLt || target == kPadRt) {
          uint8_t& t = trig[target == kPadRt];
          t = std::max<uint8_t>(t, uint8_t(v));
        } else if (src == kPadLt || src == kPadRt ? v >= 30 : v != 0) {
          out = uint16_t(out | target);
        }
      }
    }
    if (out != in || trig[0] != lt || trig[1] != rt) ++g_remapped;
    const uint16_t be = __builtin_bswap16(out);
    std::memcpy(s + 4, &be, 2);
    s[6] = trig[0];
    s[7] = trig[1];
  }
  if (g_set.dead_zone > 0) {
    const double zone = g_set.dead_zone * 32767.0;
    for (int stick = 0; stick < 2; ++stick) {
      uint8_t* p = s + 8 + 4 * stick;
      const double x = int16_t(GetU16(p)), y = int16_t(GetU16(p + 2));
      if (x * x + y * y < zone * zone) {
        if (x * x + y * y > 512.0 * 512.0) ++g_dead_zoned;  // counts real deflections, not the resting -1
        std::memset(p, 0, 4);
      }
    }
  }
}

bool HostDown(int code) {
  if (!g_input) return false;
  switch (code) {
    case VK_LBUTTON:
      return g_set.mouse && g_input->IsButtonDown(rex::MouseButton::kLeft);
    case VK_RBUTTON:
      return g_set.mouse && g_input->IsButtonDown(rex::MouseButton::kRight);
    case VK_MBUTTON:
      return g_set.mouse && g_input->IsButtonDown(rex::MouseButton::kMiddle);
    case VK_XBUTTON1:
      return g_set.mouse && g_input->IsButtonDown(rex::MouseButton::kX1);
    case VK_XBUTTON2:
      return g_set.mouse && g_input->IsButtonDown(rex::MouseButton::kX2);
    default:
      return g_set.keyboard && g_input->IsKeyDown(static_cast<rex::VirtualKey>(code));
  }
}

struct Overlay {
  uint16_t buttons = 0;
  uint8_t trigger[2] = {};
  int16_t thumb[4] = {};
  bool thumb_set[4] = {};
  bool any_key = false;  // something bound is held (keyboard/mouse in use)
  uint64_t Hash() const {
    uint64_t h = buttons | uint64_t(trigger[0]) << 16 | uint64_t(trigger[1]) << 24;
    for (int i = 0; i < 4; ++i) h = h * 1000003u ^ uint16_t(thumb[i]) ^ (thumb_set[i] ? 0x10000u : 0u);
    return h;
  }
};

// The keyboard/mouse overlay of this call (caller holds g_mutex).
Overlay Collect() {
  Overlay o;
  const double now = Wall();
  const double script_now = ScriptNow();
  std::array<bool, kMaxCode> held{};
  for (Injected& e : g_script) {
    if (e.axis >= 0 || script_now < e.at || script_now >= e.until) continue;
    for (int k : e.keys) held[size_t(k)] = true;
    if (!e.logged) {
      e.logged = true;
      FCPLOG("key script: '{}' active at {:.3f} s (frame {})", e.text, script_now, fcp::gametime::Frame());
    }
  }
  // wheel: real notches (focus) and none from the script (keys only)
  const int notches = g_input && g_set.mouse ? g_input->TakeWheel() : 0;
  int add[2] = {notches > 0 ? notches : 0, notches < 0 ? -notches : 0};
  for (int d = 0; d < 2; ++d) {
    Wheel& w = g_wheel[size_t(d)];
    w.pending = std::min(w.pending + add[d], 10);
    if (w.pending && now >= w.free_at) {
      --w.pending;
      w.down_until = now + 0.1;
      w.free_at = w.down_until + 0.06;
    }
  }
  auto down = [&](int code) {
    if (code == kWheelUp || code == kWheelDown) return now < g_wheel[code == kWheelDown].down_until;
    return held[size_t(code)] || HostDown(code);
  };
  bool pos[4] = {}, neg[4] = {};
  for (int i = 0; i < kTargetCount; ++i) {
    bool on = false;
    for (int code : g_set.binds[i]) on = on || down(code);
    if (!on) continue;
    o.any_key = true;
    const Target& t = kTargets[i];
    o.buttons |= t.button;
    if (t.trigger >= 0) o.trigger[t.trigger] = 255;
    if (t.axis >= 0) (t.sign > 0 ? pos : neg)[t.axis] = true;
  }
  for (int i = 0; i < 4; ++i) {
    if (pos[i] != neg[i]) {
      o.thumb[i] = pos[i] ? 32767 : -32767;
      o.thumb_set[i] = true;
    }
  }
  return o;
}

void PutU16(uint8_t* p, uint16_t v) {
  const uint16_t be = __builtin_bswap16(v);
  std::memcpy(p, &be, 2);
}

void PutU32(uint8_t* p, uint32_t v) {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(p, &be, 4);
}

float GetF(uint8_t* base, uint32_t a) { return std::bit_cast<float>(fcp::guest::U32(base, a)); }
void SetF(uint8_t* base, uint32_t a, float v) { PutU32(fcp::guest::Ptr(base, a), std::bit_cast<uint32_t>(v)); }

bool KbmOn() { return g_set.keyboard || g_set.mouse; }

// Is a real (non synthetic) pad assigned to the user? Logs changes.
bool RealPad(uint32_t user) {
  if (!g_input || user >= kUsers) return false;
  auto info = g_input->GetPadInfo(user);
  const int now = info ? 1 : 0;
  if (now != g_real[user]) {
    if (info) {
      FCPLOG("pad: user {} has a real pad (type {}, vendor {:04X} product {:04X}) at {:.2f} s", user,
             int(info->type), info->vendor, info->product, Wall());
    } else if (g_real[user] >= 0) {
      FCPLOG("pad: user {} lost its real pad at {:.2f} s", user, Wall());
    } else {
      FCPLOG("pad: user {} has no real pad at {:.2f} s", user, Wall());
    }
    g_real[user] = now;
  }
  return now != 0;
}

// Button prompts (fcp/button_prompts.h, auto): player 1's real pad in use = a newly pressed button, a trigger
// past half or a stick past half its deflection (a resting or drifting stick does not count). The pad's family
// (Xbox, PlayStation 3/4/5, Switch) from the SDK's pad info. Caller holds g_mutex.
uint16_t g_prompt_buttons = 0;
void NotePadUse(const uint8_t* s) {
  rex::hid::PadType type = rex::hid::PadType::kUnknown;
  if (g_input) {
    if (auto info = g_input->GetPadInfo(0)) type = info->type;
  }
  using fcp::prompts::PadFamily;
  const PadFamily family = type == rex::hid::PadType::kPS3   ? PadFamily::kPlayStation3
                           : type == rex::hid::PadType::kPS4 ? PadFamily::kPlayStation4
                           : type == rex::hid::PadType::kPS5 ? PadFamily::kPlayStation5
                           : (type == rex::hid::PadType::kSwitchPro || type == rex::hid::PadType::kSwitchJoyCon)
                               ? PadFamily::kSwitch
                               : PadFamily::kXbox;
  fcp::prompts::NotePadPresent(family);
  const uint16_t buttons = uint16_t(GetU16(s + 4) & ~0x0400u);  // without the guide button
  const uint16_t pressed = uint16_t(buttons & ~g_prompt_buttons);
  g_prompt_buttons = buttons;
  bool used = pressed != 0 || s[6] > 128 || s[7] > 128;
  for (int i = 0; i < 4 && !used; ++i) used = std::abs(int(int16_t(GetU16(s + 8 + 2 * i)))) > 16384;
  if (used) fcp::prompts::NotePad(family);
}

// Pointer lock: while player 1's look input runs and the mouse is on (the SDK frees it without focus).
void UpdatePointer() {
  if (!g_input || !g_set.mouse) return;
  const double last = g_last_look.load(std::memory_order_relaxed);
  // free while the settings menu is open (its pointer), else locked while the look input runs
  const bool want = last >= 0 && Wall() - last < 0.25 && !fcp::menu::BlocksInput();
  if (want != g_locked) {
    g_locked = want;
    g_input->SetPointerMode(want ? rex::PointerMode::kLocked : rex::PointerMode::kFree);
    FCPLOG("input: pointer {} at {:.2f} s", want ? "locked (look input runs)" : "free (no look input)", Wall());
  }
  if (!want) {
    // motion outside the look input is dropped; it still tells the button prompts the mouse is in use
    const rex::PointerDelta d = g_input->TakeMotion();
    if (d.dx != 0 || d.dy != 0) fcp::prompts::NoteMouseMotion(double(d.dx), double(d.dy));
  }
}

// connection decision for user 0: true = answer "not connected"
bool User0Disconnected(bool real, const Overlay& o) {
  if (real) {
    g_real_seen = true;
    if (g_pad_lost) FCPLOG("pad: user 0 real pad back at {:.2f} s", Wall());
    g_pad_lost = false;
    return false;
  }
  if (g_real_seen && !g_pad_lost) {
    g_pad_lost = true;
    FCPLOG("pad: user 0 disconnected at {:.2f} s: not connected until a pad returns or keyboard/mouse is used",
           Wall());
  }
  if (g_set.idle_pad) return false;
  if (!KbmOn()) return true;
  if (g_pad_lost) {
    const bool used = o.any_key || (g_input && g_set.mouse && g_input->IsAnyButtonDown()) ||
                      (g_input && g_set.keyboard && g_input->IsAnyKeyDown());
    if (!used) return true;
    g_pad_lost = false;
    g_real_seen = false;
    FCPLOG("pad: user 0 is keyboard/mouse again at {:.2f} s", Wall());
  }
  return false;
}

// ---- rumble ----
Motors Scaled(const Motors& m) {
  if (g_paused.load(std::memory_order_relaxed) || g_set.backend == Backend::kOff || g_set.rumble <= 0) return {};
  if (g_set.rumble >= 100) return m;
  return {uint16_t(uint32_t(m.low) * uint32_t(g_set.rumble) / 100u),
          uint16_t(uint32_t(m.high) * uint32_t(g_set.rumble) / 100u)};
}

// Sends the motors to every user (pause, resume, exit), outside the title's XamInputSetState.
void SendAllMotors(const char* why) {
  if (!g_input) return;
  int sent = 0;
  for (uint32_t u = 0; u < kUsers; ++u) {
    const Motors m = Scaled(g_wanted[u]);
    if (g_input->SetVibration(u, rex::hid::PadVibration{m.low, m.high})) ++sent;
  }
  const Motors m0 = Scaled(g_wanted[0]);
  FCPLOG("rumble: {}: motors set for {} user(s) with a pad (user 0: game {}/{}, sent {}/{})", why, sent,
         g_wanted[0].low, g_wanted[0].high, m0.low, m0.high);
}

// ---- focus pause ----
// Console behaviour of the guide: XN_SYS_UI "on" makes the title open its own pause menu in a level (as when a
// pad is pulled, measured in eingabe-maus-tastatur.md); the sound is muted (apu.mute) and the motors stop. On the
// return XN_SYS_UI "off", the sound and the motors come back; the game stays in its pause menu until the
// player resumes (as on the console after the guide closes).
constexpr uint32_t kXnSysUi = 0x00000009;

void Broadcast(uint32_t data) {
  rex::kernel::TitleRuntime* runtime = fcp::devstart::Runtime();
  if (!runtime) {
    FCPLOG("focus pause: no title runtime, XN_SYS_UI {} not sent", data);
    return;
  }
  runtime->GetNotificationBroadcaster().BroadcastNotification(kXnSysUi, data);
}

// The guide's XN_SYS_UI is "on" while the focus pause or the settings menu (fcp/settings_menu.h) holds the game.
std::atomic<bool> g_menu_paused{false};
std::mutex g_sys_ui_mutex;
bool g_sys_ui = false;

void UpdateSysUi(const char* why) {
  std::lock_guard lock(g_sys_ui_mutex);
  const bool want = g_paused.load() || g_menu_paused.load();
  if (want == g_sys_ui) return;
  g_sys_ui = want;
  FCPLOG("XN_SYS_UI {} ({})", want ? 1 : 0, why);
  Broadcast(want ? 1 : 0);
}

void SetPaused(bool pause, const char* source) {
  if (!g_set.focus_pause || g_paused.load() == pause) return;
  g_paused.store(pause);
  if (pause) ++g_pauses;
  FCPLOG("focus pause: {} ({}) at {:.2f} s, level {:.2f} s, frame {}", pause ? "window lost the focus, pausing"
                                                                            : "window has the focus again, resuming",
         source, Wall(), fcp::gametime::LevelSeconds(), fcp::gametime::Frame());
  UpdateSysUi("focus pause");
  if (pause) {
    if (auto s = REX_SETTINGS().Find("apu.mute"); s) g_mute_before = (*s)->GetText();
  }
  const std::string mute = pause ? "true" : (g_mute_before.empty() ? "false" : g_mute_before);
  if (auto r = REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "apu.mute", mute); !r) {
    FCPLOG("focus pause: apu.mute FAILED: {}", r.error().GetDescription());
  } else if (auto s = REX_SETTINGS().Find("apu.mute"); s) {
    FCPLOG("focus pause: apu.mute = {} (before the pause {})", (*s)->GetText(), g_mute_before);
  }
  SendAllMotors(pause ? "focus pause" : "focus back");
}

// FCP_TEST_FOCUS=<t>:<seconds>,... (test): a focus loss at t on the script clock for so many wall seconds.
void ParseFocusTest() {
  const std::string text = fcp::env::Get("FCP_TEST_FOCUS");
  if (text.empty()) return;
  std::stringstream ss(text);
  for (std::string item; std::getline(ss, item, ',');) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      FCPLOG("FCP_TEST_FOCUS: '{}' ignored (<t>:<seconds>)", item);
      continue;
    }
    g_focus_test.push_back({std::atof(item.c_str()), std::atof(item.c_str() + colon + 1)});
  }
  FCPLOG("FCP_TEST_FOCUS: {} focus losses (simulated, the same path as the window's focus event){}",
         g_focus_test.size(), g_set.focus_pause ? "" : "; pause_on_focus_loss is off, nothing pauses");
}

void PollFocusTest() {
  if (g_focus_test.empty()) return;
  const double script = ScriptNow(), wall = Wall();
  for (FocusStep& f : g_focus_test) {
    if (!f.begun && script >= 0 && script >= f.at) {
      f.begun = true;
      f.end_wall = wall + f.wall_seconds;
      SetPaused(true, "FCP_TEST_FOCUS");
    } else if (f.begun && !f.ended && wall >= f.end_wall) {
      f.ended = true;
      SetPaused(false, "FCP_TEST_FOCUS");
    }
  }
}

// The UI system updates its own focus state after its listeners ran, so IsFocused() inside the event is the old
// value (measured: inverted). The event only marks the state dirty; the next pad poll of the title (every frame in
// a level, in its pause menu and in menus) reads it.
class FocusListener final : public rex::ui::IUIEventListener {
 public:
  void OnUIEvent(const rex::ui::UIEvent& e) override {
    using T = rex::ui::UIEvent::Type;
    if (e.GetType() == T::kFocus) {
      dirty_.store(true);
    } else if (e.GetType() == T::kMinimized) {
      minimized_.store(true);
      dirty_.store(true);
    } else if (e.GetType() == T::kRestored) {
      minimized_.store(false);
      dirty_.store(true);
    }
  }
  void OnInputEvent(rex::InputEvent&) override {}
  // guest thread, g_mutex held
  void Poll() {
    if (!ui_ || !dirty_.exchange(false)) return;
    const bool focused = ui_->IsFocused(), minimized = minimized_.load();
    if (focused != last_focused_ || minimized != last_minimized_) {
      FCPLOG("input: window {}{} at {:.2f} s", focused ? "has the focus" : "lost the focus",
             minimized ? ", minimized" : "", Wall());
      last_focused_ = focused;
      last_minimized_ = minimized;
    }
    SetPaused(!focused || minimized, minimized ? "window minimized" : "window focus");
  }
  rex::ui::IUISystem* ui_ = nullptr;

 private:
  std::atomic<bool> dirty_{false}, minimized_{false};
  bool last_focused_ = true, last_minimized_ = false;
};
FocusListener g_focus;
rex::ScopeGuard g_focus_guard;

}  // namespace

void Configure(rex::hid::IInputSystem* input) {
  g_input = input;
  LoadSettings();
  ParseScript();
  ParseFocusTest();
  g_trace_look = fcp::env::Int("FCP_TRACE_LOOK", 0);
  g_trace_rumble = fcp::env::Int("FCP_TRACE_RUMBLE", 20);
  g_active = true;  // the connection fix runs even with keyboard and mouse off
  if (fcp::env::Flag("FCP_PAD_BACKGROUND")) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  }
  FCPLOG("input: keyboard {}, mouse {} ({:.3f}/{:.3f} degrees per count{}, zoom scale {} x {:.2f}, acceleration "
         "{:.3f} cap {:.2f}, smoothing {:.0f} ms), idle pad {}, input system {}",
         g_set.keyboard ? "on" : "off (FCP_KEYBOARD=0 or file)", g_set.mouse ? "on" : "off (FCP_MOUSE=0 or file)",
         g_set.degrees_x, g_set.degrees_y, g_set.invert_y ? ", inverted y" : "", ZoomName(g_set.zoom_scale),
         g_set.zoom_sensitivity, g_set.accel, g_set.accel_cap, g_set.smoothing * 1000,
         g_set.idle_pad ? "on (FCP_IDLE_PAD=1: user 0 always connected)" : "off (no pad: not connected)",
         input ? "attached" : "MISSING");
  FCPLOG("pad: backend {}{}, extra dead zone {:.2f}{}, rumble {}%, pause on focus loss {}", BackendName(g_set.backend),
         g_set.backend == Backend::kOff ? " (controllers hidden from the title)" : "", g_set.dead_zone,
         g_set.dead_zone > 0 ? "" : " (only the game's)", g_set.rumble,
         g_set.focus_pause ? "on" : "off (FCP_PAUSE_ON_FOCUS_LOSS)");
  // Diagnosis for real pads: what XInput and SDL see at launch.
  std::string xinput = "xinput1_4 missing";
  if (HMODULE x = LoadLibraryA("xinput1_4.dll")) {
    using GetState = DWORD(WINAPI*)(DWORD, void*);
    if (auto get = reinterpret_cast<GetState>(GetProcAddress(x, "XInputGetState"))) {
      xinput.clear();
      for (DWORD i = 0; i < 4; ++i) {
        uint8_t state[16] = {};
        const DWORD r = get(i, state);
        xinput += std::format("{}slot {} {}", i ? ", " : "", i, r == 0 ? "connected" : std::format("{}", r));
      }
    }
  }
  int count = 0;
  std::string names;
  if (SDL_WasInit(SDL_INIT_GAMEPAD)) {
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&count)) {
      for (int i = 0; i < count; ++i) {
        const char* n = SDL_GetJoystickNameForID(ids[i]);
        names += std::format(" [{} \"{}\" {:04X}:{:04X} gamepad {}]", ids[i], n ? n : "?",
                             SDL_GetJoystickVendorForID(ids[i]), SDL_GetJoystickProductForID(ids[i]),
                             SDL_IsGamepad(ids[i]));
      }
      SDL_free(ids);
    }
  }
  const char* bg = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
  FCPLOG("pad: at launch XInput {}; SDL gamepad subsystem {}, {} joystick(s){}; background events {}", xinput,
         SDL_WasInit(SDL_INIT_GAMEPAD) ? "up" : "not up", count, names, bg ? bg : "default (off)");
}

bool Active() { return g_active; }

void OnGetState(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t state) {
  if (!g_active || user >= kUsers) return;
  std::lock_guard lock(g_mutex);
  if (user == 0) {
    PollFocusTest();
    g_focus.Poll();
  }
  const bool off = g_set.backend == Backend::kOff;
  const bool real = !off && RealPad(user);
  const bool sdk_ok = ctx.r3.u32 == kErrorSuccess;
  const bool readable = sdk_ok && fcp::guest::Readable(base, state, 16);
  // Settings menu (fcp/settings_menu.h): while it is open the title sees idle pads and no keyboard/mouse; Back and
  // Start of the menu's own chord do not reach it.
  const bool blocked = fcp::menu::BlocksInput();
  if (readable) {
    uint8_t* s = fcp::guest::Ptr(base, state);
    if (blocked) {
      std::memset(s + 4, 0, 12);
    } else if (const uint16_t strip = off ? 0 : fcp::menu::PadButtonsToHide(GetU16(s + 4)); strip) {
      PutU16(s + 4, uint16_t(GetU16(s + 4) & ~strip));
    }
  }
  if (blocked) {
    if (user == 0) {
      UpdatePointer();  // frees the pointer, drops the motion
      if (g_input) (void)g_input->TakeWheel();
      if (sdk_ok && User0Disconnected(real, Overlay{})) {
        ++g_not_connected;
        ctx.r3.u64 = kErrorDeviceNotConnected;
      }
    } else if (off && sdk_ok) {
      ctx.r3.u64 = kErrorDeviceNotConnected;
    }
    return;
  }
  if (off) {
    // pad_backend = off: the SDK's pads are hidden; user 0 keeps keyboard and mouse
    if (user != 0) {
      if (sdk_ok) {
        ++g_off_answers;
        ctx.r3.u64 = kErrorDeviceNotConnected;
      }
      return;
    }
    if (readable) std::memset(fcp::guest::Ptr(base, state) + 4, 0, 12);
  } else if (real && readable && (!g_set.pad_identity || g_set.dead_zone > 0)) {
    PadComfort(fcp::guest::Ptr(base, state));
  }
  if (user != 0) return;  // keyboard and mouse are player 1's; the SDK answers the others
  if (real && readable) NotePadUse(fcp::guest::Ptr(base, state));  // button prompts (auto): the pad in use
  UpdatePointer();
  const Overlay o = KbmOn() ? Collect() : Overlay{};
  if (o.any_key || (g_input && ((g_set.keyboard && g_input->IsAnyKeyDown()) ||
                                (g_set.mouse && g_input->IsAnyButtonDown())))) {
    fcp::prompts::NoteKeyboard();
  }
  if (!sdk_ok) return;
  if (User0Disconnected(real, o)) {
    ++g_not_connected;
    ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  if (!KbmOn() || !readable) return;
  const uint64_t h = o.Hash();
  if (h != g_last_overlay) {
    g_last_overlay = h;
    ++g_serial;
    ++g_kbm_changes;
  }
  // X_INPUT_STATE (big endian): u32 packet, u16 buttons, u8 lt, u8 rt, s16 lx, ly, rx, ry.
  uint8_t* s = fcp::guest::Ptr(base, state);
  if (!real) {
    ++g_kbm_states;
  } else {
    ++g_merged;
  }
  PutU32(s, fcp::guest::U32(base, state) + g_serial);
  PutU16(s + 4, uint16_t(fcp::guest::U16(base, state + 4) | o.buttons));
  s[6] = std::max(s[6], o.trigger[0]);
  s[7] = std::max(s[7], o.trigger[1]);
  for (int i = 0; i < 4; ++i) {
    if (o.thumb_set[i]) PutU16(s + 8 + 2 * i, uint16_t(o.thumb[i]));
  }
}

void OnGetCapabilities(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t caps) {
  (void)base;
  (void)caps;
  if (!g_active || user >= kUsers || ctx.r3.u32 != kErrorSuccess) return;
  const bool off = g_set.backend == Backend::kOff;
  if (user != 0) {
    if (off) ctx.r3.u64 = kErrorDeviceNotConnected;
    return;
  }
  std::lock_guard lock(g_mutex);
  const bool real = !off && RealPad(0);
  if (User0Disconnected(real, Overlay{})) {
    ctx.r3.u64 = kErrorDeviceNotConnected;
  }
}

void BeforeSetState(PPCContext& ctx, uint8_t* base, uint32_t user, uint32_t vibration) {
  (void)ctx;
  t_vib_addr = 0;
  if (!g_active || user >= kUsers || !fcp::guest::Readable(base, vibration, 4)) return;
  uint8_t* p = fcp::guest::Ptr(base, vibration);
  const Motors game{GetU16(p), GetU16(p + 2)};
  std::lock_guard lock(g_mutex);
  ++g_rumble_calls;
  const bool changed = game.low != g_wanted[user].low || game.high != g_wanted[user].high;
  g_wanted[user] = game;
  const Motors sent = Scaled(game);
  if (changed) {
    ++g_rumble_changed;
    if (int64_t(g_rumble_lines) < g_trace_rumble) {
      ++g_rumble_lines;
      FCPLOG("rumble: user {} game {}/{} -> sent {}/{} ({}%{}) at {:.2f} s", user, game.low, game.high, sent.low,
             sent.high, g_set.rumble, g_paused.load() ? ", focus pause" : "", Wall());
    }
  }
  if (sent.low == game.low && sent.high == game.high) return;
  // the SDK reads the title's structure during the call: scaled for the call, the title's values back after it
  t_vib_addr = vibration;
  t_vib_saved = game;
  PutU16(p, sent.low);
  PutU16(p + 2, sent.high);
}

void AfterSetState(uint8_t* base) {
  if (!t_vib_addr) return;
  uint8_t* p = fcp::guest::Ptr(base, t_vib_addr);
  PutU16(p, t_vib_saved.low);
  PutU16(p + 2, t_vib_saved.high);
  t_vib_addr = 0;
}

void AttachUI(rex::ui::IUISystem& ui) {
  g_focus.ui_ = &ui;
  g_focus_guard = ui.AddEventListener(g_focus);
}

// ---- settings menu (fcp/settings_menu.h) ----

Live GetLive() {
  std::lock_guard lock(g_mutex);
  Live l;
  l.keyboard = g_set.keyboard;
  l.mouse = g_set.mouse;
  l.sensitivity_x = g_set.degrees_x * 10.0;
  l.sensitivity_y = g_set.degrees_y * 10.0;
  l.invert_y = g_set.invert_y;
  l.zoom_scale = ZoomName(g_set.zoom_scale);
  l.zoom_sensitivity = g_set.zoom_sensitivity;
  l.acceleration = g_set.accel;
  l.acceleration_cap = g_set.accel_cap;
  l.smoothing_ms = g_set.smoothing * 1000.0;
  l.dead_zone = g_set.dead_zone;
  l.rumble = g_set.rumble;
  l.backend = BackendName(g_set.backend);
  return l;
}

std::filesystem::path ConfigFile() { return ConfigPath(); }

std::string EnvironmentOverride(std::string_view section, std::string_view key) {
  static constexpr struct {
    const char *section, *key, *env;
  } kEnv[] = {{"mouse", "zoom_scale", "FCP_MOUSE_ZOOM_SCALE"},  {"mouse", "zoom_sensitivity", "FCP_MOUSE_ZOOM_SENSITIVITY"},
              {"mouse", "acceleration", "FCP_MOUSE_ACCEL"},     {"mouse", "acceleration_cap", "FCP_MOUSE_ACCEL_CAP"},
              {"mouse", "smoothing", "FCP_MOUSE_SMOOTHING"},    {"pad", "dead_zone", "FCP_PAD_DEAD_ZONE"},
              {"pad", "rumble", "FCP_PAD_RUMBLE"},              {"pad", "backend", "FCP_PAD_BACKEND"},
              {"keyboard", "enabled", "FCP_KEYBOARD"},          {"mouse", "enabled", "FCP_MOUSE"}};
  for (const auto& e : kEnv) {
    if (section == e.section && key == e.key) {
      const std::string v = fcp::env::Get(e.env);
      return v.empty() ? std::string() : std::format("{}={}", e.env, v);
    }
  }
  return {};
}

std::string Store(std::string_view section, std::string_view key, const std::string& literal) {
  const std::filesystem::path path = ConfigPath();
  const bool ok = fcp::toml::SetValue(path, section, key, literal);
  {
    std::lock_guard lock(g_mutex);
    LoadSettings();  // the whole file again, as at the title launch: every input setting holds at once
  }
  FCPLOG("input: [{}] {} = {} ({} {}); settings read again", section, key, literal, ok ? "stored in" : "storing FAILED in",
         path.filename().string());
  return ok ? std::string() : "storing in " + path.string() + " failed";
}

int BindingCount() { return kTargetCount; }

std::string BindingName(int i) { return i >= 0 && i < kTargetCount ? kTargets[i].name : ""; }

std::string BindingKeys(int i) {
  std::lock_guard lock(g_mutex);
  if (i < 0 || i >= kTargetCount) return {};
  std::string s;
  for (int code : g_set.binds[size_t(i)]) s += (s.empty() ? "" : ", ") + KeyLabel(code);
  return s.empty() ? "none" : s;
}

std::string DefaultBindingKeys(int i) { return i >= 0 && i < kTargetCount ? kTargets[i].keys : ""; }

std::string KeyName(int code) {
  if (code <= 0 || code >= kMaxCode) return {};
  return KeyLabel(code);
}

std::string WheelName(bool down) { return KeyLabel(down ? kWheelDown : kWheelUp); }

std::vector<int> BindingCodes(int i) {
  std::lock_guard lock(g_mutex);
  if (i < 0 || i >= kTargetCount) return {};
  return g_set.binds[size_t(i)];
}

int WheelCode(bool down) { return down ? kWheelDown : kWheelUp; }

std::string PadSourceName(std::string_view name) {
  static constexpr const char* kNames[] = {"a",  "b",  "x",     "y",    "lb", "rb",   "lt",   "rt",
                                           "ls", "rs", "start", "back", "up", "down", "left", "right"};
  static_assert(std::size(kNames) == size_t(kPadTargetCount));
  std::lock_guard lock(g_mutex);
  for (int i = 0; i < kPadTargetCount; ++i) {
    if (name != kNames[i]) continue;
    const std::vector<int>& src = g_set.pad_binds[size_t(i)];
    if (src.empty()) return {};
    for (int k = 0; k < kPadTargetCount; ++k)
      if (kPadTargets[k] == src[0]) return kNames[k];
    return {};
  }
  return {};
}

uint32_t SettingsGeneration() { return g_settings_generation.load(std::memory_order_relaxed); }

void SetMenuPause(bool open) {
  if (g_menu_paused.exchange(open) == open) return;
  UpdateSysUi(open ? "settings menu open" : "settings menu closed");
}

void Reload() {
  std::lock_guard lock(g_mutex);
  LoadSettings();
}

rex::hid::IInputSystem* Input() { return g_input; }

void DetachUI() {
  g_focus_guard.Reset();
  g_focus.ui_ = nullptr;
}

void ApplyStartupSettings(rex::SettingsRegistry& settings) {
  // Only [pad] backend of the bindings file (the rest is read at title launch); FCP_PAD_BACKEND wins.
  Backend backend = Backend::kSdl;
  std::string from = "default";
  const std::filesystem::path path = ConfigPath();
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    for (const TomlValue& v : ReadToml(path)) {
      if (v.section == "pad" && v.key == "backend" && ParseBackend(v.text, backend)) from = path.filename().string();
    }
  }
  if (const std::string b = fcp::env::Get("FCP_PAD_BACKEND"); !b.empty() && ParseBackend(b, backend)) {
    from = "FCP_PAD_BACKEND";
  }
  // off keeps the SDL driver (the SDK needs an input driver); the title does not see its pads (OnGetState).
  const char* drivers = backend == Backend::kXInput ? "xinput" : "sdl3";
  if (auto r = settings.SetText(rex::SettingLayer::kFile, "hid.drivers", drivers); !r) {
    FCPLOG("pad: hid.drivers = {} FAILED: {}", drivers, r.error().GetDescription());
    return;
  }
  FCPLOG("setting hid.drivers = {} (file layer, pad backend {} from {})", drivers, BackendName(backend), from);
}

// After the original look input (r3 aim, r4 command at entry).
void OnLook(PPCContext& ctx, uint8_t* base, uint32_t aim, uint32_t cmd, double stick_lr, double stick_ud) {
  if (!g_active || !fcp::guest::Readable(base, cmd, kCmdPitch + 4)) return;
  const uint32_t client = cmd - kClientCommand;
  const uint32_t game = fcp::guest::U32(base, fcp::addr::game_ptr);
  uint32_t first = 0, locals = 0;
  if (fcp::guest::Readable(base, game, kGameLocalCount + 4)) {
    first = fcp::guest::U32(base, game + kGameFirstLocal);
    locals = fcp::guest::U32(base, game + kGameLocalCount);
  }
  if (!g_logged_client) {
    g_logged_client = true;
    FCPLOG("look: first look input: client {:08X} command {:08X} aim {:08X}; game {:08X} first local {:08X}, "
           "{} local(s)",
           client, cmd, aim, game, first, locals);
  }
  if (first && client != first) {
    ++g_looks_other;  // split screen: players 2-4 keep their pads
    return;
  }
  ++g_looks;
  const double wall = Wall();
  g_last_look.store(wall, std::memory_order_relaxed);
  if (!g_set.mouse) return;

  std::lock_guard lock(g_mutex);
  double mx = 0, my = 0;
  if (g_locked && g_input) {
    const rex::PointerDelta d = g_input->TakeMotion();
    mx = d.dx, my = d.dy;
  }
  const double script_now = ScriptNow();
  double sx = 0, sy = 0;
  if (g_look_script_t >= 0) ScriptCounts(g_look_script_t, script_now, sx, sy);
  g_look_script_t = script_now;
  mx += sx, my += sy;
  if (mx != 0 || my != 0) fcp::prompts::NoteMouseMotion(mx, my);  // button prompts follow the device (auto)

  // the time since the last look input (acceleration, smoothing); a gap (menu, pause) starts afresh
  const double look_now = LookClock();
  const bool gap = g_look_t < 0 || look_now < g_look_t || look_now - g_look_t > 0.25;
  const double dt = gap ? 0.0 : look_now - g_look_t;
  const double from = gap ? look_now : g_look_t;
  g_look_t = look_now;
  if (gap) g_smooth.clear();

  // zoom: fov / 90 degrees as the game scales the stick (fov), tan(fov / 2) (tan) or none; a wider view than
  // 90 degrees never speeds the mouse up
  double zoom = 1.0, fov = 0.0;
  const bool want_fov = g_set.zoom_scale != ZoomScale::kOff || g_set.zoom_sensitivity != 1.0;
  if (want_fov && fcp::guest::Readable(base, aim, kAimEntityId + 4)) {
    const uint32_t system = fcp::guest::U32(base, fcp::addr::entity_system_ptr);
    const uint32_t id = fcp::guest::U32(base, aim + kAimEntityId);
    if (fcp::guest::Readable(base, system, 4)) {
      const uint32_t entity =
          fcp::guest::Call(ctx, base, __imp__EntitySystem_GetEntity, {.r3 = system, .r4 = id});
      if (fcp::guest::Readable(base, entity, kEntityCamera + 4)) {
        const uint32_t camera = fcp::guest::U32(base, entity + kEntityCamera);
        if (fcp::guest::Readable(base, camera, kCameraFov + 4)) {
          fov = GetF(base, camera + kCameraFov);
          constexpr double kNormal = 1.5707963267948966;  // 90 degrees: the game's reference (fov * 2/pi)
          if (fov > 0.05 && fov < kNormal - 1e-3) {
            const double f = g_set.zoom_scale == ZoomScale::kFov   ? fov * kTwoOverPi
                             : g_set.zoom_scale == ZoomScale::kTan ? std::tan(fov / 2) / std::tan(kNormal / 2)
                                                                   : 1.0;
            zoom = std::clamp(f * g_set.zoom_sensitivity, 0.01, 10.0);
          }
        }
      }
    }
  }
  // acceleration: the gain for this much motion in this time (counts per ms, the same at any frame rate)
  double gain = 1.0;
  if (g_set.accel > 0 && dt > 0) {
    gain = std::min(1.0 + g_set.accel * std::hypot(mx, my) / (dt * 1000.0), g_set.accel_cap);
  }
  double add[2] = {mx * g_set.degrees_x * zoom * gain,
                   my * g_set.degrees_y * zoom * gain * (g_set.invert_y ? -1 : 1)};
  if (mx != 0 || my != 0) ++g_looks_mouse;
  if (g_set.smoothing > 0) {
    // the motion of [from, now] spread over [from, from + smoothing]: what falls before now goes this frame
    if (add[0] != 0 || add[1] != 0) g_smooth.push_back({from, add[0], add[1]});
    add[0] = add[1] = 0;
    for (const Smooth& s : g_smooth) {
      const double share = (std::min(look_now, s.from + g_set.smoothing) - std::max(from, s.from)) / g_set.smoothing;
      if (share > 0) add[0] += s.yaw * share, add[1] += s.pitch * share;
    }
    std::erase_if(g_smooth, [&](const Smooth& s) { return s.from + g_set.smoothing <= look_now; });
  }
  // at most max_step degrees a frame and axis (vehicle arcs take the angle modulo 360 before they clamp:
  // FCE measured), the rest in the next frames; never more than half a turn waiting
  for (int i = 0; i < 2; ++i) {
    add[i] += g_carry[i];
    const double step = std::clamp(add[i], -g_set.max_step, g_set.max_step);
    g_carry[i] = std::clamp(add[i] - step, -180.0, 180.0);
    add[i] = step;
  }
  g_mouse_yaw_total += add[0];
  g_mouse_pitch_total += add[1];
  const float yaw0 = GetF(base, cmd + kCmdYaw), pitch0 = GetF(base, cmd + kCmdPitch);
  if (add[0] != 0) SetF(base, cmd + kCmdYaw, float(yaw0 + add[0]));
  if (add[1] != 0) SetF(base, cmd + kCmdPitch, float(pitch0 + add[1]));
  const bool moved = mx != 0 || my != 0 || add[0] != 0 || add[1] != 0;
  if (g_trace_look > 0 && (int64_t(g_looks) <= g_trace_look || g_looks % 300 == 0 || moved)) {
    if (int64_t(g_looks) <= g_trace_look || g_looks % 300 == 0 || g_looks_mouse <= uint64_t(g_trace_look) ||
        g_set.smoothing > 0 || g_set.accel > 0) {
      FCPLOG("look #{} {:.3f} s frame {} dt {:.2f} ms: stick {:+.3f} {:+.3f} -> game yaw {:+.4f} pitch {:+.4f}; mouse "
             "{:+.1f} {:+.1f} counts (script {:+.1f} {:+.1f}), gain {:.4f}, fov {:.4f} zoom {:.4f} -> {:+.4f} {:+.4f} "
             "deg (carry {:.2f} {:.2f}, total {:+.4f} {:+.4f})",
             g_looks, script_now, fcp::gametime::Frame(), dt * 1000, stick_lr, stick_ud, yaw0, pitch0, mx, my, sx, sy,
             gain, fov, zoom, add[0], add[1], g_carry[0], g_carry[1], g_mouse_yaw_total, g_mouse_pitch_total);
    }
  }
}

void WriteSummary() {
  // The motors stop at the end (SDL closes the pads later; this does not wait for it).
  {
    std::lock_guard lock(g_mutex);
    g_wanted = {};
  }
  SendAllMotors("shutdown");
  FCPLOG("input summary: look inputs {} (player 1), {} with mouse motion, {} of other players; mouse added "
         "{:+.4f} yaw {:+.4f} pitch degrees; user 0: {} answers 'not connected', {} keyboard/mouse states, {} merged "
         "with a pad, {} overlay changes; pads: {} remapped states, {} inside the extra dead zone, {} answers "
         "'not connected' (backend off); rumble: {} XamInputSetState, {} changes, strength {}%; focus pauses {}",
         g_looks, g_looks_mouse, g_looks_other, g_mouse_yaw_total, g_mouse_pitch_total, g_not_connected, g_kbm_states,
         g_merged, g_kbm_changes, g_remapped, g_dead_zoned, g_off_answers, g_rumble_calls, g_rumble_changed,
         g_set.rumble, g_pauses);
}

}  // namespace fcp::pc_input

// The game's look input, hooked: original first, then the mouse (see the file header).
REX_EXTERN(__imp__Player_LookInput);
REX_HOOK_RAW(Player_LookInput) {
  const uint32_t aim = ctx.r3.u32, cmd = ctx.r4.u32;
  const double lr = ctx.f1.f64, ud = ctx.f2.f64;
  __imp__Player_LookInput(ctx, base);
  ::fcp::pc_input::OnLook(ctx, base, aim, cmd, lr, ud);
}
