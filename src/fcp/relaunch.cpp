#include "fcp/relaunch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <thread>

#include <windows.h>

#include <rex/app/app.h>
#include <rex/platform/file.h>
#include <rex/settings_registry.h>

#include "fcp/env.h"
#include "fcp/log.h"
#include "fcp/user_paths.h"
#include "fcp/user_settings.h"
#include "fcp/watchdog.h"

namespace fcp::relaunch {
namespace {

enum class State { kIdle, kPending, kHandedOver, kFailed, kCanceled };

// Child side (from the hand-over variables).
int g_gen = 0;
std::string g_event_name;
std::vector<uint8_t> g_handover;
bool g_has_handover = false;
bool g_has_window = false;
WINDOWPLACEMENT g_placement{};
int g_client_w = 0, g_client_h = 0;
bool g_has_window_ext = false;  // the fields below came with the hand-over
int g_client_x = 0, g_client_y = 0;
bool g_fullscreen = false, g_exclusive = false;
RECT g_monitor{};
std::string g_notice;
DWORD g_parent_pid = 0;
std::atomic<bool> g_first_swap_done{false};

// Parent side.
std::mutex g_mutex;
std::vector<uint8_t> g_launch_data;  // last XamLoaderSetLaunchData
std::atomic<State> g_state{State::kIdle};
std::atomic<bool> g_dashboard{false};
HANDLE g_child = nullptr;
HANDLE g_ready = nullptr;
HANDLE g_cancel = nullptr;
DWORD g_child_pid = 0;
std::chrono::steady_clock::time_point g_spawned_at;

std::string Hex(const std::vector<uint8_t>& data) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t b : data) {
    out.push_back(kDigits[b >> 4]);
    out.push_back(kDigits[b & 15]);
  }
  return out;
}

bool Unhex(const std::string& text, std::vector<uint8_t>& out) {
  if (text.size() % 2) {
    return false;
  }
  out.clear();
  for (size_t i = 0; i < text.size(); i += 2) {
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    const int hi = nibble(text[i]), lo = nibble(text[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(uint8_t(hi << 4 | lo));
  }
  return true;
}

std::wstring Widen(const std::string& s) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

// The process's own top-level window (the SDK window), null when there is none (yet).
HWND OwnWindow() {
  struct Find {
    DWORD pid;
    HWND hwnd;
  } find{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto* f = reinterpret_cast<Find*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != f->pid || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
          return TRUE;
        }
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 64);
        if (std::wcscmp(cls, L"#32770") == 0) {  // a message box
          return TRUE;
        }
        f->hwnd = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&find));
  return find.hwnd;
}

std::string SettingText(std::string_view name) {
  if (auto s = REX_SETTINGS().Find(name); s) return (*s)->GetText();
  return {};
}

// Win32 view of the window, for the log: rectangle, client size, style, monitor; "fullscreen" when the window
// covers its monitor without a caption.
std::string DescribeWindow(HWND hwnd) {
  if (!hwnd) return "no window";
  RECT r{}, c{};
  GetWindowRect(hwnd, &r);
  GetClientRect(hwnd, &c);
  POINT origin{0, 0};
  ClientToScreen(hwnd, &origin);
  const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& m = mi.rcMonitor;
  const bool covers = r.left == m.left && r.top == m.top && r.right == m.right && r.bottom == m.bottom;
  const bool caption = (style & WS_CAPTION) == WS_CAPTION;
  std::string device;
  for (const wchar_t* p = mi.szDevice; *p; ++p) device.push_back(char(*p));
  return std::format("{} rect {},{} {}x{}, client {}x{} at {},{}, {}{}{}, monitor {} {},{} {}x{}",
                     covers && !caption ? "FULLSCREEN" : IsZoomed(hwnd) ? "MAXIMIZED" : IsIconic(hwnd) ? "MINIMIZED" : "WINDOW",
                     r.left, r.top, r.right - r.left, r.bottom - r.top, c.right, c.bottom, origin.x, origin.y,
                     caption ? "caption" : "no caption", IsWindowVisible(hwnd) ? ", visible" : ", hidden",
                     SettingText("ui.fullscreen_exclusive") == "true" ? ", exclusive set" : "", device, m.left,
                     m.top, m.right - m.left, m.bottom - m.top);
}

