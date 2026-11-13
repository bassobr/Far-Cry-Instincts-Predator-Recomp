// predator.toml: the player's settings (fcp/user_settings.h). Own code of this project; the ideas (every key also
// an environment switch, the log shows the file and every value, write over a .tmp file and MoveFileEx, tolerate a
// UTF-8 BOM, Alt+Enter) come from FCI/FCE (Far Cry Instincts and Evolution Recomp, same author).
#include "fcp/user_settings.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/input_keys.h>
#include <rex/settings_registry.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "fcp/device_preset.h"
#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/log.h"
#include "fcp/pc_input.h"
#include "fcp/relaunch.h"
#include "fcp/settings_menu.h"
#include "fcp/toml_edit.h"
#include "fcp/user_paths.h"

namespace fcp::settings {
namespace {

enum class Kind { kMode, kSize, kMonitor, kFps, kOnOffAuto, kBackend, kBool, kPath, kFactor, kAniso, kLanguage, kPrompts, kMsaa };

struct Key {
  const char* section;
  const char* key;
  const char* env;
  const char* def;  // as written into a new file
  Kind kind;
  const char* comment;
};

constexpr Key kKeys[] = {
    {"game", "image", "FCP_GAME_ISO", "\"\"", Kind::kPath,
     "The game disc image (.iso) or extracted folder; empty = FCP_GAME_ISO or title.game_root of rexglue.toml.\n"
     "# A relative path is relative to this folder."},
    {"game", "pause_on_focus_loss", "FCP_PAUSE_ON_FOCUS_LOSS", "false", Kind::kBool,
     "true = while the window has no focus the game pauses (its own pause menu in a level, as when the console's\n"
     "# guide opens), the sound is muted and the vibration stops; back in the window the game stays in its pause\n"
     "# menu. false = runs on (as before)."},
    {"game", "skip_intro", "FCP_SKIP_INTRO", "false", Kind::kBool,
     "true = the logo and intro videos at start (Ubisoft, the Far Cry intro) are not shown. The story's own\n"
     "# cutscenes stay as on the console."},
    {"game", "fps_counter", "FCP_FPS_COUNTER", "false", Kind::kBool,
     "true = a small frame rate and frame time display in the corner."},
    {"game", "language", "FCP_LANGUAGE", "\"english\"", Kind::kLanguage,
     "Text and speech language of the game, as the console's system language: english, french, german, spanish,\n"
     "# italian, or auto (the Windows display language; English when it is none of these). Needs a restart."},
    {"display", "mode", "FCP_DISPLAY_MODE", "\"windowed\"", Kind::kMode,
     "windowed | borderless (fullscreen window on the monitor) | exclusive (fullscreen video mode).\n"
     "# Alt+Enter switches between the window and fullscreen at once and stores the choice here."},
    {"display", "width", "FCP_WINDOW_WIDTH", "0", Kind::kSize,
     "Window client size in pixels; 0 = 1280x720. The game itself renders at 1280x720, as on the console."},
    {"display", "height", "FCP_WINDOW_HEIGHT", "0", Kind::kSize, nullptr},
    {"display", "monitor", "FCP_MONITOR", "0", Kind::kMonitor,
     "Monitor: 0 = the system's choice (primary), 1..n = display number."},
    {"display", "fps", "FCP_FPS", "30", Kind::kFps,
     "Frame rate: 30 (as the console), 60, 90 (Steam Deck OLED display) or 120."},
    {"display", "vsync", "FCP_RENDER_VSYNC", "\"auto\"", Kind::kOnOffAuto,
     "Wait for the monitor when presenting: auto (on up to 60 frames/s, off above), on, off."},
    {"graphics", "backend", "FCP_RENDER_BACKEND", "\"auto\"", Kind::kBackend,
     "Graphics API: auto (renderer.toml next to the EXE, else Vulkan), vulkan, d3d12."},
    {"graphics", "msaa", "FCP_RENDER_MSAA", "\"console\"", Kind::kMsaa,
     "Multisampling: console (as the console: 2x on the screen, 4x on the reflection), off, or 2, 4, 8 samples on\n"
     "# the screen-sized targets (limited to what the GPU can do; the rest stays as the console). Needs a restart."},
    {"graphics", "smaa", "FCP_RENDER_SMAA", "false", Kind::kBool,
     "SMAA 1x edge smoothing on the finished picture (false = as the console). Switches at once."},
    {"graphics", "gamma", "FCP_RENDER_GAMMA", "true", Kind::kBool,
     "The game's gamma ramp (brightness setting of the game; true = as the console)."},
    {"graphics", "scale", "FCP_RENDER_SCALE", "1", Kind::kFactor,
     "Render resolution of the screen-sized targets: 1 = as the console (1280x720), auto = the output's height\n"
     "# (e.g. 2 at 1440p), or a factor / percentage (1.5, 150%; 0.5 to 4). HUD and text get sharper with it."},
    {"graphics", "shadow_scale", "FCP_RENDER_SHADOW_SCALE", "1", Kind::kFactor,
     "Shadow map resolution as a multiple of the console's (1344x1344; 1 = as the console, up to 4)."},
    {"graphics", "aniso", "FCP_RENDER_ANISO", "1", Kind::kAniso,
     "Anisotropic texture filtering 1-16 (1 = as the game sets it)."},
    {"graphics", "fov", "FCP_RENDER_FOV", "1", Kind::kFactor,
     "Field of view as a multiple of the game's (1 = as the console; 0.5 to 2). The weapon keeps its own view."},
    {"graphics", "aspect", "FCP_RENDER_ASPECT", "\"auto\"", Kind::kFactor,
     "Shape of the game view: auto = the output's when wider than 16:9 (21:9, 32:9: the same height, more to the\n"
     "# sides; HUD, menus and videos stay 16:9 in the middle), \"16:9\" = as the console (bars at the sides), or a\n"
     "# fixed shape (\"21:9\", \"32:9\", 2.39). Narrower outputs (16:10, 4:3) show 16:9 with bars. Needs a restart."},
    {"input", "file", "FCP_INPUT_CONFIG", "\"predator_input.toml\"", Kind::kPath,
     "Keyboard and mouse bindings (relative to this folder); written with the defaults when missing."},
    {"input", "button_prompts", "FCP_BUTTON_PROMPTS", "\"auto\"", Kind::kPrompts,
     "Button symbols in hints and menus: auto (the device used last: the bound key with keyboard and mouse, the\n"
     "# controller's own buttons), xbox (as the console), keyboard, playstation, switch."},
};

std::filesystem::path g_file;  // empty: no file
std::set<std::string, std::less<>> g_from_file;
std::mutex g_write_mutex;
std::map<std::string, std::string, std::less<>> g_values;  // "section.key" -> text as in the file (or the default)
std::set<std::string, std::less<>> g_env_wins;             // "section.key" set from outside the file

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Trim(std::string_view s) {
  const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string_view::npos ? std::string() : std::string(s.substr(b, e - b + 1));
}

std::string U8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

std::filesystem::path FromU8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

std::string StripComment(const std::string& line) {
  char quote = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    if ((line[i] == '"' || line[i] == '\'') && (quote == 0 || quote == line[i])) quote = quote ? 0 : line[i];
    if (line[i] == '#' && !quote) return line.substr(0, i);
  }
  return line;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
  }
  if (!lines.empty() && lines[0].starts_with("\xEF\xBB\xBF")) lines[0].erase(0, 3);
  return lines;
}

