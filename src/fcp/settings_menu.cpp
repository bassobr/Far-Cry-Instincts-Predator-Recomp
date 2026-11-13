// The settings menu and the frame rate display (fcp/settings_menu.h). Own code of this project; the menu's idea,
// its sections, "apply at once or mark restart required" and Back + Start come from FCI's F10 menu
// (Far Cry Instincts Recomp, src/app/menu.cpp, MIT, same author), rewritten for the SDK's ImGui host.
//
// Threads: Draw, the hotkey listener and the test script run on the SDK's UI thread (Draw once per presented
// picture, inside UISystem::DrawOverlays); the game's pad poll (pc_input) asks BlocksInput / PadButtonsToHide.
#include "fcp/settings_menu.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/hid/input_system.h>
#include <rex/hid/pad.h>
#include <rex/input_keys.h>
#include <rex/platform/file.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "fcp/button_prompts.h"
#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/gametime.h"
#include "fcp/log.h"
#include "fcp/pc_input.h"
#include "fcp/render_bridge.h"
#include "fcp/user_settings.h"
#include "fcp_version.h"

namespace fcp::menu {
namespace {

using Clock = std::chrono::steady_clock;

// ---- state shared with the game's threads ----
std::atomic<bool> g_open{false};
std::atomic<bool> g_hide_chord{false};  // Back/Start stay hidden from the game until both are let go
std::atomic<uint16_t> g_script_pad{0};  // FCP_TEST_MENU pad=: buttons held

// ---- UI thread ----
rex::ui::IUISystem* g_ui = nullptr;
bool g_enabled = true;
bool g_pause = true;
ImFont* g_font = nullptr;
std::string g_font_name = "built in";
int g_select_tab = -1;     // set by the test script: that tab once
int g_capture = -1;        // binding target waiting for a key, -1 none
bool g_pad_armed = false;  // the pad counts once every button was let go after opening
uint16_t g_pad_prev = 0;
int g_saved_config_flags = 0;
bool g_saved_nav_capture = true;
std::string g_status;  // the last change, under the table
double g_status_until = 0;

double Now() { return fcp::gametime::WallSeconds(); }

void Status(std::string text) {
  FCPLOG("settings menu: {}", text);
  g_status = std::move(text);
  g_status_until = Now() + 6.0;
}

// ---- rows ----
enum class When { kLive, kRestart };
enum class Kind { kChoice, kBool, kSlider };

struct Choice {
  std::string value, label;
};

struct Row {
  int tab;  // 0 display, 1 graphics, 2 input, 3 game
  const char* label;
  Kind kind;
  When when;
  std::vector<Choice> choices;  // kChoice
  float min = 0, max = 1, step = 0.01f;
  const char* format = "%.2f";
  const char* help;
  std::function<std::string()> get;                    // text as the file holds it
  std::function<std::string(const std::string&)> set;  // store and apply; error text or ""
  std::function<std::string()> override_note;          // "FCP_X=... wins" or ""
  std::string start;                                   // value at the title launch (restart required?)
  std::string id;                                      // "section.key" / "input:section.key" (test script)
};
std::vector<Row> g_rows;

const char* const kTabs[] = {"Display", "Graphics", "Input", "Game", "About"};
constexpr int kTabCount = 5;

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(std::string_view s) {
  const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}

bool SetRuntime(std::string_view name, const std::string& value) {
  if (auto r = REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, name, value); !r) {
    FCPLOG("settings menu: {} = {} FAILED: {}", name, value, r.error().GetDescription());
    return false;
  }
  FCPLOG("settings menu: {} = {} (runtime layer)", name, value);
  return true;
}

// a predator.toml row: store, then `apply` for the live part
Row SettingsRow(int tab, const char* label, Kind kind, When when, const char* section, const char* key,
                const char* help, std::function<void(const std::string&)> apply = nullptr) {
  Row r{tab, label, kind, when, {}, 0, 1, 0.01f, "%.2f", help};
  const std::string s = section, k = key;
  r.get = [s, k] { return fcp::settings::Value(s, k); };
  r.set = [s, k, apply](const std::string& v) {
    std::string e = fcp::settings::Store(s, k, v);
    if (e.empty() && apply && fcp::settings::EnvironmentOverride(s, k).empty()) apply(v);
    return e;
  };
  r.override_note = [s, k] { return fcp::settings::EnvironmentOverride(s, k); };
  r.id = s + "." + k;
  return r;
}

// a predator_input.toml row: `get` from the settings in force, `literal` turns the menu value into the file's text
Row InputRow(int tab, const char* label, Kind kind, When when, const char* section, const char* key, const char* help,
             std::function<std::string()> get, bool quoted = false) {
  Row r{tab, label, kind, when, {}, 0, 1, 0.01f, "%.2f", help};
  const std::string s = section, k = key;
  r.get = std::move(get);
  r.set = [s, k, quoted](const std::string& v) { return fcp::pc_input::Store(s, k, quoted ? "\"" + v + "\"" : v); };
  r.override_note = [s, k] { return fcp::pc_input::EnvironmentOverride(s, k); };
  r.id = "input:" + s + "." + k;
  return r;
}

std::string Num(double v, int digits = 2) {
  std::string s = std::format("{:.{}f}", v, digits);
  if (s.find('.') != std::string::npos) {
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
  }
  return s;
}

std::string BoolText(bool b) { return b ? "true" : "false"; }

void BuildRows() {
  using fcp::pc_input::GetLive;
  g_rows.clear();
  // ---- Display ----
  {
    Row r = SettingsRow(0, "Display mode", Kind::kChoice, When::kLive, "display", "mode",
                        "Window, borderless fullscreen, or exclusive fullscreen (video mode). Alt+Enter switches too.",
                        [](const std::string& v) {
                          SetRuntime("ui.fullscreen_exclusive", v == "exclusive" ? "true" : "false");
                          SetRuntime("ui.fullscreen", v == "windowed" ? "false" : "true");
                        });
    r.choices = {{"windowed", "Window"}, {"borderless", "Borderless fullscreen"}, {"exclusive", "Exclusive fullscreen"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "Monitor", Kind::kChoice, When::kRestart, "display", "monitor",
                        "The display the game opens on.");
    r.choices = {{"0", "System default"}};
    if (g_ui) {
      const auto displays = g_ui->GetDisplays();
      for (size_t i = 0; i < displays.size(); ++i)
        r.choices.push_back({std::to_string(i + 1), std::format("{}: {} ({}x{})", i + 1, displays[i].name,
                                                                displays[i].width, displays[i].height)});
    }
    g_rows.push_back(std::move(r));
  }
  {
    Row r{0, "Window size", Kind::kChoice, When::kLive, {}, 0, 1, 0.01f, "%.2f",
          "Client size of the window (not fullscreen). The game itself renders at 1280x720 times the render scale."};
    r.choices = {{"0x0", "Default (1280x720)"}, {"1280x720", "1280 x 720"},   {"1600x900", "1600 x 900"},
                 {"1920x1080", "1920 x 1080"},  {"2560x1440", "2560 x 1440"}, {"3840x2160", "3840 x 2160"}};
    r.get = [] {
      return fcp::settings::Value("display", "width") + "x" + fcp::settings::Value("display", "height");
    };
    r.set = [](const std::string& v) {
      const size_t x = v.find('x');
      const std::string w = v.substr(0, x), h = v.substr(x + 1);
      std::string e = fcp::settings::Store("display", "width", w);
      if (e.empty()) e = fcp::settings::Store("display", "height", h);
      if (e.empty() && fcp::settings::EnvironmentOverride("display", "width").empty()) {
        SetRuntime("ui.window_width", w == "0" ? "1280" : w);
        SetRuntime("ui.window_height", h == "0" ? "720" : h);
      }
      return e;
    };
    r.override_note = [] { return fcp::settings::EnvironmentOverride("display", "width"); };
    r.id = "display.size";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "Frame rate", Kind::kChoice, When::kRestart, "display", "fps",
                        "30 as the console, or 60 / 90 / 120 frames per second (90: Steam Deck OLED display).");
    r.choices = {{"30", "30 (console)"}, {"60", "60"}, {"90", "90"}, {"120", "120"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "VSync", Kind::kChoice, When::kRestart, "display", "vsync",
                        "Wait for the monitor when presenting. Auto: on up to 60 frames per second, off above.");
    r.choices = {{"auto", "Auto"}, {"on", "On"}, {"off", "Off"}};
    g_rows.push_back(std::move(r));
  }
  // ---- Graphics ----
  {
    Row r = SettingsRow(1, "Graphics API", Kind::kChoice, When::kRestart, "graphics", "backend",
                        "Vulkan or Direct3D 12. Auto: renderer.toml next to the program, else Vulkan.");
    r.choices = {{"auto", "Auto"}, {"vulkan", "Vulkan"}, {"d3d12", "Direct3D 12"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Render scale", Kind::kChoice, When::kLive, "graphics", "scale",
                        "Resolution of the screen-sized targets as a multiple of the console's 1280x720. Auto: the "
                        "window's height. Switches with the next frame.",
                        [](const std::string& v) { fcp::render::SetScale(v); });
    r.choices = {{"0.5", "0.5x"}, {"0.75", "0.75x"}, {"1", "1x (console)"}, {"1.5", "1.5x"},
                 {"2", "2x"},     {"3", "3x"},       {"4", "4x"},           {"auto", "Auto (window height)"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Shadow resolution", Kind::kChoice, When::kRestart, "graphics", "shadow_scale",
                        "Shadow map resolution as a multiple of the console's 1344x1344.");
    r.choices = {{"1", "1x (console)"}, {"1.5", "1.5x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Anisotropic filtering", Kind::kChoice, When::kRestart, "graphics", "aniso",
                        "Sharper textures at a slant. 1 = as the game sets it.");
    r.choices = {{"1", "As the game (console)"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Field of view", Kind::kSlider, When::kRestart, "graphics", "fov",
                        "Field of view as a multiple of the game's (1 = console). The weapon keeps its own view.");
    r.min = 0.5f, r.max = 2.0f, r.step = 0.05f, r.format = "%.2fx";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Widescreen (Hor+)", Kind::kChoice, When::kRestart, "graphics", "aspect",
                        "Wider than 16:9 (21:9, 32:9): the same height, more to the sides; HUD, menus and videos stay "
                        "16:9 in the middle. 16:9 = as the console (bars at the sides).");
    r.choices = {{"auto", "Auto (the window's shape)"}, {"16:9", "16:9 (console)"}, {"21:9", "21:9"}, {"32:9", "32:9"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Multisampling (MSAA)", Kind::kChoice, When::kRestart, "graphics", "msaa",
                        "The game's own anti-aliasing: as on the console (2x), off, or more samples on the screen "
                        "(limited to what the GPU can do).");
    r.choices = {{"console", "As the console (2x)"}, {"off", "Off"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}};
    r.get = [] {  // older files: true / false
      std::string v = fcp::settings::Value("graphics", "msaa");
      for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
      if (v == "true" || v == "1" || v == "on" || v.empty()) return std::string("console");
      if (v == "false" || v == "0") return std::string("off");
      return v;
    };
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "SMAA", Kind::kBool, When::kLive, "graphics", "smaa",
                        "Edge smoothing (SMAA 1x) on the finished picture, under the menu. Off = as the console.",
                        [](const std::string& v) { fcp::render::SetSmaa(v == "true"); });
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Gamma ramp", Kind::kBool, When::kLive, "graphics", "gamma",
                        "The game's brightness curve (its brightness setting), as on the console.",
                        [](const std::string& v) { fcp::render::SetGammaRamp(v == "true"); });
    g_rows.push_back(std::move(r));
  }
  // ---- Input ----
  {
    Row r = InputRow(2, "Mouse sensitivity", Kind::kSlider, When::kLive, "mouse", "sensitivity",
                     "Both axes: 1.00 turns 0.1 degrees per mouse count.", [] { return Num(GetLive().sensitivity_x); });
    r.min = 0.1f, r.max = 10.0f, r.step = 0.05f;
    r.set = [](const std::string& v) {
      std::string e = fcp::pc_input::Store("mouse", "sensitivity_x", v);
      if (e.empty()) e = fcp::pc_input::Store("mouse", "sensitivity_y", v);
      if (e.empty()) e = fcp::pc_input::Store("mouse", "sensitivity", v);
      return e;
    };
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Vertical sensitivity", Kind::kSlider, When::kLive, "mouse", "sensitivity_y",
                     "Up and down only (the row above sets both).", [] { return Num(GetLive().sensitivity_y); });
    r.min = 0.1f, r.max = 10.0f, r.step = 0.05f;
    g_rows.push_back(std::move(r));
  }
  g_rows.push_back(InputRow(2, "Invert mouse Y", Kind::kBool, When::kLive, "mouse", "invert_y",
                            "Mouse forward looks down.", [] { return BoolText(GetLive().invert_y); }));
  {
    Row r = InputRow(2, "Mouse acceleration", Kind::kSlider, When::kLive, "mouse", "acceleration",
                     "0 = raw input. Extra gain per count/ms of mouse speed (0.05: 1.5x at 10 counts/ms).",
                     [] { return Num(GetLive().acceleration, 3); });
    r.min = 0.0f, r.max = 1.0f, r.step = 0.01f, r.format = "%.3f";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Acceleration limit", Kind::kSlider, When::kLive, "mouse", "acceleration_cap",
                     "The highest gain acceleration reaches.", [] { return Num(GetLive().acceleration_cap); });
    r.min = 1.0f, r.max = 5.0f, r.step = 0.1f, r.format = "%.1fx";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Mouse smoothing", Kind::kSlider, When::kLive, "mouse", "smoothing",
                     "0 = off. Milliseconds each motion is spread over (the total angle stays exact).",
                     [] { return Num(GetLive().smoothing_ms, 0); });
    r.min = 0.0f, r.max = 100.0f, r.step = 5.0f, r.format = "%.0f ms";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Mouse while zoomed", Kind::kChoice, When::kLive, "mouse", "zoom_scale",
                     "Scope, binoculars, vehicle view: slower by the field of view as the game scales the stick "
                     "(fov), by tan(fov/2), or unchanged.",
                     [] { return GetLive().zoom_scale; }, true);
    r.choices = {{"fov", "By field of view"}, {"tan", "By tan(fov/2)"}, {"off", "Unchanged"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Zoomed sensitivity", Kind::kSlider, When::kLive, "mouse", "zoom_sensitivity",
                     "An extra factor while zoomed.", [] { return Num(GetLive().zoom_sensitivity); });
    r.min = 0.05f, r.max = 3.0f, r.step = 0.05f, r.format = "%.2fx";
    g_rows.push_back(std::move(r));
  }
  g_rows.push_back(InputRow(2, "Keyboard", Kind::kBool, When::kLive, "keyboard", "enabled",
                            "Keyboard as player 1's controller.", [] { return BoolText(GetLive().keyboard); }));
  g_rows.push_back(InputRow(2, "Mouse", Kind::kBool, When::kLive, "mouse", "enabled",
                            "Mouse look and mouse buttons.", [] { return BoolText(GetLive().mouse); }));
  {
    Row r = InputRow(2, "Controller dead zone", Kind::kSlider, When::kLive, "pad", "dead_zone",
                     "0 = only the game's own. A stick inside this share of full deflection counts as centred "
                     "(drifting sticks).",
                     [] { return Num(GetLive().dead_zone); });
    r.min = 0.0f, r.max = 0.5f, r.step = 0.01f;
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Vibration", Kind::kSlider, When::kLive, "pad", "rumble", "Vibration strength, 0 = off.",
                     [] { return std::to_string(GetLive().rumble); });
    r.min = 0.0f, r.max = 100.0f, r.step = 5.0f, r.format = "%.0f %%";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(2, "Controller driver", Kind::kChoice, When::kRestart, "pad", "backend",
                     "SDL: every controller, up to four players. XInput: Xbox pads only. Off: no controllers.",
                     [] { return GetLive().backend; }, true);
    r.choices = {{"sdl", "SDL (all controllers)"}, {"xinput", "XInput"}, {"off", "Off"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(2, "Button prompts", Kind::kChoice, When::kLive, "input", "button_prompts",
                        "Button symbols in hints and menus. Auto: the device used last (the bound key with keyboard "
                        "and mouse, the controller's own buttons). Xbox: as the console.",
                        [](const std::string&) { fcp::prompts::Reload(); });
    r.choices = {{"auto", "Auto (device in use)"}, {"xbox", "Xbox (console)"}, {"keyboard", "Keyboard and mouse"},
                 {"playstation", "PlayStation"}, {"switch", "Switch"}};
    g_rows.push_back(std::move(r));
  }
  // ---- Game ----
  g_rows.push_back(SettingsRow(3, "Pause on focus loss", Kind::kBool, When::kLive, "game", "pause_on_focus_loss",
                               "While the window is in the background the game pauses (its own pause menu in a "
                               "level), the sound is muted and the vibration stops.",
                               [](const std::string&) { fcp::pc_input::Reload(); }));
  g_rows.push_back(SettingsRow(3, "Skip intro videos", Kind::kBool, When::kLive, "game", "skip_intro",
                               "The logo and intro videos at start are not shown. The story's own cutscenes stay. "
                               "Holds from the next start (or the next of these videos)."));
  g_rows.push_back(SettingsRow(3, "Frame rate display", Kind::kBool, When::kLive, "game", "fps_counter",
                               "Frames per second and frame time in the top left corner."));
  {
    Row r = SettingsRow(3, "Language", Kind::kChoice, When::kRestart, "game", "language",
                        "Text and speech of the game, as the console's system language. Auto: the Windows display "
                        "language (English when it is none of these).");
    r.choices = {{"english", "English"}, {"french", "Francais"}, {"german", "Deutsch"},
                 {"spanish", "Espanol"}, {"italian", "Italiano"}, {"auto", "Auto (Windows)"}};
    g_rows.push_back(std::move(r));
  }
  for (Row& r : g_rows) r.start = r.get();
}

// The value of a row as the menu draws it (a slider's text as a number, a choice as its index).
int ChoiceIndex(const Row& r, const std::string& v) {
  const std::string l = Lower(v);
  for (size_t i = 0; i < r.choices.size(); ++i)
    if (Lower(r.choices[i].value) == l) return int(i);
  // numbers: "1.0" = "1"
  char* end = nullptr;
  const double d = std::strtod(l.c_str(), &end);
  if (end && end != l.c_str() && !*end)
    for (size_t i = 0; i < r.choices.size(); ++i)
      if (std::strtod(r.choices[i].value.c_str(), nullptr) == d && r.choices[i].value != "auto") return int(i);
  return -1;
}

void Change(Row& r, const std::string& value) {
  if (r.get() == value) return;
  const std::string error = r.set(value);
  std::string note;
  if (!error.empty()) {
    note = "FAILED: " + error;
  } else if (const std::string o = r.override_note(); !o.empty()) {
    note = "stored; " + o + " from the environment wins";
  } else if (r.when == When::kRestart) {
    note = r.get() == r.start ? "stored (as at start)" : "stored, restart required";
  } else {
    note = "applied and stored";
  }
  Status(std::format("{} = {}: {}", r.label, value, note));
}

// ---- open / close ----
// ImGui's flags follow g_open inside Draw (the listener runs without ImGui's context).
bool g_io_open = false;
void ApplyIo(ImGuiIO& io) {
  const bool open = g_open.load();
  if (open == g_io_open) return;
  g_io_open = open;
  if (open) {
    g_saved_config_flags = io.ConfigFlags;
    g_saved_nav_capture = io.ConfigNavCaptureKeyboard;
    // keyboard and pad navigation; the keys still reach the listeners (F10, Escape, a binding capture)
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCaptureKeyboard = false;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  } else {
    io.ConfigFlags = g_saved_config_flags;
    io.ConfigNavCaptureKeyboard = g_saved_nav_capture;
    io.ClearInputKeys();
  }
}

void SetOpen(bool open, const char* why) {
  if (g_open.load() == open) return;
  if (open) {
    g_pad_armed = false;
  } else {
    g_capture = -1;
    g_hide_chord = true;  // a held Back/Start does not reach the game after closing
  }
  g_open = open;
  FCPLOG("settings menu: {} ({}) at {:.2f} s, level {:.2f} s, frame {}", open ? "open" : "closed", why, Now(),
         fcp::gametime::LevelSeconds(), fcp::gametime::Frame());
  if (g_pause) fcp::pc_input::SetMenuPause(open);
}

// ---- the pads (every connected one, through the SDK; also while the game polls nothing) ----
uint16_t ReadPads(rex::hid::PadState& merged) {
  merged = {};
  rex::hid::IInputSystem* input = fcp::pc_input::Input();
  if (input) {
    for (uint32_t u = 0; u < 4; ++u) {
      auto r = input->GetState(u);
      if (!r) continue;
      const rex::hid::PadState& s = r->state;
      merged.buttons |= s.buttons;
      merged.left_trigger = std::max(merged.left_trigger, s.left_trigger);
      merged.right_trigger = std::max(merged.right_trigger, s.right_trigger);
      auto bigger = [](int16_t a, int16_t b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; };
      merged.thumb_lx = bigger(merged.thumb_lx, s.thumb_lx);
      merged.thumb_ly = bigger(merged.thumb_ly, s.thumb_ly);
    }
  }
  merged.buttons |= g_script_pad.load();
  return merged.buttons;
}

constexpr uint16_t kChord = rex::hid::kPadBack | rex::hid::kPadStart;

void FeedPads(ImGuiIO& io, const rex::hid::PadState& pad) {
  uint16_t b = pad.buttons;
  const bool held = b || pad.left_trigger > 30 || pad.right_trigger > 30 || std::abs(int(pad.thumb_lx)) > 8000 ||
                    std::abs(int(pad.thumb_ly)) > 8000;
  rex::hid::PadState p = pad;
  if (!g_pad_armed) {
    if (held) p = {}, b = 0;
    else g_pad_armed = true;
  }
  if (g_capture >= 0) p = {}, b = 0;
  auto key = [&](ImGuiKey k, uint16_t bit) { io.AddKeyEvent(k, (b & bit) != 0); };
  key(ImGuiKey_GamepadFaceDown, rex::hid::kPadA);
  key(ImGuiKey_GamepadFaceRight, rex::hid::kPadB);
  key(ImGuiKey_GamepadFaceLeft, rex::hid::kPadX);
  key(ImGuiKey_GamepadFaceUp, rex::hid::kPadY);
  key(ImGuiKey_GamepadDpadUp, rex::hid::kPadDPadUp);
  key(ImGuiKey_GamepadDpadDown, rex::hid::kPadDPadDown);
  key(ImGuiKey_GamepadDpadLeft, rex::hid::kPadDPadLeft);
  key(ImGuiKey_GamepadDpadRight, rex::hid::kPadDPadRight);
  key(ImGuiKey_GamepadL1, rex::hid::kPadLeftShoulder);
  key(ImGuiKey_GamepadR1, rex::hid::kPadRightShoulder);
  auto analog = [&](ImGuiKey k, int v, int v0, int v1) {
    const float n = float(v - v0) / float(v1 - v0);
    io.AddKeyAnalogEvent(k, n > 0.1f, std::clamp(n, 0.0f, 1.0f));
  };
  constexpr int kDead = 7849;  // XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
  analog(ImGuiKey_GamepadLStickLeft, p.thumb_lx, -kDead, -32768);
  analog(ImGuiKey_GamepadLStickRight, p.thumb_lx, kDead, 32767);
  analog(ImGuiKey_GamepadLStickUp, p.thumb_ly, kDead, 32767);
  analog(ImGuiKey_GamepadLStickDown, p.thumb_ly, -kDead, -32768);
}

// ---- frame rate display ----
struct FpsMeter {
  std::deque<Clock::time_point> stamps;
  double fps = 0, avg_ms = 0, max_ms = 0;
  Clock::time_point last_update{};
  void Tick() {
    const auto now = Clock::now();
    stamps.push_back(now);
    while (stamps.size() > 2 && now - stamps.front() > std::chrono::seconds(1)) stamps.pop_front();
    if (now - last_update < std::chrono::milliseconds(250) || stamps.size() < 2) return;
    last_update = now;
    const double span = std::chrono::duration<double>(stamps.back() - stamps.front()).count();
    fps = span > 0 ? double(stamps.size() - 1) / span : 0;
    avg_ms = stamps.size() > 1 ? span * 1000.0 / double(stamps.size() - 1) : 0;
    max_ms = 0;
    for (size_t i = 1; i < stamps.size(); ++i)
      max_ms = std::max(max_ms, std::chrono::duration<double, std::milli>(stamps[i] - stamps[i - 1]).count());
  }
} g_fps;

float FontPx(const ImGuiIO& io) { return std::clamp(io.DisplaySize.y / 40.0f, 13.0f, 40.0f); }

void DrawFps(const ImGuiIO& io) {
  const float px = std::clamp(io.DisplaySize.y / 55.0f, 12.0f, 30.0f);
  ImGui::PushFont(g_font, px);
  ImGui::SetNextWindowPos(ImVec2(px * 0.5f, px * 0.5f));
  ImGui::SetNextWindowBgAlpha(0.55f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoSavedSettings;
  if (ImGui::Begin("##fcp_fps", nullptr, flags)) {
    ImGui::Text("%3.0f fps  %5.2f ms  (max %.1f)", g_fps.fps, g_fps.avg_ms, g_fps.max_ms);
  }
  ImGui::End();
  ImGui::PopFont();
}

// ---- the menu ----
const char* const kBindingLabels[][2] = {
    {"a", "A: jump"},
    {"b", "B: kick / back"},
    {"x", "X: reload / use"},
    {"y", "Y: feral ability"},
    {"lb", "LB: throw projectile"},
    {"rb", "RB: next selection"},
    {"lt", "LT: secondary fire"},
    {"rt", "RT: fire"},
    {"ls", "Left stick click: stance"},
    {"rs", "Right stick click: zoom"},
    {"start", "Start: pause menu"},
    {"back", "Back: scoreboard"},
    {"up", "D-pad up: binoculars"},
    {"down", "D-pad down: special weapon"},
    {"left", "D-pad left: primary weapon"},
    {"right", "D-pad right: secondary weapon"},
    {"lup", "Move forward"},
    {"ldown", "Move back"},
    {"lleft", "Move left"},
    {"lright", "Move right"},
    {"rup", "Look up (keys)"},
    {"rdown", "Look down (keys)"},
    {"rleft", "Look left (keys)"},
    {"rright", "Look right (keys)"},
};

std::string BindingLabel(const std::string& name) {
  for (const auto& l : kBindingLabels)
    if (name == l[0]) return l[1];
  return name;
}

void StoreBinding(int target, const std::string& keys) {
  const std::string name = fcp::pc_input::BindingName(target);
  const std::string e = fcp::pc_input::Store("bindings", name, "\"" + keys + "\"");
  Status(e.empty() ? std::format("binding {} = {}: applied and stored", name, keys) : "binding FAILED: " + e);
}

void DrawRowControl(Row& r, int index) {
  ImGui::PushID(index);
  const std::string value = r.get();
  const float width = -FLT_MIN;
  ImGui::SetNextItemWidth(width);
  switch (r.kind) {
    case Kind::kBool: {
      bool b = Lower(value) == "true" || value == "1";
      if (ImGui::Checkbox("##v", &b)) Change(r, b ? "true" : "false");
      break;
    }
    case Kind::kChoice: {
      const int at = ChoiceIndex(r, value);
      const std::string preview = at >= 0 ? r.choices[size_t(at)].label : value;
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (size_t i = 0; i < r.choices.size(); ++i) {
          const bool selected = int(i) == at;
          if (ImGui::Selectable(r.choices[i].label.c_str(), selected)) Change(r, r.choices[i].value);
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      break;
    }
    case Kind::kSlider: {
      static std::map<int, float> editing;  // the value while the slider is held
      float v = editing.contains(index) ? editing[index] : float(std::strtod(value.c_str(), nullptr));
      if (value == "auto" || value.empty()) v = 1.0f;
      ImGui::SliderFloat("##v", &v, r.min, r.max, r.format, ImGuiSliderFlags_AlwaysClamp);
      if (ImGui::IsItemActive()) editing[index] = v;
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        const float q = r.step > 0 ? std::round(v / r.step) * r.step : v;
        editing.erase(index);
        Change(r, Num(q, r.step < 0.01f ? 3 : 2));
      } else if (!ImGui::IsItemActive()) {
        editing.erase(index);
      }
      break;
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && r.help) ImGui::SetItemTooltip("%s", r.help);
  ImGui::PopID();
}

void DrawRows(int tab, float em) {
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("rows", 3, flags)) return;
  ImGui::TableSetupColumn("setting", ImGuiTableColumnFlags_WidthFixed, em * 14);
  ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("note", ImGuiTableColumnFlags_WidthFixed, em * 10);
  for (size_t i = 0; i < g_rows.size(); ++i) {
    Row& r = g_rows[i];
    if (r.tab != tab) continue;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(r.label);
    if (ImGui::IsItemHovered() && r.help) ImGui::SetTooltip("%s", r.help);
    ImGui::TableNextColumn();
    DrawRowControl(r, int(i));
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    const std::string over = r.override_note();
    if (!over.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "set by environment");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s wins over the file", over.c_str());
    } else if (r.when == When::kRestart && r.get() != r.start) {
      ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "restart required");
    } else if (r.when == When::kRestart) {
      ImGui::TextDisabled("at start");
    } else {
      ImGui::TextDisabled("at once");
    }
  }
  ImGui::EndTable();
}

void DrawBindings(float em) {
  ImGui::SeparatorText("Keyboard and mouse bindings (player 1)");
  if (g_capture >= 0)
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Press a key or mouse button for %s (Escape cancels)",
                       BindingLabel(fcp::pc_input::BindingName(g_capture)).c_str());
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (!ImGui::BeginTable("bindings", 3, flags)) return;
  ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthFixed, em * 14);
  ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed, em * 13);
  for (int i = 0; i < fcp::pc_input::BindingCount(); ++i) {
    ImGui::PushID(i);
    const std::string name = fcp::pc_input::BindingName(i);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(BindingLabel(name).c_str());
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(g_capture == i ? "..." : fcp::pc_input::BindingKeys(i).c_str());
    ImGui::TableNextColumn();
    if (ImGui::SmallButton("Change")) {
      g_capture = i;
      Status(std::format("waiting for a key for {}", name));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) StoreBinding(i, "none");
    ImGui::SameLine();
    if (ImGui::SmallButton("Default")) StoreBinding(i, fcp::pc_input::DefaultBindingKeys(i));
    ImGui::PopID();
  }
  ImGui::EndTable();
  ImGui::TextDisabled("Controller buttons can be remapped in predator_input.toml ([pad] pad_bind_<button>).");
}