// Environment block for the new process: ours without old hand-over variables and without the values this
// process took from predator.toml (the new process reads the file itself), plus the new ones.
std::wstring ChildEnvironment(const std::vector<std::pair<std::string, std::string>>& add) {
  std::wstring block;
  if (LPWCH env = GetEnvironmentStringsW()) {
    for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) {
      if (_wcsnicmp(p, L"FCP_RELAUNCH_", 13) == 0) {
        continue;
      }
      if (const wchar_t* eq = std::wcschr(p, L'='); eq && eq != p) {
        std::string name;
        for (const wchar_t* q = p; q < eq; ++q) name.push_back(char(*q));
        if (fcp::settings::FromFile(name)) {
          continue;
        }
      }
      block.append(p);
      block.push_back(L'\0');
    }
    FreeEnvironmentStringsW(env);
  }
  for (const auto& [name, value] : add) {
    block += Widen(name + "=" + value);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  return block;
}

void ShowError(const std::string& text) {
  FCPLOG("relaunch: ERROR {}", text);
  if (fcp::env::Flag("FCP_NO_DIALOGS")) {
    return;
  }
  MessageBoxW(OwnWindow(), Widen(text).c_str(), L"Far Cry Instincts Predator",
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void Waiter(double timeout_s) {
  SetThreadDescription(GetCurrentThread(), L"fcp relaunch waiter");
  HANDLE handles[3] = {g_ready, g_child, g_cancel};
  const DWORD timeout_ms = DWORD(timeout_s * 1000.0);
  const DWORD r = WaitForMultipleObjects(3, handles, FALSE, timeout_ms);
  const double after =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_spawned_at).count();
  if (r == WAIT_OBJECT_0) {
    State expected = State::kPending;
    if (g_state.compare_exchange_strong(expected, State::kHandedOver)) {
      FCPLOG("relaunch: new process PID {} showed its first frame after {:.2f} s, this process exits",
             g_child_pid, after);
      rex::app::RequestShutdown(0);
    }
    return;
  }
  if (r == WAIT_OBJECT_0 + 2) {
    return;  // canceled by CancelIfPending
  }
  State expected = State::kPending;
  if (!g_state.compare_exchange_strong(expected, State::kFailed)) {
    return;
  }
  if (r == WAIT_OBJECT_0 + 1) {
    DWORD code = 0;
    GetExitCodeProcess(g_child, &code);
    ShowError(std::format("The restarted game (PID {}) ended after {:.1f} s before showing a frame "
                          "(exit code 0x{:X}). See predator.gen{}.log in {}.",
                          g_child_pid, after, code, g_gen + 1, fcp::paths::Utf8(fcp::paths::Root())));
  } else {
    TerminateProcess(g_child, 0xFC0E);
    WaitForSingleObject(g_child, 5000);
    ShowError(std::format("The restarted game (PID {}) showed no frame within {:.0f} s and was "
                          "terminated.", g_child_pid, timeout_s));
  }
  rex::app::RequestShutdown(1);
}

void ResetTitle(HWND hwnd, std::wstring original) {
  std::this_thread::sleep_for(std::chrono::seconds(8));
  if (IsWindow(hwnd)) {
    SetWindowTextW(hwnd, original.c_str());
  }
}

}  // namespace

std::string DescribeLaunchData(const std::vector<uint8_t>& d);