// [section] key = value -> "section.key" -> value without quotes
std::map<std::string, std::string> Parse(const std::filesystem::path& path) {
  std::map<std::string, std::string> out;
  std::string section;
  int number = 0;
  for (const std::string& raw : ReadLines(path)) {
    ++number;
    const std::string t = Trim(StripComment(raw));
    if (t.empty()) continue;
    if (t.front() == '[' && t.back() == ']') {
      section = Lower(Trim(std::string_view(t).substr(1, t.size() - 2)));
      continue;
    }
    const size_t eq = t.find('=');
    if (eq == std::string::npos) {
      FCPLOG("settings: {}:{}: no '=', line ignored", U8(path.filename()), number);
      continue;
    }
    std::string value = Trim(std::string_view(t).substr(eq + 1));
    // "text" or 'text', taken as written (a backslash in a Windows path is no escape here)
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') && value.back() == value.front()) {
      value = value.substr(1, value.size() - 2);
    }
    out[section + "." + Lower(Trim(std::string_view(t).substr(0, eq)))] = value;
  }
  return out;
}

std::string DefaultFile() {
  std::string s =
      "# Far Cry Instincts Predator (PC recompilation): settings.\n"
      "# Written with the defaults when missing. Edit and restart the game. Every key is also an environment\n"
      "# variable (name in brackets); a variable set in the environment wins over this file.\n";
  const char* section = "";
  for (const Key& k : kKeys) {
    if (std::string_view(section) != k.section) {
      section = k.section;
      s += std::format("\n[{}]\n", section);
    }
    if (k.comment) s += std::format("# {}\n", k.comment);
    // a device default (fcp/device_preset.h) replaces the built-in one; the comment names the device
    if (const std::string d = fcp::device::Default(k.env); !d.empty()) {
      const std::string text = k.def[0] == '"' ? std::format("\"{}\"", d) : d;
      s += std::format("{} = {}  # [{}] {} default (elsewhere {})\n", k.key, text, k.env, fcp::device::Name(), k.def);
      continue;
    }
    s += std::format("{} = {}  # [{}]\n", k.key, k.def, k.env);
  }
  return s;
}