std::string ReadVersionFile() {
  std::ifstream in(rex::platform::GetExecutableFolder() / "VERSION");
  std::string v;
  std::getline(in, v);
  return Trim(v);
}

void DrawAbout() {
  static const std::string version = ReadVersionFile();
  ImGui::Text("Far Cry Instincts Predator - PC recompilation %s", version.empty() ? "(development build)" : version.c_str());
  ImGui::Text("Build %s, commit %s%s, %s", FCP_BUILD_CONFIG, FCP_GIT_COMMIT, FCP_GIT_DIRTY ? " (modified)" : "", FCP_BUILD_TIME);
  ImGui::Text("Graphics: %s; Dear ImGui %s", fcp::render::BackendName(), IMGUI_VERSION);
  ImGui::Spacing();
  ImGui::TextWrapped(
      "This program contains no part of the game. The game code is made on this PC from your own disc image. "
      "Far Cry is a trademark of Ubisoft Entertainment; this project is not affiliated with or endorsed by Ubisoft "
      "or Microsoft.");
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Third-party components (ReXGlue SDK, Xenia, NVRHI, Dear ImGui, SDL3, FFmpeg, LLVM and others) keep their "
      "licenses: see THIRD_PARTY_NOTICES.md and the folder licenses next to the program.");
  ImGui::Spacing();
  ImGui::TextDisabled("Settings: %s", fcp::settings::File().string().c_str());
  ImGui::TextDisabled("Bindings: %s", fcp::pc_input::ConfigFile().string().c_str());
}