void Configure() {
  g_gen = static_cast<int>(fcp::env::Int("FCP_RELAUNCH_GEN", 0));
  g_event_name = fcp::env::Get("FCP_RELAUNCH_EVENT");
  g_parent_pid = static_cast<DWORD>(fcp::env::Int("FCP_RELAUNCH_PARENT", 0));
  g_notice = fcp::env::Get("FCP_RELAUNCH_NOTICE");
  if (g_gen > 0) {
    g_has_handover = Unhex(fcp::env::Get("FCP_RELAUNCH_DATA"), g_handover) && !g_handover.empty();
    int x = 0, y = 0, w = 0, h = 0, show = 0, cw = 0, ch = 0;
    int cx = 0, cy = 0, fs = 0, ex = 0, ml = 0, mt = 0, mw = 0, mh = 0;
    const std::string window = fcp::env::Get("FCP_RELAUNCH_WINDOW");
    const int fields = std::sscanf(window.c_str(), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &x, &y, &w, &h,
                                   &show, &cw, &ch, &cx, &cy, &fs, &ex, &ml, &mt, &mw, &mh);
    if ((fields == 7 || fields == 15) && w > 0 && h > 0) {
      g_has_window = true;
      g_placement.length = sizeof(g_placement);
      g_placement.showCmd = UINT(show);
      g_placement.rcNormalPosition = {x, y, x + w, y + h};
      g_client_w = cw;
      g_client_h = ch;
      if (fields == 15) {
        g_has_window_ext = true;
        g_client_x = cx;
        g_client_y = cy;
        g_fullscreen = fs != 0;
        g_exclusive = ex != 0;
        g_monitor = {ml, mt, ml + mw, mt + mh};
      }
    }
  }
}

int Generation() {
  return g_gen;
}

std::string LogName() {
  if (g_gen == 0) {
    // A new chain: remove the logs of an earlier chain (own files in the user folder only).
    std::error_code ec;
    const std::filesystem::path dir = fcp::paths::Root();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (name.starts_with("predator.gen") && name.ends_with(".log")) {
        std::filesystem::remove(entry.path(), ec);
      }
    }
    return "predator.log";
  }
  return std::format("predator.gen{}.log", g_gen);
}

void LogStart() {
  if (g_gen == 0) {
    FCPLOG("relaunch: generation 0 (PID {})", GetCurrentProcessId());
    return;
  }
  FCPLOG("relaunch: generation {} (PID {}), parent PID {}, launch data {} bytes, window {}, event {}",
         g_gen, GetCurrentProcessId(), g_parent_pid, g_handover.size(),
         g_has_window ? std::format("{},{} {}x{} show {}", g_placement.rcNormalPosition.left,
                                    g_placement.rcNormalPosition.top,
                                    g_placement.rcNormalPosition.right - g_placement.rcNormalPosition.left,
                                    g_placement.rcNormalPosition.bottom - g_placement.rcNormalPosition.top,
                                    g_placement.showCmd) +
                            (g_has_window_ext
                                 ? std::format(", client at {},{}, {}", g_client_x, g_client_y,
                                               g_fullscreen ? (g_exclusive ? "exclusive fullscreen" : "fullscreen")
                                                            : "windowed")
                                 : std::string())
                      : std::string("none"),
         g_event_name.empty() ? "none" : g_event_name);
  if (g_has_handover) {
    FCPLOG("relaunch: handed-over launch data: {}", DescribeLaunchData(g_handover));
  }
}

const std::vector<uint8_t>* HandoverData() {
  return g_has_handover ? &g_handover : nullptr;
}

int HandoverClientWidth() {
  return g_client_w;
}

int HandoverClientHeight() {
  return g_client_h;
}

