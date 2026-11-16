// Display helpers: DPI awareness and scale per monitor, video modes (src/fcp/display.h).
//
// Idea of the DPI handling from FCI (Far Cry Instincts Recomp, README "Anzeige": "DPI-bewusst (pro Monitor) ...
// das Standardfenster behält seine bisherige Größe auf dem Bildschirm", MIT, same author); the code is our own (FCI
// has a Win32/D3D11 window, this port an SDL3 one).
#include "fcp/display.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <rex/settings_registry.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/ui_system.h>

#include "fcp/env.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"

namespace fcp::display {
namespace {

// DeclareDpiAware (before the log): 1 declared, 0 refused (error), -1 not asked (FCP_DPI_EARLY=0)
int g_declared = -1;
DWORD g_declare_error = 0;
bool g_follow = true;                 // FCP_DPI_WINDOW (read in LogStart)
std::atomic<int> g_fake_percent{0};   // FCP_TEST_DPI: the scale in force, 0 = the monitor's
double g_window_scale = 0;            // UI thread: the scale the window's size was made for
int g_set_w = -1, g_set_h = -1;       // UI thread: client size FollowScale gave the window last
double g_points_w = 0, g_points_h = 0;  // UI thread: that size at 100 % (before a cut to the work area)

using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);  // shcore, MDT_EFFECTIVE_DPI = 0

GetDpiForMonitorFn DpiForMonitorFn() {
  static const GetDpiForMonitorFn fn = [] {
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    return shcore ? reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
  }();
  return fn;
}

UINT RealMonitorDpi(HMONITOR monitor) {
  UINT x = 0, y = 0;
  if (GetDpiForMonitorFn fn = DpiForMonitorFn(); fn && monitor && SUCCEEDED(fn(monitor, 0, &x, &y)) && x) return x;
  return GetDpiForSystem();
}

double MonitorScale(HMONITOR monitor) {
  if (const int fake = g_fake_percent.load(); fake > 0) return fake / 100.0;
  return RealMonitorDpi(monitor) / 96.0;
}

UINT ScaleDpi(double scale) { return UINT(std::lround(scale * 96.0)); }

// SDL's display order on Windows: the primary monitor first, then the others as EnumDisplayMonitors lists them
// (SDL_windowsmodes.c, the GDI path). [display] monitor n = the n-th of them, 0 = the primary one.
std::vector<HMONITOR> MonitorsInSdlOrder() {
  struct Collect {
    std::vector<HMONITOR> primary, others;
  } c;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* c = reinterpret_cast<Collect*>(p);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        ((mi.dwFlags & MONITORINFOF_PRIMARY) ? c->primary : c->others).push_back(m);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&c));
  c.primary.insert(c.primary.end(), c.others.begin(), c.others.end());
  return c.primary;
}

HMONITOR MonitorByIndex(int monitor) {
  const std::vector<HMONITOR> all = MonitorsInSdlOrder();
  if (monitor >= 1 && size_t(monitor) <= all.size()) return all[size_t(monitor) - 1];
  return MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

HWND OwnWindow(rex::ui::IUISystem* ui) {
  if (!ui) return nullptr;
  auto native = ui->GetNativeWindow();
  return native ? static_cast<HWND>(native->window) : nullptr;
}

HMONITOR WindowMonitor(rex::ui::IUISystem* ui) {
  HWND hwnd = OwnWindow(ui);
  return hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

std::wstring MonitorDevice(HMONITOR monitor) {
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  return GetMonitorInfoW(monitor, &mi) ? std::wstring(mi.szDevice) : std::wstring();
}

std::string Narrow(const std::wstring& w) {
  std::string s;
  for (wchar_t c : w) s += c < 128 ? char(c) : '?';
  return s;
}

std::string SettingText(std::string_view name) {
  if (auto s = REX_SETTINGS().Find(name); s) return (*s)->GetText();
  return {};
}

std::string AwarenessText() {
  const DPI_AWARENESS_CONTEXT ctx = GetThreadDpiAwarenessContext();
  if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return "per-monitor v2";
  if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return "per-monitor (v1)";
  if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)) return "system";
  if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED)) return "unaware (GDI scaled)";
  if (AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_UNAWARE)) return "unaware";
  return "unknown";
}