void DrawMenu(ImGuiIO& io) {
  const float px = FontPx(io);
  const float em = px;
  ImGui::PushFont(g_font, px);
  const float k = px / 13.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * k, 10 * k));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6 * k, 3 * k));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * k, 5 * k));
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5 * k, 3 * k));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4 * k);
  const ImVec2 size(std::min(io.DisplaySize.x * 0.9f, em * 46), std::min(io.DisplaySize.y * 0.88f, em * 34));
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // opaque (the SDK's style lets the game shine through frames and windows)
  constexpr ImGuiCol kOpaque[] = {ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered,
                                  ImGuiCol_FrameBgActive, ImGuiCol_TitleBg, ImGuiCol_TitleBgActive};
  for (ImGuiCol c : kOpaque) {
    ImVec4 v = ImGui::GetStyleColorVec4(c);
    if (c == ImGuiCol_FrameBg || c == ImGuiCol_FrameBgHovered || c == ImGuiCol_FrameBgActive) {
      const ImVec4 w = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);  // the frame over the window, blended once
      v = ImVec4(w.x + (v.x - w.x) * v.w, w.y + (v.y - w.y) * v.w, w.z + (v.z - w.z) * v.w, 1.0f);
      v.x = std::min(1.0f, v.x + 0.06f), v.y = std::min(1.0f, v.y + 0.06f), v.z = std::min(1.0f, v.z + 0.06f);
    }
    v.w = 1.0f;
    ImGui::PushStyleColor(c, v);
  }
  static bool focus_first = true;
  if (focus_first) ImGui::SetNextWindowFocus(), focus_first = false;
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings;
  bool keep = true;
  if (ImGui::Begin("Settings###fcp_settings", &keep, flags)) {
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    if (ImGui::BeginTabBar("tabs")) {
      for (int t = 0; t < kTabCount; ++t) {
        const ImGuiTabItemFlags tf = g_select_tab == t ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(kTabs[t], nullptr, tf)) {
          ImGui::BeginChild("page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
          if (t < 4) DrawRows(t, em);
          if (t == 2) DrawBindings(em);
          if (t == 4) DrawAbout();
          ImGui::EndChild();
          ImGui::EndTabItem();
        }
      }
      g_select_tab = -1;
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (!g_status.empty() && Now() < g_status_until) ImGui::TextUnformatted(g_status.c_str());
    else ImGui::TextDisabled("Changes are stored at once. Hover a setting for help.");
    ImGui::TextDisabled("F10 / Escape / Back+Start / B: close");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - em * 5);
    if (ImGui::Button("Close", ImVec2(em * 5, 0))) keep = false;
  }
  ImGui::End();
  ImGui::PopStyleColor(int(std::size(kOpaque)));
  ImGui::PopStyleVar(5);
  ImGui::PopFont();
  if (!keep) {
    focus_first = true;
    SetOpen(false, "close button");
  }
  if (!g_open.load()) focus_first = true;
}