void ApplyWindowHandover(rex::SettingsRegistry& settings) {
  if (!g_has_window) {
    return;
  }
  const auto set = [&](std::string_view name, const std::string& value) {
    if (auto r = settings.SetText(rex::SettingLayer::kEnvironment, name, value); !r) {
      FCPLOG("setting {} = {} FAILED: {}", name, value, r.error().GetDescription());
      return;
    }
    FCPLOG("setting {} = {} (environment layer, relaunch: previous window)", name, value);
  };
  const bool keep = fcp::env::Flag("FCP_RELAUNCH_KEEP_WINDOW", true);
  if (!g_has_window_ext || !keep) {
    if (g_client_w > 0 && g_client_h > 0) {
      set("ui.window_width", std::to_string(g_client_w));
      set("ui.window_height", std::to_string(g_client_h));
    }
    if (!keep) FCPLOG("relaunch: FCP_RELAUNCH_KEEP_WINDOW=0: only the window size, placement at title launch");
    return;
  }
  if (g_fullscreen) {
    // The window goes fullscreen on the monitor it is created on, before it shows: create it centered on the
    // previous monitor (windowed size of this process's settings, used when the player leaves fullscreen).
    int w = std::atoi(SettingText("ui.window_width").c_str()), h = std::atoi(SettingText("ui.window_height").c_str());
    if (w <= 0 || h <= 0) w = 1280, h = 720;
    const int mw = g_monitor.right - g_monitor.left, mh = g_monitor.bottom - g_monitor.top;
    set("ui.fullscreen", "true");
    set("ui.fullscreen_exclusive", g_exclusive ? "true" : "false");
    set("ui.window_position", std::format("{},{}", g_monitor.left + std::max(0, (mw - w) / 2),
                                          g_monitor.top + std::max(0, (mh - h) / 2)));
    return;
  }
  const bool maximized = g_placement.showCmd == SW_SHOWMAXIMIZED;
  set("ui.fullscreen", "false");
  if (g_client_w > 0 && g_client_h > 0 && !maximized) {
    set("ui.window_width", std::to_string(g_client_w));
    set("ui.window_height", std::to_string(g_client_h));
  }
  if (maximized) {
    // client size and origin of a maximized window are the monitor's; the normal rectangle follows at title launch
    set("ui.window_maximized", "true");
    set("ui.window_position", std::format("{},{}", g_placement.rcNormalPosition.left, g_placement.rcNormalPosition.top));
  } else {
    set("ui.window_position", std::format("{},{}", g_client_x, g_client_y));
  }
}

void OnTitleLaunch() {
  if (g_gen > 0 && fcp::env::Int("FCP_TEST_RELAUNCH_FAIL", -1) == g_gen) {
    FCPLOG("FCP_TEST_RELAUNCH_FAIL={}: this process exits with code 7 before its first frame", g_gen);
    fcp::log::Drain(std::chrono::milliseconds(1000));
    TerminateProcess(GetCurrentProcess(), 7);
  }
  if (g_gen == 0) {
    return;
  }
  if (const int64_t delay = fcp::env::Int("FCP_TEST_RELAUNCH_DELAY_MS", 0); delay > 0) {
    FCPLOG("FCP_TEST_RELAUNCH_DELAY_MS={}: waiting before the title starts (longer hand-over)", delay);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
  }
  HWND hwnd = OwnWindow();
  if (!hwnd) {
    FCPLOG("relaunch: no window found at title launch, placement not applied");
    return;
  }
  if (g_has_window) {
    const std::string before = DescribeWindow(hwnd);
    WINDOWPLACEMENT now{};
    now.length = sizeof(now);
    GetWindowPlacement(hwnd, &now);
    const RECT& a = now.rcNormalPosition;
    const RECT& b = g_placement.rcNormalPosition;
    const bool same = now.showCmd == g_placement.showCmd && a.left == b.left && a.top == b.top &&
                      a.right == b.right && a.bottom == b.bottom;
    if (g_has_window_ext && g_fullscreen && fcp::env::Flag("FCP_RELAUNCH_KEEP_WINDOW", true)) {
      FCPLOG("relaunch: fullscreen handed over, no placement; window at title launch: {}", before);
    } else if (same) {
      FCPLOG("relaunch: window already where the previous one was (opened there, not moved): {}", before);
    } else {
      SetWindowPlacement(hwnd, &g_placement);
      FCPLOG("relaunch: window placement of the previous process applied: was {}, now {}", before,
             DescribeWindow(hwnd));
    }
  }
  if (g_notice == "mapeditor") {
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    std::wstring original = title;
    SetWindowTextW(hwnd, (original + L"  -  Map editor is not supported in this version").c_str());
    FCPLOG("relaunch: notice shown in the window title for 8 s (map editor not supported)");
    std::thread(ResetTitle, hwnd, std::move(original)).detach();
  }
}

