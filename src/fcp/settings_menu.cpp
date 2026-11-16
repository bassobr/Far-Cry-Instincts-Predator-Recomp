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
#include <cstdlib>
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
#include <rex/app/app.h>
#include <rex/hid/input_system.h>
#include <rex/hid/pad.h>
#include <rex/input_keys.h>
#include <rex/platform/file.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "fcp/button_prompts.h"
#include "fcp/detail_distance.h"
#include "fcp/device_preset.h"
#include "fcp/devstart.h"
#include "fcp/display.h"
#include "fcp/env.h"
#include "fcp/framerate.h"
#include "fcp/gametime.h"
#include "fcp/guest.h"
#include "fcp/log.h"
#include "fcp/watchdog.h"
#include "fcp_addresses.h"
#include "fcp/pc_input.h"
#include "fcp/relaunch.h"
#include "fcp/render_bridge.h"
#include "fcp/speech_volume.h"
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
float g_scroll_to = -1;    // set by the test script: the page scrolled to this share of its height once
int g_page = 0;            // the tab shown (Defaults applies to it)
bool g_defaults = true;    // FCP_MENU_DEFAULTS=0: no Defaults button, no default marks (as before)
bool g_short_mark = true;  // FCP_MENU_SHORT_MARK=0: a closed choice field always shows the full default mark
bool g_wrap_hints = true;  // FCP_MENU_WRAP_HINTS=0: the grey hints of the Controller page stay on one line (cut)
int g_capture = -1;        // binding target waiting for a key, -1 none
// Controller page: the pad binding target waiting for a controller button (-1 none), added or in place of the list,
// armed once every button was let go, given up at that time (wall seconds)
int g_pad_capture = -1;
bool g_pad_capture_add = false, g_pad_capture_armed = false;
double g_pad_capture_until = 0;
constexpr double kPadCaptureSeconds = 8.0;
bool g_pad_armed = false;  // the pad counts once every button was let go after opening
uint16_t g_pad_prev = 0;
int g_saved_config_flags = 0;
bool g_saved_nav_capture = true;
enum class Ask { kNone, kExit, kRestart };
Ask g_ask = Ask::kNone;  // Exit Game / Apply asks first (below DrawRows)
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
  // "Defaults" of the page (ResetPage): the value it sets (the device's default where it has one), and the device's
  // name when the default is the device's ("" = the built-in one). No `def`: Defaults leaves the row alone.
  std::function<std::string()> def;
  std::string def_from;
};
std::vector<Row> g_rows;

const char* const kTabs[] = {"Display", "Graphics", "Input", "Controller", "Game", "About"};
constexpr int kTabCount = 6;
constexpr int kInputTab = 2, kControllerTab = 3, kGameTab = 4, kAboutTab = 5;

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
  r.def = [s, k] { return fcp::settings::DefaultText(s, k); };
  r.def_from = fcp::settings::DefaultSource(s, k);
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

// ---- Defaults (per page) ----
// The predator_input.toml rows' defaults (pc_input::Defaults), by row id; a row added later sets its own `def`.
void SetInputDefaults() {
  const fcp::pc_input::Live d = fcp::pc_input::Defaults();
  const std::map<std::string, std::string> values = {
      {"input:mouse.sensitivity", Num(d.sensitivity_x)},
      {"input:mouse.sensitivity_y", Num(d.sensitivity_y)},
      {"input:mouse.invert_y", BoolText(d.invert_y)},
      {"input:mouse.acceleration", Num(d.acceleration, 3)},
      {"input:mouse.acceleration_cap", Num(d.acceleration_cap)},
      {"input:mouse.smoothing", Num(d.smoothing_ms, 0)},
      {"input:mouse.zoom_scale", d.zoom_scale},
      {"input:mouse.zoom_sensitivity", Num(d.zoom_sensitivity)},
      {"input:keyboard.enabled", BoolText(d.keyboard)},
      {"input:mouse.enabled", BoolText(d.mouse)},
      {"input:pad.dead_zone", Num(d.dead_zone)},
      {"input:pad.rumble", std::to_string(d.rumble)},
      {"input:pad.backend", d.backend},
  };
  for (Row& r : g_rows) {
    if (r.def) continue;
    if (const auto it = values.find(r.id); it != values.end()) r.def = [v = it->second] { return v; };
  }
}

// "1" = "1.0", "True" = "true"
bool SameValue(const std::string& a, const std::string& b) {
  if (Lower(a) == Lower(b)) return true;
  char *ea = nullptr, *eb = nullptr;
  const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
  return ea && ea != a.c_str() && !*ea && eb && eb != b.c_str() && !*eb && std::fabs(x - y) < 1e-6;
}