// ---- FCP_TEST_MENU ----
struct ScriptItem {
  double at;
  std::string what;
  bool done = false;
};
std::vector<ScriptItem> g_script;

void PostKey(const std::string& name) {
  if (!g_ui) return;
  auto native = g_ui->GetNativeWindow();
  HWND hwnd = native ? static_cast<HWND>(native->window) : nullptr;
  const std::string n = Lower(name);
  const int vk = n == "f10" ? VK_F10 : n == "escape" ? VK_ESCAPE : n == "enter" ? VK_RETURN : n == "up" ? VK_UP
                 : n == "down" ? VK_DOWN : n == "left" ? VK_LEFT : n == "right" ? VK_RIGHT : n == "space" ? VK_SPACE : 0;
  if (!hwnd || !vk) {
    FCPLOG("FCP_TEST_MENU: post={}: {}", name, hwnd ? "unknown key" : "no window");
    return;
  }
  const LPARAM scan = LPARAM(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
  FCPLOG("FCP_TEST_MENU: post {} (foreground: {})", name, GetForegroundWindow() == hwnd ? "yes" : "no");
  PostMessageW(hwnd, vk == VK_F10 ? WM_SYSKEYDOWN : WM_KEYDOWN, WPARAM(vk), 1 | scan);
  PostMessageW(hwnd, vk == VK_F10 ? WM_SYSKEYUP : WM_KEYUP, WPARAM(vk), 1 | scan | (LPARAM(1) << 30) | (LPARAM(1) << 31));
}

uint16_t PadBits(const std::string& list) {
  static constexpr struct {
    const char* name;
    uint16_t bits;
  } kPad[] = {{"a", rex::hid::kPadA},           {"b", rex::hid::kPadB},          {"x", rex::hid::kPadX},
              {"y", rex::hid::kPadY},           {"back", rex::hid::kPadBack},    {"start", rex::hid::kPadStart},
              {"up", rex::hid::kPadDPadUp},     {"down", rex::hid::kPadDPadDown}, {"left", rex::hid::kPadDPadLeft},
              {"right", rex::hid::kPadDPadRight}, {"combo", kChord}};
  uint16_t bits = 0;
  std::stringstream ss(list);
  for (std::string item; std::getline(ss, item, '+');)
    for (const auto& p : kPad)
      if (Lower(Trim(item)) == p.name) bits = uint16_t(bits | p.bits);
  return bits;
}

void RunScript() {
  const double now = Now();
  for (ScriptItem& it : g_script) {
    if (it.done || now < it.at) continue;
    it.done = true;
    const std::string& w = it.what;
    const size_t eq = w.find('=');
    const std::string verb = Lower(w.substr(0, eq)), arg = eq == std::string::npos ? "" : w.substr(eq + 1);
    FCPLOG("FCP_TEST_MENU: {:.1f} s: {}", it.at, w);
    if (verb == "open") SetOpen(true, "FCP_TEST_MENU");
    else if (verb == "close") SetOpen(false, "FCP_TEST_MENU");
    else if (verb == "toggle") SetOpen(!g_open.load(), "FCP_TEST_MENU");
    else if (verb == "tab") {
      for (int t = 0; t < kTabCount; ++t)
        if (Lower(kTabs[t]) == Lower(arg)) g_select_tab = t;
    } else if (verb == "dump") {
      fcp::render::RequestPresentDump(arg.empty() ? "menu.png" : arg);
    } else if (verb == "set" || verb == "input") {
      const size_t dot = arg.find('.'), eq2 = arg.find('=');
      if (dot == std::string::npos || eq2 == std::string::npos || eq2 < dot) {
        FCPLOG("FCP_TEST_MENU: '{}' is not <section>.<key>=<value>", w);
        continue;
      }
      const std::string section = arg.substr(0, dot), key = arg.substr(dot + 1, eq2 - dot - 1), value = arg.substr(eq2 + 1);
      // the menu's own row when there is one (the same path as a click), else the file directly
      const std::string id = (verb == "input" ? "input:" : "") + section + "." + key;
      bool found = false;
      for (Row& r : g_rows)
        if (r.id == id) Change(r, value), found = true;
      if (!found && verb == "set") {
        const std::string e = fcp::settings::Store(section, key, value);
        Status(std::format("[{}] {} = {}: {}", section, key, value, e.empty() ? "stored" : e));
      } else if (!found) {
        const std::string e = fcp::pc_input::Store(section, key, value);
        Status(std::format("input [{}] {} = {}: {}", section, key, value, e.empty() ? "applied and stored" : e));
      }
    } else if (verb == "post") {
      PostKey(arg);
    } else if (verb == "pad") {
      const uint16_t bits = PadBits(arg);
      g_script_pad = uint16_t(g_script_pad.load() | bits);
      g_script.push_back({now + 0.2, "padup=" + arg});
    } else if (verb == "padup") {
      g_script_pad = uint16_t(g_script_pad.load() & ~PadBits(arg));
    } else {
      FCPLOG("FCP_TEST_MENU: unknown action '{}'", w);
    }
  }
}

void ParseScript() {
  const std::string text = fcp::env::Get("FCP_TEST_MENU");
  if (text.empty()) return;
  if (!fcp::devstart::ActiveInGeneration("FCP_TEST_MENU_GENS")) {
    FCPLOG("FCP_TEST_MENU: not in this relaunch generation");
    return;
  }
  std::stringstream ss(text);
  for (std::string item; std::getline(ss, item, ',');) {
    item = Trim(item);
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      FCPLOG("FCP_TEST_MENU: '{}' ignored (<t>:<action>)", item);
      continue;
    }
    g_script.push_back({std::atof(item.c_str()), item.substr(colon + 1)});
  }
  FCPLOG("FCP_TEST_MENU: {} actions", g_script.size());
}