void OnFirstSwap() {
  if (g_first_swap_done.exchange(true)) {
    return;
  }
  FCPLOG("window at the first frame (generation {}): {}", g_gen, DescribeWindow(OwnWindow()));
  if (g_gen == 0) {
    return;
  }
  if (g_event_name.empty()) {
    return;
  }
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, Widen(g_event_name).c_str());
  if (!event) {
    FCPLOG("relaunch: first frame, hand-over event not found ({}), parent gone?", GetLastError());
    return;
  }
  SetEvent(event);
  CloseHandle(event);
  FCPLOG("relaunch: first frame, hand-over event set (parent PID {})", g_parent_pid);
}

std::string DescribeLaunchData(const std::vector<uint8_t>& d) {
  if (d.size() < 0x3F8) {
    return std::format("{} bytes", d.size());
  }
  const auto text = [&](size_t from, size_t max) {
    std::string s;
    for (size_t i = from; i < from + max && d[i]; ++i) {
      s.push_back(d[i] >= 0x20 && d[i] < 0x7F ? char(d[i]) : '?');
    }
    return s;
  };
  const auto u32 = [&](size_t at) {
    return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
  };
  std::string block;
  for (size_t i = 0x180; i < 0x18C; ++i) {
    block += std::format("{:02X}", d[i]);
  }
  return std::format("cmd=\"{}\" port={} +0x328={:08X} +0x32C=\"{}\" +0x36C={} +0x370=\"{}\" +0x3A8={:08X} "
                     "+0x3AC={} +0x3F3={} +0x3F4={} +0x3F5={} profile={}",
                     text(0x3B2, 64), int(int8_t(d[0x3F2])), u32(0x328), text(0x32C, 64), u32(0x36C),
                     text(0x370, 40), u32(0x3A8), u32(0x3AC), d[0x3F3], d[0x3F4], d[0x3F5], block);
}

void OnSetLaunchData(const uint8_t* data, uint32_t size) {
  std::lock_guard lock(g_mutex);
  g_launch_data.assign(data, data + size);
  FCPLOG("relaunch: title set launch data: {}", DescribeLaunchData(g_launch_data));
}