bool ParseBoolText(std::string v, bool& out) {
  v = Lower(v);
  if (v == "true" || v == "1" || v == "on" || v == "yes") return out = true, true;
  if (v == "false" || v == "0" || v == "off" || v == "no") return out = false, true;
  return false;
}

bool ParseInt(const std::string& v, int lo, int hi, int& out) {
  char* end = nullptr;
  const long n = std::strtol(v.c_str(), &end, 10);
  if (!end || end == v.c_str() || *end || n < lo || n > hi) return false;
  out = int(n);
  return true;
}

// File text -> environment value; "" = leave the module's default; false = invalid.
bool Normalize(const Key& k, const std::string& text, std::string& env) {
  const std::string v = Lower(Trim(text));
  int n = 0;
  bool b = false;
  switch (k.kind) {
    case Kind::kMode:
      if (v == "windowed" || v == "window") return env = "windowed", true;
      if (v == "borderless" || v == "fullscreen") return env = "borderless", true;
      if (v == "exclusive") return env = "exclusive", true;
      return false;
    case Kind::kSize:
      if (!ParseInt(v, 0, 16384, n)) return false;
      return env = n > 0 ? std::to_string(n) : "", true;
    case Kind::kMonitor:
      if (!ParseInt(v, 0, 16, n)) return false;
      return env = n > 0 ? std::to_string(n) : "", true;
    case Kind::kFps:
      // 90: the Steam Deck OLED's refresh rate
      if (v != "30" && v != "60" && v != "90" && v != "120") return false;
      return env = v, true;
    case Kind::kOnOffAuto:
      if (v == "auto" || v.empty()) return env = "", true;
      if (!ParseBoolText(v, b)) return false;
      return env = b ? "1" : "0", true;
    case Kind::kBackend:
      if (v == "auto" || v.empty()) return env = "", true;
      if (v == "vulkan" || v == "d3d12") return env = v, true;
      return false;
    case Kind::kBool:
      if (!ParseBoolText(v, b)) return false;
      return env = b ? "1" : "0", true;
    case Kind::kFactor:
      // checked by the renderer (log line there): 1, auto, 1.5, 150%
      if (v.empty()) return env = "", true;
      return env = v, true;
    case Kind::kAniso:
      if (!ParseInt(v, 1, 16, n)) return false;
      return env = std::to_string(n), true;
    case Kind::kLanguage:
      for (const auto& [name, code] : {std::pair{"auto", "auto"}, {"english", "en"}, {"french", "fr"},
                                       {"german", "de"}, {"spanish", "es"}, {"italian", "it"}}) {
        if (v == name || v == code) return env = name, true;
      }
      return false;
    case Kind::kMsaa:
      if (v == "console" || v.empty()) return env = "console", true;
      if (v == "2" || v == "4" || v == "8") return env = v, true;
      if (!ParseBoolText(v, b)) return false;  // older files: true = console, false = off
      return env = b ? "console" : "off", true;
    case Kind::kPrompts:
      for (const char* name : {"auto", "xbox", "keyboard", "playstation", "switch"}) {
        if (v == name) return env = name, true;
      }
      return false;
    case Kind::kPath: {
      if (v.empty()) return env = "", true;
      std::filesystem::path p = FromU8(Trim(text));
      if (p.is_relative()) p = fcp::paths::Root() / p;
      return env = U8(p), true;
    }
  }
  return false;
}