void LoadFont(ImGuiIO& io);

// ---- the dialog (always there: draws the menu, the frame rate display, reads the pads) ----
class MenuDialog final : public rex::ui::DialogBase {
 public:
  bool Draw(ImGuiIO& io) override {
    if (!font_tried_ && (g_open.load() || fcp::env::Flag("FCP_FPS_COUNTER"))) {
      font_tried_ = true;
      LoadFont(io);  // on first use, inside the frame (the atlas is not locked with RendererHasTextures)
    }
    g_fps.Tick();
    RunScript();
    rex::hid::PadState pad;
    const uint16_t buttons = ReadPads(pad);
    // Back + Start: open or close (on the press of the chord)
    if ((buttons & kChord) == kChord && (g_pad_prev & kChord) != kChord) {
      g_hide_chord = true;
      SetOpen(!g_open.load(), "Back+Start");
      g_pad_armed = false;
    }
    if (g_open.load()) {
      FeedPads(io, pad);
      // B closes, as Escape does (not while a binding waits for a key)
      if (g_pad_armed && g_capture < 0 && (buttons & rex::hid::kPadB) && !(g_pad_prev & rex::hid::kPadB) &&
          !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        SetOpen(false, "B");
      }
    }
    g_pad_prev = buttons;
    ApplyIo(io);
    if (fcp::env::Flag("FCP_FPS_COUNTER")) DrawFps(io);
    if (g_open.load()) {
      // a binding waits: the mouse goes to the listener, not to ImGui
      if (g_capture >= 0) io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
      else io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
      DrawMenu(io);
    }
    return true;
  }