void SetFake(int percent) {
  g_fake_percent = percent;
  // the SDK's content scale (ImGui) reads this at its next resize or DPI event (rexglue/UPSTREAM.md, 351)
  SetEnvironmentVariableA("REX_TEST_CONTENT_SCALE", percent > 0 ? std::format("{:.4g}", percent / 100.0).c_str() : nullptr);
}

// The window keeps its size on the screen when the scale changes (Windows' own rule for per-monitor v2 windows; SDL
// keeps the pixel size instead). Windowed only; fullscreen and maximized windows are sized by the monitor.
void FollowScale(rex::ui::IUISystem* ui, const char* why) {
  HWND hwnd = OwnWindow(ui);
  if (!hwnd) return;
  const double now = g_fake_percent.load() > 0 ? g_fake_percent.load() / 100.0 : GetDpiForWindow(hwnd) / 96.0;
  const double before = g_window_scale > 0 ? g_window_scale : now;
  if (std::fabs(now / before - 1.0) < 0.01) return;
  const bool fullscreen = SettingText("ui.fullscreen") == "true";
  if (!g_follow || fullscreen || IsZoomed(hwnd) || IsIconic(hwnd)) {
    FCPLOG("display: scale {:.0f} % -> {:.0f} % ({}): window size kept ({})", before * 100, now * 100, why,
           !g_follow ? "FCP_DPI_WINDOW=0" : fullscreen ? "fullscreen" : IsZoomed(hwnd) ? "maximized" : "minimized");
    if (!g_follow) g_window_scale = now;
    return;
  }
  RECT client{}, outer{};
  GetClientRect(hwnd, &client);
  GetWindowRect(hwnd, &outer);
  const int cw = client.right - client.left, ch = client.bottom - client.top;
  if (cw <= 0 || ch <= 0) return;
  // the size at 100 % this window stands for: the one asked last time while the window still has the size it got
  // then (a size cut to the work area comes back on a smaller scale), else the size now
  if (cw != g_set_w || ch != g_set_h) g_points_w = cw / before, g_points_h = ch / before;
  int nw = int(std::lround(g_points_w * now)), nh = int(std::lround(g_points_h * now));
  const DWORD style = DWORD(GetWindowLongW(hwnd, GWL_STYLE)), ex = DWORD(GetWindowLongW(hwnd, GWL_EXSTYLE));
  const UINT dpi = ScaleDpi(now);
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
  const RECT& work = mi.rcWork;
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&frame, style, FALSE, ex, dpi);
  const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  const int max_w = (work.right - work.left) - fw, max_h = (work.bottom - work.top) - fh;
  if ((nw > max_w || nh > max_h) && max_w > 0 && max_h > 0) {
    const double f = std::min(double(max_w) / nw, double(max_h) / nh);
    nw = int(nw * f), nh = int(nh * f);
  }
  const int ow = nw + fw, oh = nh + fh;
  const int x = std::clamp(int(outer.left), int(work.left), std::max(int(work.left), int(work.right) - ow));
  const int y = std::clamp(int(outer.top), int(work.top), std::max(int(work.top), int(work.bottom) - oh));
  SetWindowPos(hwnd, nullptr, x, y, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
  RECT after{};
  GetClientRect(hwnd, &after);
  g_set_w = after.right - after.left, g_set_h = after.bottom - after.top;
  FCPLOG("display: scale {:.0f} % -> {:.0f} % ({}): window client {}x{} -> {}x{} (asked {}x{}) at {},{}", before * 100,
         now * 100, why, cw, ch, after.right - after.left, after.bottom - after.top, nw, nh, x, y);
  g_window_scale = now;
}