std::string DefaultValue(const Key& k) {
  if (std::string dev = fcp::device::Default(k.env); !dev.empty()) return dev;  // fcp/device_preset.h
  std::string d = k.def;
  if (d.size() >= 2 && d.front() == '"') d = d.substr(1, d.size() - 2);
  return d;
}

void SetEnv(const char* name, const std::string& value) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), w.data(), n);
  SetEnvironmentVariableW(std::wstring(name, name + std::strlen(name)).c_str(), w.c_str());
}

// Writes `mode = "<mode>"` into [display], keeping every other line (comments included).
bool StoreMode(const std::string& mode) {
  if (g_file.empty()) return false;
  const bool ok = fcp::toml::SetValue(g_file, "display", "mode", fcp::toml::Quote(mode));
  if (ok) {
    std::lock_guard lock(g_write_mutex);
    g_values["display.mode"] = mode;
  }
  return ok;
}

const Key* FindKey(std::string_view section, std::string_view key) {
  for (const Key& k : kKeys) {
    if (section == k.section && key == k.key) return &k;
  }
  return nullptr;
}

bool Quoted(const Key& k) { return k.def[0] == '"'; }

std::string SettingText(std::string_view name) {
  if (auto s = REX_SETTINGS().Find(name); s) return (*s)->GetText();
  return {};
}

void ToggleFullscreen() {
  const bool now = SettingText("ui.fullscreen") == "true";
  const bool exclusive = SettingText("ui.fullscreen_exclusive") == "true";
  const bool next = !now;
  if (auto set = REX_SETTINGS().SetText(rex::SettingLayer::kRuntime, "ui.fullscreen", next ? "true" : "false");
      !set) {
    FCPLOG("settings: Alt+Enter: ui.fullscreen FAILED: {}", set.error().GetDescription());
    return;
  }
  const std::string mode = next ? (exclusive ? "exclusive" : "borderless") : "windowed";
  std::string stored;
  if (!FromFile("FCP_DISPLAY_MODE") && !fcp::env::Get("FCP_DISPLAY_MODE").empty()) {
    stored = "not stored (FCP_DISPLAY_MODE comes from the environment)";
  } else if (g_file.empty()) {
    stored = "not stored (no settings file)";
  } else {
    stored = StoreMode(mode) ? std::format("stored in {}", U8(g_file)) : std::format("storing in {} FAILED", U8(g_file));
  }
  FCPLOG("settings: Alt+Enter: {} -> {} ({})", now ? "fullscreen" : "window", mode, stored);
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    FCPLOG("settings: window 1 s after Alt+Enter: {}", fcp::relaunch::DescribeOwnWindow());
  }).detach();
}