// "Default: ..." for tooltips and the choice marked as the default
std::string DefaultNote(const Row& r) {
  return r.def_from.empty() ? std::string("default") : r.def_from + " default";
}

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
    // the video mode of exclusive fullscreen: the modes of the window's monitor (fcp/display.h); at once
    Row r = SettingsRow(0, "Exclusive mode", Kind::kChoice, When::kLive, "display", "exclusive_mode",
                        "Resolution and refresh rate of exclusive fullscreen, from the monitor's list. Desktop: the "
                        "monitor's own mode. The desktop gets its mode back on Alt+Tab and when the game ends.",
                        [](const std::string& v) { SetRuntime("ui.fullscreen_mode", Lower(v) == "desktop" ? "" : v); });
    const fcp::display::Mode desk = fcp::display::DesktopMode(g_ui);
    r.choices = {{"desktop", desk.width > 0 ? "Desktop (" + desk.Label() + ")" : std::string("Desktop")}};
    for (const fcp::display::Mode& m : fcp::display::MonitorModes(g_ui)) r.choices.push_back({m.Value(), m.Label()});
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
          "Client size of the window (not fullscreen; fullscreen uses the monitor's). Narrower or wider than 16:9 "
          "fills the window (see Picture shape); the render resolution is under Graphics."};
    // short enough for the combo with " - default" behind it (at most about 38 characters at 1280x720): on a Steam
    // Deck its own size; elsewhere 1280x720 at 100 % display scale, growing with the scale (fcp/display.h);
    // FCP_DPI_WINDOW=0: 1280x720 pixels at every scale, as before
    const bool deck = fcp::device::Name().starts_with("Steam Deck");
    r.choices = {{"0x0", deck ? "Built in (1280x800)"
                              : fcp::env::Flag("FCP_DPI_WINDOW", true) ? "Built in (1280x720 at 100 % scale)"
                                                                       : "Built in (1280x720, Deck 1280x800)"}};
    if (g_ui) {
      // the monitor's own size (native) first; the monitor of [display] monitor, else the first one
      const auto displays = g_ui->GetDisplays();
      size_t m = size_t(std::max(1, std::atoi(fcp::settings::Value("display", "monitor").c_str()))) - 1;
      if (m >= displays.size()) m = 0;
      if (!displays.empty() && displays[m].width > 0 && displays[m].height > 0)
        r.choices.push_back({std::format("{}x{}", displays[m].width, displays[m].height),
                             std::format("Native ({} x {})", displays[m].width, displays[m].height)});
    }
    for (const char* s : {"1024x768", "1280x720", "1280x800", "1600x900", "1680x1050", "1920x1080", "1920x1200", "2560x1080",
                          "2560x1440", "2560x1600", "3440x1440", "3840x2160"}) {
      std::string v = s, label = v;
      label.replace(label.find('x'), 1, " x ");
      bool dup = false;
      for (const Choice& c : r.choices) dup = dup || c.value == v;
      if (!dup) r.choices.push_back({v, label});
    }
    r.get = [] {
      return fcp::settings::Value("display", "width") + "x" + fcp::settings::Value("display", "height");
    };
    r.set = [](const std::string& v) {
      const size_t x = v.find('x');
      const std::string w = v.substr(0, x), h = v.substr(x + 1);
      std::string e = fcp::settings::Store("display", "width", w);
      if (e.empty()) e = fcp::settings::Store("display", "height", h);
      if (e.empty() && fcp::settings::EnvironmentOverride("display", "width").empty()) {
        // built in: 1280x720 at 100 %, scaled with the monitor's scale (fcp/display.h)
        int bw = 1280, bh = 720;
        fcp::display::BuiltInWindowSize(std::atoi(fcp::settings::Value("display", "monitor").c_str()), bw, bh);
        SetRuntime("ui.window_width", w == "0" ? std::to_string(bw) : w);
        SetRuntime("ui.window_height", h == "0" ? std::to_string(bh) : h);
      }
      return e;
    };
    r.override_note = [] { return fcp::settings::EnvironmentOverride("display", "width"); };
    r.id = "display.size";
    r.def = [] { return fcp::settings::DefaultText("display", "width") + "x" + fcp::settings::DefaultText("display", "height"); };
    r.def_from = fcp::settings::DefaultSource("display", "width");
    g_rows.push_back(std::move(r));
  }
  // Frame rate, VSync, pacing: at once through fcp::framerate::Reconfigure (FCP_FPS_LIVE=0: at the next start)
  const bool live_fps = fcp::framerate::LiveChanges();
  const auto reconfigure = [](const std::string&) { fcp::framerate::Reconfigure("settings menu"); };
  {
    Row r = SettingsRow(0, "Frame rate", Kind::kChoice, live_fps ? When::kLive : When::kRestart, "display", "fps",
                        "Highest frame rate: 30 as the console, 40 to 120, or Auto (the monitor's refresh rate, at most "
                        "120). Smooth when the display's refresh rate is a multiple (90 Hz: 30, 45, 90; 60 Hz: 30, 60; "
                        "120 Hz: 30, 40, 60, 120). On a 90 Hz display (Steam Deck OLED) 45 is smoother: Auto means 90 "
                        "there, without display pacing. predator.toml takes any whole number from 40 to 120 "
                        "(with unsupported_uncapped_fps = true up to 240: not supported, see FRAMERATE.md).",
                        live_fps ? std::function<void(const std::string&)>(reconfigure) : nullptr);
    r.choices = {{"auto", std::format("Auto (this monitor: {})", fcp::framerate::AutoTarget())}, {"30", "30 (console)"}};
    for (int f : {40, 45, 50, 60, 72, 75, 90, 100, 120}) r.choices.push_back({std::to_string(f), std::to_string(f)});
    // above 120 only with unsupported_uncapped_fps in predator.toml (as FCI's menu): marked as not supported
    const auto label = [](int f) {
      return f > fcp::framerate::kMaxFps ? std::to_string(f) + " (unsupported)" : std::to_string(f);
    };
    if (fcp::framerate::UncappedAllowed()) {
      for (int f : {144, 165, 240}) r.choices.push_back({std::to_string(f), label(f)});
    }
    // the monitor's own rate and half of it, when they are not listed and the game logic takes them
    const int monitor = fcp::framerate::AutoTarget();
    for (int f : {monitor, monitor / 2}) {
      bool listed = f == 30 || f < fcp::framerate::kMinFree || f > fcp::framerate::MaxFps();
      for (const Choice& c : r.choices) listed = listed || c.value == std::to_string(f);
      if (!listed)
        r.choices.push_back({std::to_string(f), label(f) + (f == monitor ? " (this monitor)" : " (half of this monitor)")});
    }
    std::sort(r.choices.begin() + 2, r.choices.end(),
              [](const Choice& a, const Choice& b) { return std::atoi(a.value.c_str()) < std::atoi(b.value.c_str()); });
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "VSync", Kind::kChoice, live_fps ? When::kLive : When::kRestart, "display", "vsync",
                        "Wait for the monitor when presenting. Auto: on up to 60 frames per second, off above.",
                        live_fps ? std::function<void(const std::string&)>(reconfigure) : nullptr);
    r.choices = {{"auto", "Auto"}, {"on", "On"}, {"off", "Off"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "Frame pacing", Kind::kChoice, live_fps ? When::kLive : When::kRestart, "display", "pacing",
                        "Display: every frame stays equally long on the display, timed by the display's refresh "
                        "(needs Vulkan and a frame rate the refresh rate is a multiple of). Game: the game's own timing.",
                        live_fps ? std::function<void(const std::string&)>(reconfigure) : nullptr);
    r.choices = {{"display", "Display"}, {"game", "Game"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "Frames ahead", Kind::kChoice, When::kLive, "display", "frame_queue",
                        "Game frames that may wait on the GPU before the game waits for it. 1: least input latency. "
                        "3: evens out single slow frames, up to one frame more latency.",
                        [](const std::string& v) { fcp::render::SetFramesAhead(std::atoi(v.c_str())); });
    r.choices = {{"1", "1 (least latency)"}, {"2", "2"}, {"3", "3"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "HDR10 output", Kind::kChoice, When::kLive, "display", "hdr",
                        "HDR10 on an HDR display. Auto: on when Windows HDR is on for this monitor. The game's picture "
                        "keeps its SDR look at the paper white. Off = SDR as before. Switches with the next frame.",
                        [](const std::string& v) { fcp::render::SetHdr(v); });
    r.choices = {{"off", "Off (SDR)"}, {"on", "On"}, {"auto", "Auto (Windows HDR)"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "HDR paper white", Kind::kSlider, When::kLive, "display", "hdr_paper_white",
                        "HDR10: brightness of the game's white, HUD, menus and this menu in nits. Switches at once.",
                        [](const std::string&) { fcp::render::SetHdrLevels(); });
    r.min = 80.0f, r.max = 1000.0f, r.step = 10.0f, r.format = "%.0f nits";
    r.get = [] {  // "auto" (Windows' value) shows as 200 here
      const std::string v = fcp::settings::Value("display", "hdr_paper_white");
      return std::atof(v.c_str()) >= 80.0 ? v : std::string("200");
    };
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "HDR peak brightness", Kind::kChoice, When::kLive, "display", "hdr_peak",
                        "HDR10: the display's peak brightness, the limit of the highlight boost. Switches at once.",
                        [](const std::string&) { fcp::render::SetHdrLevels(); });
    r.choices = {{"0", "As Windows reports"}, {"400", "400 nits"}, {"600", "600 nits"}, {"800", "800 nits"},
                 {"1000", "1000 nits"}, {"1500", "1500 nits"}, {"2000", "2000 nits"}, {"4000", "4000 nits"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(0, "HDR highlight boost", Kind::kSlider, When::kLive, "display", "hdr_boost",
                        "HDR10: stretches the bright parts of the picture towards the peak (1: the game's white, also the "
                        "HUD's, reaches it). 0 = off, the picture as in SDR. Switches at once.",
                        [](const std::string&) { fcp::render::SetHdrLevels(); });
    r.min = 0.0f, r.max = 1.0f, r.step = 0.05f;
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
    Row r = SettingsRow(1, "Render resolution", Kind::kChoice, When::kLive, "graphics", "resolution",
                        "Pixels of the game picture. Scale: from the render scale above. Native: the picture's size in "
                        "the window or on the monitor. A fixed size smaller than that is faster and scaled up. Switches "
                        "with the next frame.",
                        [](const std::string& v) { fcp::render::SetResolution(v); });
    r.choices = {{"scale", "From the render scale"}, {"native", "Native (output size)"}};
    for (const char* s : {"960x540", "1280x720", "1280x800", "1600x900", "1920x1080", "1920x1200", "2560x1080", "2560x1440",
                          "3440x1440", "3840x2160"}) {
      std::string label = s;
      label.replace(label.find('x'), 1, " x ");
      r.choices.push_back({s, label});
    }
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Shadow resolution", Kind::kChoice, When::kRestart, "graphics", "shadow_scale",
                        "Shadow map resolution as a multiple of the console's 1344x1344.");
    r.choices = {{"1", "1x (console)"}, {"1.5", "1.5x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Shadow distance", Kind::kChoice, When::kRestart, "graphics", "shadow_distance",
                        "How far the sun's shadows reach, as a multiple of the console's (about 15 m ahead). The shadow map "
                        "grows with it, so the shadows stay as sharp.");
    r.choices = {{"1", "1x (console)"}, {"1.5", "1.5x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Shadow filter", Kind::kChoice, When::kLive, "graphics", "shadow_filter",
                        "Edges of the shadows. Console: the game's own filter over 2x2 shadow map texels. Soft: an even "
                        "filter over 4x4 texels, smoother edges without stair steps. Switches at once.",
                        [](const std::string& v) { fcp::render::SetShadowFilter(v); });
    r.choices = {{"console", "As the console"}, {"soft", "Soft"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Ambient occlusion", Kind::kChoice, When::kLive, "graphics", "ao",
                        "Darker creases, corners and contact areas on the opaque scene (not on sky, water, particles, the "
                        "HUD or the weapon; fades out with distance). SSAO: 16 samples. GTAO: ground truth AO, a little "
                        "sharper. Costs GPU time. Switches at once.",
                        [](const std::string& v) { fcp::render::SetAo(v); });
    r.choices = {{"off", "Off (console)"}, {"ssao", "SSAO"}, {"gtao", "GTAO"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Detail distance", Kind::kChoice, When::kLive, "graphics", "lod_distance",
                        "How far vegetation, buildings and characters keep their detailed models (and trees stay "
                        "trees instead of sprites), as a multiple of the console's. Switches at once.",
                        [](const std::string& v) { fcp::detail::SetLodDistance(float(std::atof(v.c_str()))); });
    r.choices = {{"0.5", "0.5x"}, {"1", "1x (console)"}, {"1.5", "1.5x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"8", "8x"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "Object view distance", Kind::kChoice, When::kLive, "graphics", "view_distance",
                        "How far vegetation, buildings and characters are drawn, as a multiple of the console's. The "
                        "level's fog still ends the view. Switches at once.",
                        [](const std::string& v) { fcp::detail::SetViewDistance(float(std::atof(v.c_str()))); });
    r.choices = {{"0.5", "0.5x"}, {"1", "1x (console)"}, {"1.5", "1.5x"}, {"2", "2x"}, {"3", "3x"}, {"4", "4x"}, {"8", "8x"}};
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
    Row r = SettingsRow(1, "Picture shape (Hor+/Vert+)", Kind::kChoice, When::kRestart, "graphics", "aspect",
                        "Wider than 16:9 (21:9, 32:9): the same height, more to the sides. Narrower (16:10, 4:3): the "
                        "same width, more above and below. HUD, menus and videos stay 16:9 in the middle. 16:9 = as "
                        "the console (bars).");
    r.choices = {{"auto", "Auto (the window's shape)"}, {"16:9", "16:9 (console)"}, {"16:10", "16:10"}, {"4:3", "4:3"},
                 {"21:9", "21:9"}, {"32:9", "32:9"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(1, "HUD layout", Kind::kChoice, When::kLive, "graphics", "hud_aspect",
                        "With a wider or narrower picture: radar, health, ammunition, hints and compass at the edges of "
                        "the picture or of a 21:9 box (experimental). 16:9 = all of the HUD in the middle band.",
                        [](const std::string& v) { fcp::render::SetHudAspect(v); });
    r.choices = {{"16:9", "16:9 band (as before)"}, {"auto", "Picture edges"}, {"21:9", "21:9 box"}};
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
  {
    Row r = SettingsRow(1, "Colour blind mode", Kind::kChoice, When::kLive, "graphics", "colour_blind",
                        "Shifts the colours a colour blind eye cannot tell apart into ones it can (daltonization), "
                        "on the finished picture. Off = as the console.",
                        [](const std::string& v) { fcp::render::SetColourBlind(v); });
    r.choices = {{"off", "Off (console)"}, {"protanopia", "Protanopia (red)"}, {"deuteranopia", "Deuteranopia (green)"},
                 {"tritanopia", "Tritanopia (blue)"}};
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
  // ---- Controller (the ports and the buttons: DrawController) ----
  {
    Row r = InputRow(kControllerTab, "Dead zone", Kind::kSlider, When::kLive, "pad", "dead_zone",
                     "0 = only the game's own. A stick inside this share of full deflection counts as centred "
                     "(drifting sticks).",
                     [] { return Num(GetLive().dead_zone); });
    r.min = 0.0f, r.max = 0.5f, r.step = 0.01f;
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(kControllerTab, "Vibration", Kind::kSlider, When::kLive, "pad", "rumble",
                     "Vibration strength, 0 = off.", [] { return std::to_string(GetLive().rumble); });
    r.min = 0.0f, r.max = 100.0f, r.step = 5.0f, r.format = "%.0f %%";
    g_rows.push_back(std::move(r));
  }
  {
    Row r = InputRow(kControllerTab, "Controller driver", Kind::kChoice, When::kRestart, "pad", "backend",
                     "SDL: every controller, up to four players. XInput: Xbox pads only. Off: no controllers.",
                     [] { return GetLive().backend; }, true);
    r.choices = {{"sdl", "SDL (all controllers)"}, {"xinput", "XInput"}, {"off", "Off"}};
    g_rows.push_back(std::move(r));
  }
  {
    Row r = SettingsRow(kInputTab, "Button prompts", Kind::kChoice, When::kLive, "input", "button_prompts",
                        "Button symbols in hints and menus. Auto: the device used last (the bound key with keyboard "
                        "and mouse, the controller's own buttons). Xbox: as the console.",
                        [](const std::string&) { fcp::prompts::Reload(); });
    r.choices = {{"auto", "Auto (device in use)"}, {"xbox", "Xbox (console)"}, {"keyboard", "Keyboard and mouse"},
                 {"playstation", "PlayStation"}, {"switch", "Switch"}};
    g_rows.push_back(std::move(r));
  }
  // ---- Game ----
  g_rows.push_back(SettingsRow(kGameTab, "Pause on focus loss", Kind::kBool, When::kLive, "game", "pause_on_focus_loss",
                               "While the window is in the background the game pauses (its own pause menu in a "
                               "level), the sound is muted and the vibration stops.",
                               [](const std::string&) { fcp::pc_input::Reload(); }));
  g_rows.push_back(SettingsRow(kGameTab, "Skip intro videos", Kind::kBool, When::kLive, "game", "skip_intro",
                               "The logo and intro videos at start are not shown. The story's own cutscenes stay. "
                               "Holds from the next start (or the next of these videos)."));
  g_rows.push_back(SettingsRow(kGameTab, "Frame rate display", Kind::kBool, When::kLive, "game", "fps_counter",
                               "Frames per second and frame time in the top left corner."));
  {
    Row r = SettingsRow(kGameTab, "Speech volume", Kind::kSlider, When::kLive, "audio", "speech_volume",
                        "Dialogue, radio and shouts, apart from music and effects. 100 % = as the console.",
                        [](const std::string& v) { fcp::speech::SetVolume(std::atoi(v.c_str())); });
    r.min = 0.0f, r.max = 100.0f, r.step = 5.0f, r.format = "%.0f %%";
    g_rows.push_back(std::move(r));
  }
  g_rows.push_back(SettingsRow(kGameTab, "Audio limiter", Kind::kBool, When::kLive, "audio", "limiter",
                               "Turns the loudest moments (explosions) down for an instant instead of clipping them. "
                               "Below full scale the sound is unchanged. Off = as before.",
                               [](const std::string& v) { SetRuntime("apu.limiter", v == "true" ? "true" : "false"); }));
  {
    Row r = SettingsRow(kGameTab, "Language", Kind::kChoice, When::kRestart, "game", "language",
                        "Text and speech of the game, as the console's system language (the disc has all five). "
                        "Auto: the Windows display language (English when it is none of these). The game reads it at "
                        "start: Apply (restarts the game) below.");
    r.choices = {{"english", "English"}, {"french", "Francais"}, {"german", "Deutsch"},
                 {"spanish", "Espanol"}, {"italian", "Italiano"}, {"auto", "Auto (Windows)"}};
    g_rows.push_back(std::move(r));
  }
  SetInputDefaults();
  if (!g_defaults) {
    for (Row& r : g_rows) r.def = nullptr;  // FCP_MENU_DEFAULTS=0
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

// "Defaults" (footer, one page): every row of the page with a default back to it, the rows the environment decides
// left alone; the Input page also resets the key bindings, the Controller page the controller buttons. Device defaults (Steam Deck, ROG Xbox Ally X) are the
// target where the device has one. Idea after FCI's settings menu (Far Cry Instincts Recomp, src/app/menu.cpp,
// Defaults(), MIT, same author).
bool HasDefaults(int tab) {
  if (!g_defaults) return false;
  for (const Row& r : g_rows)
    if (r.tab == tab && r.def) return true;
  return false;
}

void ResetPage(int tab, const char* why) {
  int changed = 0, same = 0, env = 0, failed = 0;
  bool restart = false;
  for (Row& r : g_rows) {
    if (r.tab != tab || !r.def) continue;
    if (!r.override_note().empty()) {
      ++env;
      continue;
    }
    const std::string d = r.def();
    if (SameValue(r.get(), d)) {
      ++same;
      continue;
    }
    const std::string error = r.set(d);
    FCPLOG("settings menu: defaults: {} = {}{}", r.label, d, error.empty() ? "" : " FAILED: " + error);
    if (!error.empty()) {
      ++failed;
      continue;
    }
    ++changed;
    restart = restart || (r.when == When::kRestart && r.get() != r.start);
  }
  int bindings = 0;
  if (tab == kInputTab) {
    for (int i = 0; i < fcp::pc_input::BindingCount(); ++i) {
      if (fcp::pc_input::BindingKeys(i) == fcp::pc_input::DefaultBindingKeys(i)) continue;
      const std::string e = fcp::pc_input::Store("bindings", fcp::pc_input::BindingName(i),
                                                 "\"" + fcp::pc_input::DefaultBindingKeys(i) + "\"");
      e.empty() ? ++bindings : ++failed;
    }
  }
  // the Controller page: the controller buttons of all 16 Xbox buttons too (as "All controller buttons to
  // defaults"); with FCP_PAD_REMAP=0 there is no remapping to reset
  if (tab == kControllerTab && fcp::pc_input::PadRemapOn()) {
    for (int i = 0; i < fcp::pc_input::PadBindingCount(); ++i)
      if (fcp::pc_input::PadBindingSources(i) != fcp::pc_input::DefaultPadBindingSources(i)) ++bindings;
    if (bindings) {
      const std::string e = fcp::pc_input::ResetPadBindings();
      if (!e.empty()) {
        FCPLOG("settings menu: defaults: controller buttons FAILED: {}", e);
        bindings = 0;
        ++failed;
      }
    }
  }
  FCPLOG("settings menu: defaults for the {} page ({}): {} changed, {} bindings, {} already default, {} set by the "
         "environment, {} failed", kTabs[tab], why, changed, bindings, same, env, failed);
  std::string text = std::format("{} defaults: {} setting{} changed", kTabs[tab], changed, changed == 1 ? "" : "s");
  if (tab == kInputTab) text += std::format(", {} binding{}", bindings, bindings == 1 ? "" : "s");
  if (tab == kControllerTab) text += std::format(", {} controller button{}", bindings, bindings == 1 ? "" : "s");
  if (env) text += std::format(", {} set by the environment kept", env);
  if (failed) text += std::format(", {} FAILED", failed);
  if (restart) text += "; restart required";
  Status(text);
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
    g_pad_capture = -1;
    g_ask = Ask::kNone;   // closing answers a pending question with no
    g_hide_chord = true;  // a held Back/Start does not reach the game after closing
  }
  g_open = open;
  FCPLOG("settings menu: {} ({}) at {:.2f} s, level {:.2f} s, frame {}", open ? "open" : "closed", why, Now(),
         fcp::gametime::LevelSeconds(), fcp::gametime::Frame());
  if (g_pause) fcp::pc_input::SetMenuPause(open);
}

// ---- the pads (every connected one, through the SDK; also while the game polls nothing) ----
// ui: the UI reading, also while a title's keyboard prompt hides the pads from the title (UPSTREAM 381)
uint16_t ReadPads(rex::hid::PadState& merged, bool ui = false) {
  merged = {};
  rex::hid::IInputSystem* input = fcp::pc_input::Input();
  if (input) {
    for (uint32_t u = 0; u < 4; ++u) {
      auto r = ui ? input->GetUIState(u) : input->GetState(u);
      if (!r) continue;
      const rex::hid::PadState& s = r->state;
      merged.buttons |= s.buttons;
      merged.left_trigger = std::max(merged.left_trigger, s.left_trigger);
      merged.right_trigger = std::max(merged.right_trigger, s.right_trigger);
      auto bigger = [](int16_t a, int16_t b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; };
      merged.thumb_lx = bigger(merged.thumb_lx, s.thumb_lx);
      merged.thumb_ly = bigger(merged.thumb_ly, s.thumb_ly);
      merged.extra |= s.extra;  // paddles, misc, touchpad, guide (the Controller page's capture)
    }
  }
  merged.buttons |= g_script_pad.load();
  return merged.buttons;
}

constexpr uint16_t kChord = rex::hid::kPadBack | rex::hid::kPadStart;

// prompt: also Start and the triggers (the on-screen keyboard of a title's text prompt, UPSTREAM 381)
void FeedPads(ImGuiIO& io, const rex::hid::PadState& pad, bool prompt = false) {
  uint16_t b = pad.buttons;
  const bool held = b || pad.left_trigger > 30 || pad.right_trigger > 30 || std::abs(int(pad.thumb_lx)) > 8000 ||
                    std::abs(int(pad.thumb_ly)) > 8000;
  rex::hid::PadState p = pad;
  if (!g_pad_armed) {
    if (held) p = {}, b = 0;
    else g_pad_armed = true;
  }
  if (g_capture >= 0 || g_pad_capture >= 0) p = {}, b = 0;
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
  if (prompt) {
    key(ImGuiKey_GamepadStart, rex::hid::kPadStart);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, p.left_trigger > 30, float(p.left_trigger) / 255.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, p.right_trigger > 30, float(p.right_trigger) / 255.0f);
  }
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

// The help text and, when the row has one, its default ("Default: 45 (Steam Deck OLED default)").
std::string HelpText(const Row& r) {
  std::string help = r.help ? r.help : "";
  if (!r.def) return help;
  const std::string d = r.def();
  const int at = r.kind == Kind::kChoice ? ChoiceIndex(r, d) : -1;
  const std::string shown = at >= 0 ? r.choices[size_t(at)].label : d;
  return help + (help.empty() ? "" : "\n\n") + "Default: " + shown + (r.def_from.empty() ? "" : " (" + DefaultNote(r) + ")");
}

// A grey hint line that wraps at the page's edge (the Controller page's port hint was cut on the Deck at 1280x800).
// FCP_MENU_WRAP_HINTS=0: one line as before.
void HintText(const std::string& s) {
  if (!g_wrap_hints) {
    ImGui::TextDisabled("%s", s.c_str());
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
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
      // the default's choice is marked ("- default", on a device "- Steam Deck OLED default")
      const int def_at = r.def ? ChoiceIndex(r, r.def()) : -1;
      const auto label = [&](int i) {
        return r.choices[size_t(i)].label + (i == def_at ? " - " + DefaultNote(r) : std::string());
      };
      std::string preview = at >= 0 ? label(at) : value;
      // A device's name makes the mark long ("Native (1280 x 800) - Steam Deck OLED default" was cut on the Deck at
      // 1280x800): when the closed field is too narrow for it, it shows "- default"; the list and the help keep the
      // device's name. A long choice can be too wide even then ("Desktop (1280 x 800 @ 90 Hz) - default" was cut on
      // the Deck): the closed field then drops the mark, and a text that still does not fit ends in "...".
      // FCP_MENU_SHORT_MARK=0: always the full mark, never shortened.
      if (at >= 0 && g_short_mark) {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float room = ImGui::CalcItemWidth() - ImGui::GetFrameHeight() - st.FramePadding.x * 2;
        const auto fits = [&](const std::string& s) { return ImGui::CalcTextSize(s.c_str()).x <= room; };
        if (!fits(preview) && at == def_at && !r.def_from.empty()) preview = r.choices[size_t(at)].label + " - default";
        if (!fits(preview) && at == def_at) preview = r.choices[size_t(at)].label;
        if (!fits(preview)) {
          std::string s = preview;
          while (!s.empty() && !fits(s + "...")) {
            while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();  // UTF-8 tail
            if (!s.empty()) s.pop_back();
          }
          while (!s.empty() && s.back() == ' ') s.pop_back();
          preview = s + "...";
        }
      }
      if (ImGui::BeginCombo("##v", preview.c_str())) {
        for (size_t i = 0; i < r.choices.size(); ++i) {
          const bool selected = int(i) == at;
          if (ImGui::Selectable(label(int(i)).c_str(), selected)) Change(r, r.choices[i].value);
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
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && (r.help || r.def))
    ImGui::SetItemTooltip("%s", HelpText(r).c_str());
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
    if (ImGui::IsItemHovered() && (r.help || r.def)) ImGui::SetTooltip("%s", HelpText(r).c_str());
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
      g_pad_capture = -1;
      Status(std::format("waiting for a key for {}", name));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) StoreBinding(i, "none");
    ImGui::SameLine();
    if (ImGui::SmallButton("Default")) StoreBinding(i, fcp::pc_input::DefaultBindingKeys(i));
    ImGui::PopID();
  }
  ImGui::EndTable();
  ImGui::TextDisabled("Controller buttons: see the Controller page.");
}

// ---- Controller page: the ports, the pad settings, the controller buttons per Xbox button ----
// After FCI's CONTROLLER page (Far Cry Instincts Recomp, src/app/menu.cpp DrawController / PollPadCapture, MIT, same
// author): a port list, the general rows, a row per Xbox button with a capture that waits until every button was let
// go, then takes the next one pressed; a controller button is bound once. Rewritten for this menu's table layout.
void PadStatus(const std::string& what, const std::string& error) {
  Status(error.empty() ? what + ": applied and stored" : what + " FAILED: " + error);
}

void StartPadCapture(int target, bool add) {
  g_capture = -1;
  g_pad_capture = target;
  g_pad_capture_add = add;
  g_pad_capture_armed = false;
  g_pad_capture_until = Now() + kPadCaptureSeconds;
  Status(std::format("press a controller button for {} ({}; Escape or {:.0f} s: unchanged)",
                     BindingLabel(fcp::pc_input::PadBindingName(target)), add ? "added" : "in place of the list",
                     kPadCaptureSeconds));
}

// UI thread, every frame while the menu is open: the pads' merged reading (ReadPads)
void PollPadCapture(const rex::hid::PadState& pad) {
  if (g_pad_capture < 0) return;
  const std::string source = fcp::pc_input::PressedPadSource(pad);
  const bool held = !source.empty() || pad.buttons != 0;
  if (Now() > g_pad_capture_until) {
    Status(std::format("binding unchanged (no controller button within {:.0f} s)", kPadCaptureSeconds));
    g_pad_capture = -1;
    return;
  }
  if (!g_pad_capture_armed) {
    g_pad_capture_armed = !held;  // the button that started the capture (A) is let go first
    return;
  }
  if (source.empty()) return;
  const int target = g_pad_capture;
  g_pad_capture = -1;
  g_pad_armed = false;  // the button that bound acts in the menu only once let go
  const std::string name = fcp::pc_input::PadBindingName(target);
  PadStatus(std::format("controller {} {} {}", name, g_pad_capture_add ? "+=" : "=", source),
            fcp::pc_input::SetPadBinding(target, source, g_pad_capture_add));
}

const char* PadTypeName(rex::hid::PadType t) {
  using T = rex::hid::PadType;
  switch (t) {
    case T::kX360: return "Xbox 360";
    case T::kXOne: return "Xbox One / Series";
    case T::kPS3: return "PlayStation 3";
    case T::kPS4: return "PlayStation 4";
    case T::kPS5: return "PlayStation 5";
    case T::kSwitchPro: return "Switch Pro";
    case T::kSwitchJoyCon: return "Joy-Con";
    case T::kStandard: return "generic";
    default: return "";
  }
}

// the SDK's pad list, read at most twice a second
std::vector<rex::hid::IInputSystem::PadListing> g_pads;
double g_pads_at = -1;

void DrawPorts(float em) {
  if (g_pads_at < 0 || Now() - g_pads_at > 0.5) {
    g_pads_at = Now();
    rex::hid::IInputSystem* input = fcp::pc_input::Input();
    g_pads = input ? input->ListPads() : std::vector<rex::hid::IInputSystem::PadListing>{};
  }
  ImGui::SeparatorText("Controllers");
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (ImGui::BeginTable("ports", 2, flags)) {
    ImGui::TableSetupColumn("port", ImGuiTableColumnFlags_WidthFixed, em * 14);
    ImGui::TableSetupColumn("pad", ImGuiTableColumnFlags_WidthStretch);
    const bool off = fcp::pc_input::GetLive().backend == "off";
    auto line = [](const rex::hid::IInputSystem::PadListing& p) {
      const char* type = PadTypeName(p.info.type);
      std::string s = p.name.empty() ? std::string("controller") : p.name;
      s += *type ? std::format(" ({}, {})", type, p.driver) : std::format(" ({})", p.driver);
      return s;
    };
    for (uint32_t port = 0; port < 4; ++port) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::Text("Port %u%s", port + 1, port == 0 ? " (also keyboard and mouse)" : "");
      ImGui::TableNextColumn();
      std::string text;
      for (const auto& p : g_pads)
        if (p.user == port) text += (text.empty() ? "" : " + ") + line(p);
      ImGui::AlignTextToFramePadding();
      if (text.empty()) ImGui::TextDisabled("-");
      else if (off) ImGui::TextDisabled("%s (driver off: the game does not see it)", text.c_str());
      else ImGui::TextUnformatted(text.c_str());
    }
    for (const auto& p : g_pads) {
      if (p.user < 4) continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("No port");
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextDisabled("%s", line(p).c_str());
    }
    ImGui::EndTable();
  }
  HintText("A controller keeps its port until it is unplugged; the next one takes the lowest free port "
           "(XInput pads their XInput number).");
}

void DrawPadBindings(float em) {
  ImGui::SeparatorText("Controller buttons (all ports)");
  if (!fcp::pc_input::PadRemapOn()) {
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "FCP_PAD_REMAP=0 is set: every button acts as itself.");
    return;
  }
  if (g_pad_capture >= 0) {
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Press a controller button for %s (Escape or %.0f s: unchanged)",
                       BindingLabel(fcp::pc_input::PadBindingName(g_pad_capture)).c_str(),
                       std::max(0.0, g_pad_capture_until - Now()));
  }
  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;
  if (ImGui::BeginTable("padbinds", 3, flags)) {
    ImGui::TableSetupColumn("target", ImGuiTableColumnFlags_WidthFixed, em * 14);
    ImGui::TableSetupColumn("sources", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("buttons", ImGuiTableColumnFlags_WidthFixed, em * 15);
    for (int i = 0; i < fcp::pc_input::PadBindingCount(); ++i) {
      ImGui::PushID(1000 + i);
      const std::string name = fcp::pc_input::PadBindingName(i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(BindingLabel(name).c_str());
      ImGui::TableNextColumn();
      ImGui::AlignTextToFramePadding();
      const std::string sources = fcp::pc_input::PadBindingSources(i);
      std::string shown = sources;
      for (char& c : shown) c = char(std::toupper(static_cast<unsigned char>(c)));
      if (g_pad_capture == i) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "...");
      else if (sources == fcp::pc_input::DefaultPadBindingSources(i)) ImGui::TextUnformatted(shown.c_str());
      else ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", shown.c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Change")) StartPadCapture(i, false);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetItemTooltip("Waits for a controller button: it acts as this button (and leaves any other).");
      ImGui::SameLine();
      if (ImGui::SmallButton("Add")) StartPadCapture(i, true);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetItemTooltip("Waits for a further controller button for this button.");
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear")) PadStatus("controller " + name + " = none", fcp::pc_input::ClearPadBinding(i));
      ImGui::SameLine();
      if (ImGui::SmallButton("Default"))
        PadStatus("controller " + name + " = default", fcp::pc_input::DefaultPadBinding(i));
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  if (ImGui::Button("All controller buttons to defaults"))
    PadStatus("controller buttons = defaults", fcp::pc_input::ResetPadBindings());
  HintText("Blue: changed. The menu itself always uses the controller's own buttons.");
  ImGui::TextWrapped("Buttons: %s.", fcp::pc_input::PadSourceList().c_str());
  if (!fcp::pc_input::PadExtraBindable())
    HintText("FCP_PAD_EXTRA=0: paddles, misc and touchpad act as before (paddles on A/B/X/Y).");
}

void DrawController(float em) {
  DrawPorts(em);
  ImGui::SeparatorText("General");
  DrawRows(kControllerTab, em);
  DrawPadBindings(em);
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

// ---- Exit Game and Apply (restarts the game): ask first ----
// Exit Game as in FCI's settings menu (Far Cry Instincts Recomp, src/app/menu.cpp, MIT, same author): a button at
// the left of the footer, a confirmation in place of the panel, A / Enter answers, B / Escape goes back. The end is
// the window's close button (the SDK's shutdown: sound off, the window closes, exit code 0). The restart has no FCI
// counterpart (FCI marks a restart only): in a level the game's own "ReturnToMenu" (as its pause menu's quit: the
// game writes its launch data and relaunches, back to the main menu with the profile), else a fresh start.
int g_ask_focus = 0;          // the dialog's first button takes the focus in that many frames
int g_focus_back = 0;         // back from the dialog: the button that opened it takes the focus in that many frames
Ask g_focus_back_to = Ask::kNone;
double g_restart_queued = -1;  // ReturnToMenu queued at (wall seconds); a host restart follows when it did nothing
constexpr double kRestartFallback = 10.0;

// a restart-required setting differs from the value at start (the environment does not set it)
bool RestartPending() {
  for (const Row& r : g_rows)
    if (r.when == When::kRestart && r.override_note().empty() && r.get() != r.start) return true;
  return false;
}

void ExitGame(const char* why) {
  FCPLOG("settings menu: exit game ({}): the game ends as by the window's close button (level {}, {:.2f} s)", why,
         fcp::gametime::InLevel() ? fcp::gametime::LevelName() : std::string("none"), Now());
  g_ask = Ask::kNone;
  rex::app::RequestShutdown(0);
}

// the profile manager's current profile is there and valid (runtime_addresses.toml profile_manager_ptr: +0x14
// current profile, +1 valid; selected with Press Start)
bool ProfileSelected() {
  uint8_t* base = fcp::watch::GuestBase();
  if (!base) return false;
  const uint32_t manager = fcp::guest::U32(base, fcp::addr::profile_manager_ptr);
  if (!manager || !fcp::guest::Readable(base, manager, 0x18)) return false;
  const uint32_t profile = fcp::guest::U32(base, manager + 0x14);
  if (!profile || !fcp::guest::Readable(base, profile, 2)) return false;
  return *fcp::guest::Ptr(base, profile + 1) != 0;
}

void RestartGame(const char* why) {
  g_ask = Ask::kNone;
  if (fcp::relaunch::Active() || g_restart_queued >= 0) {
    Status("a restart is already running");
    return;
  }
  const bool level = fcp::gametime::InLevel();
  const std::string mode = Lower(fcp::env::Get("FCP_MENU_RESTART"));  // game (default) | host
  // ReturnToMenu needs a profile: on the title screen ("Press Start", no profile yet) it crashed in the game code
  // (measured), there a fresh start is the same thing anyway
  const bool profile = ProfileSelected();
  const bool host = mode == "host" || !profile;
  FCPLOG("settings menu: restart ({}) in {}, {}", why, level ? "level " + fcp::gametime::LevelName() : "the menus",
         mode == "host" ? "host restart (FCP_MENU_RESTART=host)"
         : host         ? "host restart (no profile selected yet)"
                        : "the game's ReturnToMenu");
  SetOpen(false, "restart");
  if (host) {
    fcp::relaunch::RestartNow("settings menu");
    return;
  }
  fcp::devstart::QueueCommand("ReturnToMenu");
  g_restart_queued = Now();
}

// UI thread, once a frame: ReturnToMenu that did not relaunch within kRestartFallback seconds -> a host restart
void CheckRestart() {
  if (g_restart_queued < 0 || fcp::relaunch::Active()) return;
  if (Now() - g_restart_queued < kRestartFallback) return;
  g_restart_queued = -1;
  FCPLOG("settings menu: ReturnToMenu did not relaunch within {:.0f} s: host restart", kRestartFallback);
  fcp::relaunch::RestartNow("settings menu, fallback");
}

void AskFor(Ask what, const char* why) {
  if (what == Ask::kRestart && !fcp::gametime::InLevel()) {
    RestartGame(why);  // no progress to lose in the menus: at once
    return;
  }
  g_ask = what;
  g_ask_focus = 2;
  FCPLOG("settings menu: {} ({}), asking to confirm", what == Ask::kExit ? "exit game" : "restart", why);
}

void CancelAsk(const char* why) {
  if (g_ask == Ask::kNone) return;
  FCPLOG("settings menu: {} cancelled ({})", g_ask == Ask::kExit ? "exit game" : "restart", why);
  g_focus_back_to = g_ask;
  g_focus_back = 2;
  g_ask = Ask::kNone;
}

void ConfirmAsk(const char* why) {
  const Ask a = g_ask;
  if (a == Ask::kExit) ExitGame(why);
  else if (a == Ask::kRestart) RestartGame(why);
}

void DrawAsk(const ImGuiIO& io, float em) {
  const bool exit = g_ask == Ask::kExit;
  const float w = std::min(em * 30, io.DisplaySize.x - 16);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
  ImGui::SetNextWindowFocus();
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin(exit ? "Exit to desktop?###fcp_ask" : "Restart the game?###fcp_ask", nullptr, flags)) {
    ImGui::PushTextWrapPos(w - em);
    if (exit) {
      ImGui::TextUnformatted("The game ends. Progress since the last checkpoint is lost; the settings are kept.");
    } else {
      ImGui::TextUnformatted("The game restarts with the changed settings and goes back to the main menu. Progress "
                             "since the last checkpoint is lost; the settings are kept.");
    }
    ImGui::TextDisabled(exit ? "A or Enter exits, B or Escape goes back." : "A or Enter restarts, B or Escape goes back.");
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, em * 0.4f));
    const float bw = em * 7;
    const float pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(pad, 0.0f));
    if (g_ask_focus > 0 && --g_ask_focus == 0) ImGui::SetKeyboardFocusHere();
    bool yes = ImGui::Button(exit ? "Exit" : "Restart", ImVec2(bw, 0));
    ImGui::SameLine();
    bool no = ImGui::Button("Cancel", ImVec2(bw, 0));
    const bool no_focused = ImGui::IsItemFocused();
    // A and Enter wherever the navigation stands: yes, unless Cancel has the focus (B and Escape go back)
    for (ImGuiKey key : {ImGuiKey_GamepadFaceDown, ImGuiKey_Enter, ImGuiKey_KeypadEnter})
      if (ImGui::IsKeyPressed(key, false)) (no_focused ? no : yes) = true;
    if (no) CancelAsk("button");
    else if (yes) ConfirmAsk("button");
  }
  ImGui::End();
}
// ---- end Exit Game and Apply ----

void DrawMenu(ImGuiIO& io) {
  const float px = FontPx(io);
  const float em = px;
  {
    // DPI check in the log: layout in points, font rasterized at the framebuffer scale (rexglue change 351)
    static float last_w = 0, last_h = 0, last_scale = 0;
    if (io.DisplaySize.x != last_w || io.DisplaySize.y != last_h || io.DisplayFramebufferScale.x != last_scale) {
      last_w = io.DisplaySize.x, last_h = io.DisplaySize.y, last_scale = io.DisplayFramebufferScale.x;
      FCPLOG("settings menu: display {:.0f}x{:.0f} points, framebuffer scale {:.2f}, font {:.1f} points = {:.1f} pixels",
             last_w, last_h, last_scale, px, px * last_scale);
    }
  }
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
  const bool asking = g_ask != Ask::kNone;  // the confirmation in place of the panel this frame
  if (asking) {
    DrawAsk(io, em);
  } else if (ImGui::Begin("Settings###fcp_settings", &keep, flags)) {
    const float footer = ImGui::GetFrameHeightWithSpacing() * 3.3f;
    if (ImGui::BeginTabBar("tabs")) {
      for (int t = 0; t < kTabCount; ++t) {
        const ImGuiTabItemFlags tf = g_select_tab == t ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem(kTabs[t], nullptr, tf)) {
          g_page = t;
          ImGui::BeginChild("page", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
          if (t == kControllerTab) DrawController(em);
          else if (t < kAboutTab) DrawRows(t, em);
          if (t == kInputTab) DrawBindings(em);
          if (t == kAboutTab) DrawAbout();
          // a line of room below the last line: scrolled to the end, the page's last text line (the Controller
          // page's button names, the Input page's note) was cut off at the bottom edge (seen in staging-v030)
          ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeightWithSpacing()));
          if (g_scroll_to >= 0) {
            ImGui::SetScrollY(g_scroll_to * ImGui::GetScrollMaxY());
            g_scroll_to = -1;
          }
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
    const bool pending = RestartPending();
    if (pending) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Changed settings take effect after a restart.");
    else ImGui::TextDisabled("F10 / Escape / Back+Start / B: close");
    // Exit Game at the left (as FCI), Apply while a change waits for a restart, Close at the right
    const bool focus_back = g_focus_back > 0 && --g_focus_back == 0;
    if (focus_back && g_focus_back_to == Ask::kExit) ImGui::SetKeyboardFocusHere();
    if (ImGui::Button("Exit Game", ImVec2(em * 6, 0))) AskFor(Ask::kExit, "button");
    if (pending) {
      ImGui::SameLine();
      if (focus_back && g_focus_back_to == Ask::kRestart) ImGui::SetKeyboardFocusHere();
      if (ImGui::Button("Apply (restarts the game)")) AskFor(Ask::kRestart, "button");
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetItemTooltip(fcp::gametime::InLevel()
                                  ? "Restarts the game now, back to the main menu (asks first: progress since the last "
                                    "checkpoint is lost)."
                                  : "Restarts the game now; the window and display mode stay.");
    }
    // Defaults (this page only) left of Close; not on About
    const float right = ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX();
    if (HasDefaults(g_page)) {
      ImGui::SameLine(right - em * 11 - ImGui::GetStyle().ItemSpacing.x);
      if (ImGui::Button("Defaults", ImVec2(em * 6, 0))) ResetPage(g_page, "button");
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetItemTooltip("Sets the settings of this page (%s) back to their defaults%s. Other pages stay.",
                              kTabs[g_page], g_page == kInputTab        ? ", the key bindings too"
                                             : g_page == kControllerTab ? ", the controller buttons too"
                                                                        : "");
    }
    ImGui::SameLine(right - em * 5);
    if (ImGui::Button("Close", ImVec2(em * 5, 0))) keep = false;
  }
  if (!asking) ImGui::End();
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
std::mutex g_test_mutex;
std::vector<std::string> g_test_actions;  // TestActions: from other threads, taken over by RunScript

void PostKey(const std::string& name) {
  if (!g_ui) return;
  auto native = g_ui->GetNativeWindow();
  HWND hwnd = native ? static_cast<HWND>(native->window) : nullptr;
  const std::string n = Lower(name);
  const int vk = n == "f10" ? VK_F10 : n == "f11" ? VK_F11 : n == "escape" ? VK_ESCAPE : n == "enter" ? VK_RETURN : n == "up" ? VK_UP
                 : n == "down" ? VK_DOWN : n == "left" ? VK_LEFT : n == "right" ? VK_RIGHT : n == "space" ? VK_SPACE
                 : n == "backspace" ? VK_BACK : n == "tab" ? VK_TAB : 0;
  if (!hwnd || !vk) {
    FCPLOG("FCP_TEST_MENU: post={}: {}", name, hwnd ? "unknown key" : "no window");
    return;
  }
  const LPARAM scan = LPARAM(MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC)) << 16;
  FCPLOG("FCP_TEST_MENU: post {} (foreground: {})", name, GetForegroundWindow() == hwnd ? "yes" : "no");
  PostMessageW(hwnd, vk == VK_F10 ? WM_SYSKEYDOWN : WM_KEYDOWN, WPARAM(vk), 1 | scan);
  PostMessageW(hwnd, vk == VK_F10 ? WM_SYSKEYUP : WM_KEYUP, WPARAM(vk), 1 | scan | (LPARAM(1) << 30) | (LPARAM(1) << 31));
}

// type=<text>: characters as the window gets them from a keyboard (WM_CHAR; SDL makes text input events of them
// while text input is on, i.e. while an ImGui text field has the focus)
void PostText(const std::string& text) {
  if (!g_ui) return;
  auto native = g_ui->GetNativeWindow();
  HWND hwnd = native ? static_cast<HWND>(native->window) : nullptr;
  if (!hwnd) {
    FCPLOG("FCP_TEST_MENU: type={}: no window", text);
    return;
  }
  const int wn = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
  std::wstring wide(size_t(wn), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), wn);
  FCPLOG("FCP_TEST_MENU: type \"{}\" ({} characters)", text, wide.size());
  for (wchar_t c : wide) PostMessageW(hwnd, WM_CHAR, WPARAM(c), 1);
}

uint16_t PadBits(const std::string& list) {
  static constexpr struct {
    const char* name;
    uint16_t bits;
  } kPad[] = {{"a", rex::hid::kPadA},           {"b", rex::hid::kPadB},          {"x", rex::hid::kPadX},
              {"y", rex::hid::kPadY},           {"back", rex::hid::kPadBack},    {"start", rex::hid::kPadStart},
              {"up", rex::hid::kPadDPadUp},     {"down", rex::hid::kPadDPadDown}, {"left", rex::hid::kPadDPadLeft},
              {"right", rex::hid::kPadDPadRight}, {"lb", rex::hid::kPadLeftShoulder},
              {"rb", rex::hid::kPadRightShoulder}, {"combo", kChord}};
  uint16_t bits = 0;
  std::stringstream ss(list);
  for (std::string item; std::getline(ss, item, '+');)
    for (const auto& p : kPad)
      if (Lower(Trim(item)) == p.name) bits = uint16_t(bits | p.bits);
  return bits;
}

void RunScript() {
  const double now = Now();
  {
    std::lock_guard lock(g_test_mutex);
    for (const std::string& a : g_test_actions) g_script.push_back({now, a});
    g_test_actions.clear();
  }
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
    else if (verb == "scroll") g_scroll_to = std::clamp(float(std::atof(arg.c_str())), 0.0f, 1.0f);
    else if (verb == "tab") {
      for (int t = 0; t < kTabCount; ++t)
        if (Lower(kTabs[t]) == Lower(arg)) g_select_tab = t;
    } else if (verb == "defaults") {
      // as a click on Defaults: the page shown, or defaults=<tab>
      int tab = g_page;
      for (int t = 0; t < kTabCount; ++t)
        if (!arg.empty() && Lower(kTabs[t]) == Lower(arg)) tab = t;
      if (HasDefaults(tab)) ResetPage(tab, "FCP_TEST_MENU");
      else FCPLOG("FCP_TEST_MENU: the {} page has no defaults", kTabs[tab]);
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
    } else if (verb == "exit" || verb == "apply") {
      // as a click on Exit Game / Apply (restarts the game): asks first (Apply in the menus restarts at once)
      AskFor(verb == "exit" ? Ask::kExit : Ask::kRestart, "FCP_TEST_MENU");
    } else if (verb == "yes") {
      ConfirmAsk("FCP_TEST_MENU");
    } else if (verb == "no") {
      CancelAsk("FCP_TEST_MENU");
    } else if (verb == "quit") {
      ExitGame("FCP_TEST_MENU quit");
    } else if (verb == "restart") {
      RestartGame("FCP_TEST_MENU restart");
    } else if (verb == "post") {
      PostKey(arg);
    } else if (verb == "type") {
      PostText(arg);
    } else if (verb == "raise") {
      // the window to the foreground: SDL drops key and text events while it has no keyboard focus
      if (g_ui) g_ui->RaiseWindow();
      FCPLOG("FCP_TEST_MENU: raise (focused: {})", g_ui && g_ui->IsFocused() ? "yes" : "no");
    } else if (verb == "imtype") {
      // characters straight into ImGui (inside its frame), without the window and SDL
      ImGuiIO& io = ImGui::GetIO();
      io.AddInputCharactersUTF8(arg.c_str());
      FCPLOG("FCP_TEST_MENU: imtype \"{}\"", arg);
    } else if (verb == "imkey") {
      const std::string n = Lower(arg);
      const ImGuiKey k = n == "enter" ? ImGuiKey_Enter : n == "escape" ? ImGuiKey_Escape
                         : n == "backspace" ? ImGuiKey_Backspace : ImGuiKey_None;
      if (k == ImGuiKey_None) {
        FCPLOG("FCP_TEST_MENU: imkey={}: unknown key", arg);
      } else {
        ImGui::GetIO().AddKeyEvent(k, true);
        g_script.push_back({now + 0.1, "imkeyup=" + n});
        FCPLOG("FCP_TEST_MENU: imkey {}", n);
      }
    } else if (verb == "imkeyup") {
      const ImGuiKey k = arg == "enter" ? ImGuiKey_Enter : arg == "escape" ? ImGuiKey_Escape : ImGuiKey_Backspace;
      ImGui::GetIO().AddKeyEvent(k, false);
    } else if (verb == "pad") {
      const uint16_t bits = PadBits(arg);
      g_script_pad = uint16_t(g_script_pad.load() | bits);
      g_script.push_back({now + 0.2, "padup=" + arg});
    } else if (verb == "padup") {
      g_script_pad = uint16_t(g_script_pad.load() & ~PadBits(arg));
    } else if (verb == "padbind" || verb == "padadd" || verb == "padclear" || verb == "paddefault" ||
               verb == "padcapture" || verb == "padcaptureadd") {
      // the Controller page's buttons: <target>[:<source>] (target and source as pad_bind_ names)
      const size_t colon = arg.find(':');
      const std::string name = Lower(Trim(arg.substr(0, colon)));
      const std::string source = colon == std::string::npos ? "" : Lower(Trim(arg.substr(colon + 1)));
      int target = -1;
      for (int t = 0; t < fcp::pc_input::PadBindingCount(); ++t)
        if (fcp::pc_input::PadBindingName(t) == name) target = t;
      if (target < 0) {
        FCPLOG("FCP_TEST_MENU: '{}': unknown controller button '{}'", w, name);
      } else if (verb == "padbind" || verb == "padadd") {
        PadStatus(std::format("controller {} {} {}", name, verb == "padadd" ? "+=" : "=", source),
                  fcp::pc_input::SetPadBinding(target, source, verb == "padadd"));
      } else if (verb == "padclear") {
        PadStatus("controller " + name + " = none", fcp::pc_input::ClearPadBinding(target));
      } else if (verb == "paddefault") {
        PadStatus("controller " + name + " = default", fcp::pc_input::DefaultPadBinding(target));
      } else {
        StartPadCapture(target, verb == "padcaptureadd");
      }
    } else if (verb == "padreset") {
      PadStatus("controller buttons = defaults", fcp::pc_input::ResetPadBindings());
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
    // <t>@<g>:<action>: only in relaunch generation g (with FCP_TEST_MENU_GENS listing it, e.g. all)
    if (const size_t at = item.find('@'); at < colon &&
                                          std::atoi(item.c_str() + at + 1) != fcp::relaunch::Generation()) {
      continue;
    }
    g_script.push_back({std::atof(item.c_str()), item.substr(colon + 1)});
  }
  FCPLOG("FCP_TEST_MENU: {} actions", g_script.size());
}

void LoadFont(ImGuiIO& io);

// ---- a title's keyboard prompt (XamShowKeyboardUI, e.g. the cheat code): the pads drive its on-screen keyboard ----
// The SDK hides the pads from the title while the prompt is open; the UI reading (GetUIState) still sees them and
// goes to ImGui as gamepad keys, which the prompt reads itself (UPSTREAM 381). FCP_KEYBOARD_UI=0: off (as before).
bool g_prompt_fed = false;
void FeedPrompt(ImGuiIO& io) {
  static const bool enabled = fcp::env::Flag("FCP_KEYBOARD_UI", true);
  const bool prompt = enabled && !g_open.load() &&
                      ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
  if (prompt != g_prompt_fed) {
    g_prompt_fed = prompt;
    g_pad_armed = false;  // the button that opened the prompt (or closed it) acts only once let go
    if (!prompt) FeedPads(io, rex::hid::PadState{}, true);  // let go of every key
    FCPLOG("keyboard prompt: {} at {:.2f} s, the pads {}", prompt ? "open" : "closed", Now(),
           prompt ? "drive its on-screen keyboard" : "are the game's again");
  }
  if (!prompt) return;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  rex::hid::PadState pad;
  ReadPads(pad, true);
  FeedPads(io, pad, true);
}

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
    CheckRestart();
    rex::hid::PadState pad;
    const uint16_t buttons = ReadPads(pad);
    // Back + Start: open or close (on the press of the chord)
    if ((buttons & kChord) == kChord && (g_pad_prev & kChord) != kChord) {
      g_hide_chord = true;
      SetOpen(!g_open.load(), "Back+Start");
      g_pad_armed = false;
    }
    if (g_open.load()) {
      const bool capturing = g_pad_capture >= 0;
      PollPadCapture(pad);
      FeedPads(io, pad);
      // B closes, as Escape does (not while a binding waits for a key or a controller button)
      if (g_pad_armed && g_capture < 0 && !capturing && g_pad_capture < 0 && (buttons & rex::hid::kPadB) &&
          !(g_pad_prev & rex::hid::kPadB) &&
          !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        if (g_ask != Ask::kNone) CancelAsk("B");  // Exit Game's / Apply's question: back to the settings
        else SetOpen(false, "B");
      }
    }
    FeedPrompt(io);
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
    if (g_pad_capture >= 0 && g_open.load() && e.type == T::kKeyDown && !e.repeat &&
        e.key == rex::VirtualKey::kEscape) {
      e.handled = true;
      Status("binding unchanged");
      g_pad_capture = -1;
      return;
    }
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
      if (g_ask != Ask::kNone) CancelAsk("Escape");
      else SetOpen(false, "Escape");
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
  g_defaults = fcp::env::Flag("FCP_MENU_DEFAULTS", true);
  g_short_mark = fcp::env::Flag("FCP_MENU_SHORT_MARK", true);
  g_wrap_hints = fcp::env::Flag("FCP_MENU_WRAP_HINTS", true);
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

void TestActions(const std::string& actions) {
  std::lock_guard lock(g_test_mutex);
  std::stringstream ss(actions);
  for (std::string item; std::getline(ss, item, ',');)
    if (!Trim(item).empty()) g_test_actions.push_back(Trim(item));
}

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