// Exclusive fullscreen without the focus: SDL leaves the video mode when the window loses the focus (it minimizes),
// but a window that never had it (the player clicked another program while the game was loading) keeps the mode
// and the desktop stays in it behind the other program (measured: 2560x1440@60 for the whole run). Such a window is
// minimized after 1.5 s, which gives the desktop its mode back; back in the game it goes fullscreen again.
// UI thread; FCP_EXCLUSIVE_BACKGROUND_MINIMIZE=0 leaves it (as before).
int g_background_checks = 0;
void CheckExclusiveInBackground(rex::ui::IUISystem* ui) {
  HWND hwnd = OwnWindow(ui);
  if (!hwnd || IsIconic(hwnd) || fcp::relaunch::Active() || SettingText("ui.fullscreen") != "true" ||
      SettingText("ui.fullscreen_exclusive") != "true") {
    g_background_checks = 0;
    return;
  }
  DWORD fg_pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &fg_pid);
  if (fg_pid == GetCurrentProcessId()) {
    g_background_checks = 0;
    return;
  }
  if (++g_background_checks < 3) return;
  g_background_checks = 0;
  FCPLOG("display: exclusive fullscreen without the focus (foreground: process {}): window minimized, the desktop gets "
         "its mode back", fg_pid);
  ShowWindow(hwnd, SW_MINIMIZE);
}

class DpiListener final : public rex::ui::IUIEventListener {
 public:
  void OnUIEvent(const rex::ui::UIEvent& e) override {
    if (e.GetType() == rex::ui::UIEvent::Type::kDPIChanged) FollowScale(ui_, "DPI change");
  }
  void OnInputEvent(rex::InputEvent&) override {}
  rex::ui::IUISystem* ui_ = nullptr;
};
DpiListener g_listener;
rex::ScopeGuard g_listener_guard;

struct FakeStep {
  double at;
  int percent;
};
std::vector<FakeStep> g_fake_steps;

int ParsePercent(const std::string& s) {
  const int p = std::atoi(s.c_str());
  return p >= 50 && p <= 400 ? p : 0;
}

}  // namespace

std::string Mode::Value() const { return std::format("{}x{}@{}", width, height, hz); }
std::string Mode::Label() const { return std::format("{} x {} @ {} Hz", width, height, hz); }

void DeclareDpiAware() {
  if (!fcp::env::Flag("FCP_DPI_EARLY", true)) return;
  if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
    g_declared = 1;
  } else {
    g_declared = 0;
    g_declare_error = GetLastError();
  }
}

void LogStart() {
  g_follow = fcp::env::Flag("FCP_DPI_WINDOW", true);
  // FCP_TEST_DPI=<percent>[,<t>:<percent>...]: the first value without a time from the start on
  const std::string test = fcp::env::Get("FCP_TEST_DPI");
  std::stringstream ss(test);
  for (std::string item; std::getline(ss, item, ',');) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
      if (const int p = ParsePercent(item)) SetFake(p);
    } else if (const int p = ParsePercent(item.substr(colon + 1))) {
      g_fake_steps.push_back({std::atof(item.c_str()), p});
    }
  }
  FCPLOG("display: DPI awareness {} ({}){}", AwarenessText(),
         g_declared == 1   ? "declared at start"
         : g_declared == 0 ? std::format("declaring at start FAILED, error {}", g_declare_error)
                           : std::string("FCP_DPI_EARLY=0: left to SDL"),
         g_follow ? "" : "; FCP_DPI_WINDOW=0: window size in pixels, as before");
  int n = 0;
  for (HMONITOR m : MonitorsInSdlOrder()) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(m, &mi);
    FCPLOG("display: monitor {} {} {}x{} at {},{}{}, scale {:.0f} % ({} dpi)", ++n, Narrow(mi.szDevice),
           mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, mi.rcMonitor.left,
           mi.rcMonitor.top, (mi.dwFlags & MONITORINFOF_PRIMARY) ? " (primary)" : "", RealMonitorDpi(m) / 0.96,
           RealMonitorDpi(m));
  }
  if (g_fake_percent.load() > 0 || !g_fake_steps.empty())
    FCPLOG("FCP_TEST_DPI: scale {} % from the start{} (stands for the monitor's; REX_TEST_CONTENT_SCALE for ImGui)",
           g_fake_percent.load() > 0 ? std::to_string(g_fake_percent.load()) : std::string("of the monitor"),
           g_fake_steps.empty() ? "" : std::format(", {} timed changes", g_fake_steps.size()));
}

double StartScale(int monitor) { return MonitorScale(MonitorByIndex(monitor)); }