 private:
  bool font_tried_ = false;
};
MenuDialog g_dialog;
rex::ScopeGuard g_dialog_guard;

// ---- F10, Escape, a binding capture (UI thread, before ImGui and the game's keys) ----
class Hotkeys final : public rex::ui::IUIEventListener {
 public:
  void OnUIEvent(const rex::ui::UIEvent&) override {}
  void OnInputEvent(rex::InputEvent& e) override {
    using T = rex::InputEvent::Type;
    if (g_capture >= 0 && g_open.load()) {
      std::string key;
      if (e.type == T::kKeyDown && !e.repeat) {
        if (e.key == rex::VirtualKey::kEscape) {
          e.handled = true;
          Status("binding unchanged");
          g_capture = -1;
          return;
        }
        if (e.key == rex::VirtualKey::kF10) return;
        key = fcp::pc_input::KeyName(int(e.key));
      } else if (e.type == T::kButtonDown) {
        const int vk = e.button == rex::MouseButton::kLeft     ? VK_LBUTTON
                       : e.button == rex::MouseButton::kRight  ? VK_RBUTTON
                       : e.button == rex::MouseButton::kMiddle ? VK_MBUTTON
                       : e.button == rex::MouseButton::kX1     ? VK_XBUTTON1
                       : e.button == rex::MouseButton::kX2     ? VK_XBUTTON2
                                                               : 0;
        key = fcp::pc_input::KeyName(vk);
      } else if (e.type == T::kWheel && e.scroll_y != 0) {
        key = fcp::pc_input::WheelName(e.scroll_y < 0);
      }
      if (!key.empty()) {
        e.handled = true;
        const int target = g_capture;
        g_capture = -1;
        StoreBinding(target, key);
      }
      return;
    }
    if (e.type != T::kKeyDown || e.repeat) return;
    if (e.key == rex::VirtualKey::kF10 && e.modifiers == rex::KeyModifiers{}) {
      e.handled = true;  // never the game's
      SetOpen(!g_open.load(), "F10");
    } else if (e.key == rex::VirtualKey::kEscape && g_open.load()) {
      e.handled = true;
      SetOpen(false, "Escape");
    }
  }
};
Hotkeys g_hotkeys;
rex::ScopeGuard g_hotkeys_guard;