class Hotkeys final : public rex::ui::IUIEventListener {
 public:
  void OnUIEvent(const rex::ui::UIEvent&) override {}
  void OnInputEvent(rex::InputEvent& e) override {
    if (e.type != rex::InputEvent::Type::kKeyDown || e.key != rex::VirtualKey::kReturn) return;
    const auto mods = static_cast<unsigned>(e.modifiers);
    if (trace_) FCPLOG("settings: Enter down, modifiers {:X}, repeat {}", mods, e.repeat);
    if (mods != static_cast<unsigned>(rex::KeyModifiers::kAlt)) return;
    e.handled = true;  // the game never sees this Enter (Start on the keyboard pad)
    if (!e.repeat) ToggleFullscreen();
  }
  bool trace_ = false;
};
Hotkeys g_hotkeys;
rex::ScopeGuard g_hotkeys_guard;

}  // namespace

void Load() {
  const std::string over = fcp::env::Get("FCP_SETTINGS_FILE");
  if (Lower(over) == "none" || over == "0") {
    g_file.clear();
    FCPLOG("settings: FCP_SETTINGS_FILE={}: no settings file, built-in defaults", over);
  } else {
    g_file = over.empty() ? fcp::paths::Root() / "predator.toml" : FromU8(over);
    if (g_file.is_relative()) g_file = fcp::paths::Root() / g_file;
  }
  std::map<std::string, std::string> values;
  if (!g_file.empty()) {
    std::error_code ec;
    if (!std::filesystem::exists(g_file, ec)) {
      std::filesystem::create_directories(g_file.parent_path(), ec);
      std::ofstream out(g_file, std::ios::binary);
      out << DefaultFile();
      FCPLOG("settings: {} missing, written with the defaults{}", U8(g_file), out ? "" : " (FAILED)");
    } else {
      values = Parse(g_file);
      FCPLOG("settings: {} read ({} keys)", U8(g_file), values.size());
    }
  }
  std::set<std::string> known;
  for (const Key& k : kKeys) {
    const std::string name = std::string(k.section) + "." + k.key;
    known.insert(name);
    const auto found = values.find(name);
    const bool in_file = found != values.end();
    const std::string text = in_file ? found->second : DefaultValue(k);
    std::string env;
    if (!Normalize(k, text, env)) {
      FCPLOG("settings: [{}] {} = \"{}\" is not valid, default {}", k.section, k.key, text, k.def);
      Normalize(k, DefaultValue(k), env);
      g_values[name] = DefaultValue(k);
    } else {
      g_values[name] = text;
    }
    const std::string outside = fcp::env::Get(k.env);
    if (!outside.empty()) {
      g_env_wins.insert(name);
      FCPLOG("settings: [{}] {}: environment {}={} wins{}", k.section, k.key, k.env, outside,
             in_file ? std::format(" over the file's \"{}\"", found->second) : std::string());
      continue;
    }
    if (env.empty()) {
      FCPLOG("settings: [{}] {} = {}{} -> {} not set (module default)", k.section, k.key,
             in_file ? found->second : DefaultValue(k), in_file ? "" : " (default)", k.env);
      continue;
    }
    SetEnv(k.env, env);
    g_from_file.insert(k.env);
    FCPLOG("settings: [{}] {} = {}{} -> {}={}", k.section, k.key, in_file ? found->second : DefaultValue(k),
           in_file ? "" : " (default)", k.env, env);
  }
  for (const auto& [name, value] : values) {
    if (!known.contains(name)) FCPLOG("settings: unknown key {} = {} ignored", name, value);
  }
}