void BuiltInWindowSize(int monitor, int& width, int& height) {
  width = 1280, height = 720;
  if (!g_follow) return;
  const HMONITOR m = MonitorByIndex(monitor);
  const double scale = MonitorScale(m);
  if (scale <= 1.001) return;
  int w = int(std::lround(1280 * scale)), h = int(std::lround(720 * scale));
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(m, &mi);
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, ScaleDpi(scale));
  const int max_w = (mi.rcWork.right - mi.rcWork.left) - (frame.right - frame.left);
  const int max_h = (mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top);
  if (max_w > 0 && max_h > 0 && (w > max_w || h > max_h)) {
    const double f = std::max(1.0, std::min(max_w / 1280.0, max_h / 720.0));  // never below 1280x720
    w = int(1280 * f), h = int(720 * f);
  }
  width = w, height = h;
}

std::vector<Mode> MonitorModes(rex::ui::IUISystem* ui) {
  const std::wstring device = MonitorDevice(WindowMonitor(ui));
  std::set<std::tuple<int, int, int>> seen;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD i = 0; EnumDisplaySettingsExW(device.empty() ? nullptr : device.c_str(), i, &dm, 0); ++i) {
    if (dm.dmBitsPerPel != 32 || dm.dmDisplayFrequency < 48 || dm.dmPelsWidth < 640 || dm.dmPelsHeight < 480) continue;
    seen.insert({int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)});
  }
  std::vector<Mode> modes;
  for (auto it = seen.rbegin(); it != seen.rend(); ++it) modes.push_back({std::get<0>(*it), std::get<1>(*it), std::get<2>(*it)});
  std::stable_sort(modes.begin(), modes.end(), [](const Mode& a, const Mode& b) {
    return a.width * a.height != b.width * b.height ? a.width * a.height > b.width * b.height
                                                    : a.width != b.width ? a.width > b.width : a.hz > b.hz;
  });
  return modes;
}

Mode DesktopMode(rex::ui::IUISystem* ui) {
  const std::wstring device = MonitorDevice(WindowMonitor(ui));
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (!EnumDisplaySettingsW(device.empty() ? nullptr : device.c_str(), ENUM_REGISTRY_SETTINGS, &dm)) return {};
  return {int(dm.dmPelsWidth), int(dm.dmPelsHeight), int(dm.dmDisplayFrequency)};
}

void AttachUI(rex::ui::IUISystem& ui) {
  g_listener.ui_ = &ui;
  g_listener_guard = ui.AddEventListener(g_listener);
  ui.PostTask([&ui] {
    HWND hwnd = OwnWindow(&ui);
    g_window_scale = g_fake_percent.load() > 0 ? g_fake_percent.load() / 100.0 : hwnd ? GetDpiForWindow(hwnd) / 96.0 : 1.0;
    RECT rc{};
    if (hwnd) GetClientRect(hwnd, &rc);
    FCPLOG("display: window on a {:.0f} % monitor ({}), client {}x{}{}", g_window_scale * 100,
           hwnd ? Narrow(MonitorDevice(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST))) : std::string("no window"),
           rc.right - rc.left, rc.bottom - rc.top, g_follow ? "; a scale change keeps its size on the screen" : "");
  });
  if (fcp::env::Flag("FCP_EXCLUSIVE_BACKGROUND_MINIMIZE", true)) {
    std::thread([&ui] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!g_listener.ui_) return;  // DetachUI
        ui.PostTask([&ui] { CheckExclusiveInBackground(&ui); });
      }
    }).detach();
  }
  if (g_fake_steps.empty()) return;
  std::thread([&ui, steps = g_fake_steps] {
    const auto start = std::chrono::steady_clock::now();
    for (const FakeStep& s : steps) {
      std::this_thread::sleep_until(start + std::chrono::duration<double>(s.at));
      FCPLOG("FCP_TEST_DPI: {:.1f} s: scale {} % (as a move to such a monitor)", s.at, s.percent);
      SetFake(s.percent);
      ui.PostTask([&ui] { FollowScale(&ui, "FCP_TEST_DPI"); });
    }
  }).detach();
}

void DetachUI() {
  g_listener_guard.Reset();
  g_listener.ui_ = nullptr;
}

}  // namespace fcp::display