void LoadFont(ImGuiIO& io) {
  char windir[MAX_PATH] = "C:\\Windows";
  GetEnvironmentVariableA("WINDIR", windir, sizeof windir);
  for (const char* name : {"seguisb.ttf", "segoeui.ttf", "arial.ttf"}) {
    const std::string path = std::string(windir) + "\\Fonts\\" + name;
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    ImFontConfig config;
    config.OversampleH = 2;
    config.OversampleV = 1;
    if (ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &config)) {
      g_font = f;
      g_font_name = name;
      break;
    }
  }
  FCPLOG("settings menu: font {} (ImGui {})", g_font_name, IMGUI_VERSION);
}

}  // namespace

void Attach(rex::ui::IUISystem& ui) {
  g_enabled = fcp::env::Flag("FCP_MENU", true);
  if (!g_enabled) {
    FCPLOG("settings menu: off (FCP_MENU=0)");
    return;
  }
  g_ui = &ui;
  g_pause = fcp::env::Flag("FCP_MENU_PAUSE", true);
  ParseScript();
  g_hotkeys_guard = ui.AddEventListener(g_hotkeys);
  ui.PostTask([&ui] {
    BuildRows();
    g_dialog_guard = ui.AddDialog(g_dialog);
    FCPLOG("settings menu: ready (F10, Back+Start; renderer {}, {}; holds the game while open: {})",
           fcp::render::BackendName(), fcp::render::Presenting() ? "presenting" : "NOT presenting: the menu is invisible",
           g_pause ? "yes" : "no (FCP_MENU_PAUSE=0)");
  });
}

void Detach() {
  if (!g_ui) return;
  g_hotkeys_guard.Reset();
  rex::ui::IUISystem* ui = g_ui;
  (void)ui->RunSynchronous([] {
    if (g_open.load()) SetOpen(false, "shutdown");
    g_dialog_guard.Reset();
  });
  g_ui = nullptr;
}

bool BlocksInput() { return g_open.load(std::memory_order_relaxed); }

uint16_t PadButtonsToHide(uint16_t buttons) {
  if (!g_enabled) return 0;
  if ((buttons & kChord) == kChord) {
    g_hide_chord = true;
    return kChord;
  }
  if (g_hide_chord.load(std::memory_order_relaxed)) {
    if (!(buttons & kChord)) {
      g_hide_chord = false;
      return 0;
    }
    return uint16_t(buttons & kChord);
  }
  return 0;
}

}  // namespace fcp::menu