void Apply(rex::SettingsRegistry& settings) {
  const auto set = [&](std::string_view name, const std::string& value, const char* from) {
    if (auto r = settings.SetText(rex::SettingLayer::kFile, name, value); !r) {
      FCPLOG("settings: {} = {} FAILED: {}", name, value, r.error().GetDescription());
      return;
    }
    FCPLOG("setting {} = {} (file layer, {})", name, value, from);
  };
  std::string mode = Lower(fcp::env::Get("FCP_DISPLAY_MODE"));
  if (mode.empty()) mode = "windowed";
  if (mode != "windowed" && mode != "borderless" && mode != "exclusive") {
    FCPLOG("settings: FCP_DISPLAY_MODE={} unknown (windowed, borderless, exclusive): windowed", mode);
    mode = "windowed";
  }
  set("ui.fullscreen", mode == "windowed" ? "false" : "true", "FCP_DISPLAY_MODE");
  set("ui.fullscreen_exclusive", mode == "exclusive" ? "true" : "false", "FCP_DISPLAY_MODE");
  const int64_t w = fcp::env::Int("FCP_WINDOW_WIDTH", 0), h = fcp::env::Int("FCP_WINDOW_HEIGHT", 0);
  if (w > 0 && h > 0) {
    set("ui.window_width", std::to_string(w), "FCP_WINDOW_WIDTH");
    set("ui.window_height", std::to_string(h), "FCP_WINDOW_HEIGHT");
  }
  if (const int64_t m = fcp::env::Int("FCP_MONITOR", 0); m > 0) {
    set("ui.monitor", std::to_string(m), "FCP_MONITOR");
  }
  // [game] language: the console's system language, which the title reads with XGetLanguage (kernel.user_language:
  // 0 = the host's preferred locales, else the XLanguage id). Read until the launch.
  {
    std::string lang = Lower(fcp::env::Get("FCP_LANGUAGE"));
    if (lang.empty()) lang = "english";
    const char* id = lang == "auto" ? "0" : lang == "german" || lang == "de" ? "3" : lang == "french" || lang == "fr" ? "4"
                     : lang == "spanish" || lang == "es" ? "5" : lang == "italian" || lang == "it" ? "6"
                     : lang == "english" || lang == "en" ? "1" : nullptr;
    if (!id) {
      FCPLOG("settings: FCP_LANGUAGE={} unknown (auto, english, french, german, spanish, italian): english", lang);
      id = "1";
    }
    set("kernel.user_language", id, "FCP_LANGUAGE");
  }
  fcp::pc_input::ApplyStartupSettings(settings);  // hid.drivers from [pad] backend of predator_input.toml
}

void AttachUI(rex::ui::IUISystem& ui) {
  fcp::pc_input::AttachUI(ui);  // focus pause
  g_hotkeys_guard = ui.AddEventListener(g_hotkeys);
  FCPLOG("settings: Alt+Enter switches window/fullscreen");
  fcp::menu::Attach(ui);  // F10 / Back+Start settings menu, frame rate display
  // Test: FCP_TEST_ALT_ENTER=<s>[,<s>...] posts Alt+Enter as window messages to the own window at those seconds
  // after the title launch (the whole path: SDL, the listener, the setting, the file), no focus needed.
  const std::string test = fcp::env::Get("FCP_TEST_ALT_ENTER");
  if (test.empty()) return;
  if (!fcp::devstart::ActiveInGeneration("FCP_TEST_ALT_ENTER_GENS")) {
    FCPLOG("FCP_TEST_ALT_ENTER: not in this relaunch generation (FCP_TEST_ALT_ENTER_GENS, default 0)");
    return;
  }
  g_hotkeys.trace_ = true;
  std::vector<double> times;
  std::stringstream ss(test);
  for (std::string item; std::getline(ss, item, ',');) {
    if (!Trim(item).empty()) times.push_back(std::atof(item.c_str()));
  }
  std::thread([&ui, times] {
    const auto start = std::chrono::steady_clock::now();
    for (double t : times) {
      std::this_thread::sleep_until(start + std::chrono::duration<double>(t));
      auto native = ui.GetNativeWindow();
      HWND hwnd = native ? static_cast<HWND>(native->window) : nullptr;
      if (!hwnd) {
        FCPLOG("FCP_TEST_ALT_ENTER: {:.1f} s: no window", t);
        continue;
      }
      constexpr LPARAM kAltContext = LPARAM(1) << 29;
      const LPARAM menu_scan = LPARAM(MapVirtualKeyW(VK_MENU, MAPVK_VK_TO_VSC)) << 16;
      const LPARAM enter_scan = LPARAM(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC)) << 16;
      constexpr LPARAM kUp = (LPARAM(1) << 30) | (LPARAM(1) << 31);
      FCPLOG("FCP_TEST_ALT_ENTER: {:.1f} s: posting Alt+Enter to the window (foreground: {}, SDK focus: {})", t,
             GetForegroundWindow() == hwnd ? "yes" : "no", ui.IsFocused() ? "yes" : "no");
      PostMessageW(hwnd, WM_SYSKEYDOWN, VK_MENU, 1 | menu_scan | kAltContext);
      PostMessageW(hwnd, WM_SYSKEYDOWN, VK_RETURN, 1 | enter_scan | kAltContext);
      PostMessageW(hwnd, WM_SYSKEYUP, VK_RETURN, 1 | enter_scan | kAltContext | kUp);
      PostMessageW(hwnd, WM_KEYUP, VK_MENU, 1 | menu_scan | kUp);
    }
  }).detach();
}