void OnLaunchTitle(bool has_path, const std::string& path, uint32_t flags) {
  if (!has_path) {
    FCPLOG("relaunch: the title asks for the dashboard (XLaunchNewImage(NULL)): clean shutdown");
    g_dashboard = true;
    return;
  }
  std::string lower = path;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const bool map_editor = lower.ends_with("ige.xex");
  const bool game = lower.empty() || lower.ends_with("default.xex");
  if (!map_editor && !game) {
    FCPLOG("relaunch: unknown launch target \"{}\" flags {:08X}: the title only exits", path, flags);
    return;
  }
  if (fcp::env::Flag("FCP_NO_RELAUNCH")) {
    FCPLOG("relaunch: FCP_NO_RELAUNCH=1, target \"{}\": the title only exits (SDK behaviour)", path);
    return;
  }
  State expected = State::kIdle;
  if (!g_state.compare_exchange_strong(expected, State::kPending)) {
    FCPLOG("relaunch: a relaunch is already running, \"{}\" ignored", path);
    return;
  }
  if (rex::app::IsShutdownRequested()) {
    g_state = State::kCanceled;
    FCPLOG("relaunch: shutdown already requested, no relaunch");
    return;
  }
  std::vector<uint8_t> data;
  {
    std::lock_guard lock(g_mutex);
    data = g_launch_data;
  }
  if (map_editor) {
    FCPLOG("relaunch: map editor \"{}\" is not supported (ige.xex is not recompiled); restarting "
           "default.xex with the launch data prepared for the editor, back to the main menu", path);
  }
  const int child_gen = g_gen + 1;
  const std::string event_name =
      std::format("Local\\fcp_relaunch_{}_{}_ready", GetCurrentProcessId(), child_gen);
  // Hand-over objects before CreateProcess.
  g_ready = CreateEventW(nullptr, TRUE, FALSE, Widen(event_name).c_str());
  g_cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::vector<std::pair<std::string, std::string>> add = {
      {"FCP_RELAUNCH_GEN", std::to_string(child_gen)},
      {"FCP_RELAUNCH_EVENT", event_name},
      {"FCP_RELAUNCH_PARENT", std::to_string(GetCurrentProcessId())},
      {"FCP_RELAUNCH_DATA", Hex(data)},
  };
  if (map_editor) {
    add.emplace_back("FCP_RELAUNCH_NOTICE", "mapeditor");
  }
  if (HWND hwnd = OwnWindow()) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    RECT client{};
    if (GetWindowPlacement(hwnd, &wp) && GetClientRect(hwnd, &client)) {
      const RECT& r = wp.rcNormalPosition;
      POINT origin{0, 0};
      ClientToScreen(hwnd, &origin);
      MONITORINFO mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
      const RECT& m = mi.rcMonitor;
      // fullscreen as the SDK has it (the player's Alt+Enter included), exclusive or borderless
      const bool fullscreen = SettingText("ui.fullscreen") == "true";
      const bool exclusive = SettingText("ui.fullscreen_exclusive") == "true";
      add.emplace_back("FCP_RELAUNCH_WINDOW",
                       std::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}", r.left, r.top, r.right - r.left,
                                   r.bottom - r.top, wp.showCmd, client.right, client.bottom, origin.x, origin.y,
                                   fullscreen ? 1 : 0, exclusive ? 1 : 0, m.left, m.top, m.right - m.left,
                                   m.bottom - m.top));
      FCPLOG("relaunch: hands over the window: {} (setting ui.fullscreen {})", DescribeWindow(hwnd),
             fullscreen ? "true" : "false");
    }
  }
  std::wstring environment = ChildEnvironment(add);
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring command_line = GetCommandLineW();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // No CREATE_SUSPENDED: a parent that dies at any point never leaves a frozen process behind.
  if (!CreateProcessW(exe, command_line.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                      environment.data(), nullptr, &startup, &process)) {
    g_state = State::kFailed;
    ShowError(std::format("The game could not restart itself (CreateProcess error {}).", GetLastError()));
    return;
  }
  AllowSetForegroundWindow(process.dwProcessId);
  CloseHandle(process.hThread);
  g_child = process.hProcess;
  g_child_pid = process.dwProcessId;
  g_spawned_at = std::chrono::steady_clock::now();
  const double timeout = double(fcp::env::Int("FCP_RELAUNCH_TIMEOUT", 120));
  FCPLOG("relaunch: started generation {} as PID {} (target \"{}\", {} bytes launch data, swaps={}); "
         "this window stays until its first frame",
         child_gen, g_child_pid, path, data.size(), fcp::watch::SwapCount());
  std::thread(Waiter, timeout).detach();
}

bool Pending() {
  return g_state.load() == State::kPending;
}

bool Active() {
  return g_state.load() != State::kIdle;
}

void CancelIfPending(const char* reason) {
  State expected = State::kPending;
  if (!g_state.compare_exchange_strong(expected, State::kCanceled)) {
    return;
  }
  SetEvent(g_cancel);
  TerminateProcess(g_child, 0xFC0C);
  const DWORD waited = WaitForSingleObject(g_child, 5000);
  FCPLOG("relaunch: canceled ({}): new process PID {} terminated{}", reason, g_child_pid,
         waited == WAIT_OBJECT_0 ? "" : " (did not end within 5 s)");
}

std::string DescribeOwnWindow() {
  return DescribeWindow(OwnWindow());
}

bool DashboardRequested() {
  return g_dashboard.load();
}

}  // namespace fcp::relaunch