void DetachUI() {
  fcp::menu::Detach();
  g_hotkeys_guard.Reset();
  fcp::pc_input::DetachUI();
}

bool FromFile(std::string_view env_name) { return g_from_file.contains(env_name); }

std::string Value(std::string_view section, std::string_view key) {
  const Key* k = FindKey(section, key);
  std::lock_guard lock(g_write_mutex);
  const std::string name = std::string(section) + "." + std::string(key);
  if (k && g_env_wins.contains(name)) return fcp::env::Get(k->env);  // what is in force
  const auto it = g_values.find(name);
  if (it != g_values.end()) return it->second;
  return k ? DefaultValue(*k) : std::string();
}

std::string EnvironmentOverride(std::string_view section, std::string_view key) {
  const Key* k = FindKey(section, key);
  {
    std::lock_guard lock(g_write_mutex);
    if (!k || !g_env_wins.contains(std::string(section) + "." + std::string(key))) return {};
  }
  return std::format("{}={}", k->env, fcp::env::Get(k->env));
}

std::string Store(std::string_view section, std::string_view key, const std::string& text) {
  const Key* k = FindKey(section, key);
  if (!k) return "unknown key";
  std::string env;
  if (!Normalize(*k, text, env)) return std::format("\"{}\" is not valid", text);
  const std::string name = std::string(section) + "." + std::string(key);
  bool env_wins = false;
  {
    std::lock_guard lock(g_write_mutex);
    g_values[name] = text;
    env_wins = g_env_wins.contains(name);
  }
  // this process: the variable as Load would set it (modules that read it later see the new value)
  if (!env_wins) {
    SetEnv(k->env, env);
    if (!env.empty()) g_from_file.insert(k->env);
  }
  std::string result;
  if (g_file.empty()) {
    result = "not stored (no settings file)";
  } else if (!fcp::toml::SetValue(g_file, section, key, Quoted(*k) ? fcp::toml::Quote(text) : text)) {
    result = std::format("storing in {} FAILED", U8(g_file));
  } else {
    result = std::format("stored in {}", U8(g_file.filename()));
  }
  FCPLOG("settings: [{}] {} = {} ({}{}{})", section, key, text, result, env_wins ? "; the environment's " : "",
         env_wins ? std::string(k->env) + " still wins in this and the next start" : std::string());
  return g_file.empty() || result.starts_with("stored") ? std::string() : result;
}

std::filesystem::path File() { return g_file; }

}  // namespace fcp::settings
