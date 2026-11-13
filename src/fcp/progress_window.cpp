// Progress window of the first start (fcp/progress_window.h).
// Adapted from src/app/game_code.cpp of Far Cry Instincts Recomp (MIT, same author), which adapted it from
// SCDA-XBERecomp's src/app/game_code.cpp (MIT License, Copyright (c) TGP482 and Joshhhuaaa).
#include "fcp/progress_window.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include "fcp/env.h"
#include "fcp/log.h"

namespace fcp::progress {
namespace {

constexpr wchar_t kTitle[] = L"Far Cry Instincts Predator";
constexpr UINT kBuildDone = WM_APP + 1;

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct State {
  std::atomic<Stage> stage{Stage::kDisc};
  std::atomic<uint64_t> bytes_done{0}, bytes_total{0};
  std::atomic<uint32_t> files_done{0}, files_total{0}, first_wave{1};
  std::atomic<int64_t> compile_start{0};
  std::atomic<float> stage_fraction{0.0f};
  std::atomic<bool> cancel{false};
  std::mutex text_mutex;
  std::wstring text;
};
State g_state;

enum class WindowState { kStarting, kShown, kFailed };
std::atomic<WindowState> g_window_state{WindowState::kStarting};
std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_over{false};
std::thread g_thread;
HFONT g_font = nullptr, g_heading_font = nullptr;

// Share of the whole first start per stage (measured on a 16 core PC: disc 1 s, codegen 30 s, compile 70 s,
// link 5 s, shaders 20 s), so the bar moves about evenly.
float Fraction() {
  const float f = std::clamp(g_state.stage_fraction.load(), 0.0f, 1.0f);
  switch (g_state.stage.load()) {
    case Stage::kDisc: return 0.01f * f;
    case Stage::kCodegen: return 0.01f + 0.21f * f;
    case Stage::kCompile: {
      const uint64_t total = g_state.bytes_total;
      return 0.22f + 0.55f * (total ? float(g_state.bytes_done) / float(total) : 0.0f);
    }
    case Stage::kLink: return 0.77f + 0.06f * f;
    case Stage::kShaders: return 0.83f + 0.17f * f;
    default: return 1.0f;
  }
}

std::wstring StageText() {
  std::lock_guard lock(g_state.text_mutex);
  return g_state.text;
}

std::wstring TimeLeftText() {
  if (g_state.stage != Stage::kCompile) return {};
  if (g_state.files_done < g_state.first_wave) return L"Working out the time left";
  const uint64_t done = g_state.bytes_done, total = g_state.bytes_total;
  if (!done || done >= total) return {};
  const double elapsed = double(NowMs() - g_state.compile_start);
  const double left = elapsed * double(total - done) / double(done);
  const int minutes = int(left / 60000.0 + 0.5);
  if (minutes < 1) return L"Less than a minute left to compile";
  return L"About " + std::to_wstring(minutes) + (minutes == 1 ? L" minute" : L" minutes") + L" left to compile";
}

void DrawLine(HDC dc, RECT row, const std::wstring& text, COLORREF color, UINT align) {
  SetTextColor(dc, color);
  DrawTextW(dc, text.c_str(), int(text.size()), &row, align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

void FillBox(HDC dc, const RECT& box, COLORREF color) {
  HBRUSH brush = CreateSolidBrush(color);
  FillRect(dc, &box, brush);
  DeleteObject(brush);
}

constexpr COLORREF kBackground = RGB(14, 16, 14);
constexpr COLORREF kText = RGB(232, 236, 226);
constexpr COLORREF kDim = RGB(150, 158, 144);
constexpr COLORREF kBarEmpty = RGB(34, 40, 32);
constexpr COLORREF kBarFull = RGB(196, 160, 60);

void Paint(HDC dc, const RECT& client, int unit) {
  FillBox(dc, client, kBackground);
  SetBkMode(dc, TRANSPARENT);
  RECT heading{client.left + 2 * unit, client.top + 2 * unit, client.right - 2 * unit, client.top + 5 * unit};
  RECT stage = heading;
  OffsetRect(&stage, 0, 3 * unit);
  RECT bar{stage.left, stage.bottom + unit, stage.right, stage.bottom + unit + unit * 3 / 2};
  RECT bottom{bar.left, bar.bottom + unit / 2, bar.right, bar.bottom + unit / 2 + 3 * unit};
  SelectObject(dc, g_heading_font);
  const int percent = int(Fraction() * 100.0f);
  DrawLine(dc, heading, L"Preparing the game for your PC (one time) \x2026 " + std::to_wstring(percent) + L" %", kText,
           DT_LEFT);
  SelectObject(dc, g_font);
  DrawLine(dc, stage, StageText(), kDim, DT_LEFT);
  FillBox(dc, bar, kBarEmpty);
  RECT filled = bar;
  filled.right = bar.left + LONG(float(bar.right - bar.left) * Fraction());
  FillBox(dc, filled, kBarFull);
  DrawLine(dc, bottom, L"The game starts by itself when this is done.", kDim, DT_LEFT);
  DrawLine(dc, bottom, TimeLeftText(), kDim, DT_RIGHT);
}

int Unit(HWND hwnd) { return MulDiv(10, GetDpiForWindow(hwnd), 96); }

void PaintWindow(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT client;
  GetClientRect(hwnd, &client);
  HDC memory = CreateCompatibleDC(dc);
  HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
  HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
  HGDIOBJ old_font = SelectObject(memory, g_font);
  Paint(memory, client, Unit(hwnd));
  BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
  SelectObject(memory, old_font);
  SelectObject(memory, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(memory);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: PaintWindow(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_CLOSE: {
      const wchar_t* question =
          L"Stop preparing the game? What is done so far is kept, and the next start carries on.";
      if (MessageBoxW(hwnd, question, kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES) g_state.cancel = true;
      return 0;
    }
    case kBuildDone: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND OpenWindow() {
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.lpszClassName = L"FCPFirstStart";
  RegisterClassExW(&wc);
  const UINT dpi = GetDpiForSystem();
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, MulDiv(620, int(dpi), 96), MulDiv(150, int(dpi), 96)};
  AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
  const int w = r.right - r.left, h = r.bottom - r.top;
  const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, kTitle, style, x, y, w, h, nullptr, nullptr, inst, nullptr);
  if (!hwnd) return nullptr;
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi);
  g_font = CreateFontIndirectW(&metrics.lfMessageFont);
  LOGFONTW heading = metrics.lfMessageFont;
  heading.lfHeight = heading.lfHeight * 4 / 3;
  heading.lfWeight = FW_SEMIBOLD;
  g_heading_font = CreateFontIndirectW(&heading);
  SetTimer(hwnd, 1, 100, nullptr);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  return hwnd;
}

void WindowThread() {
  const std::string test = fcp::env::Get("FCP_TEST_PROGRESS_WINDOW");
  if (test == "hang") {
    FCPLOG("first start: FCP_TEST_PROGRESS_WINDOW=hang: the progress window's thread blocks");
    Sleep(INFINITE);
  }
  HWND hwnd = test == "fail" ? nullptr : OpenWindow();
  if (!hwnd) {
    const DWORD error = GetLastError();
    g_window_state = WindowState::kFailed;
    FCPLOG("first start: no progress window ({}, error {}); the build goes on without it",
           test.empty() ? "CreateWindowEx failed" : "FCP_TEST_PROGRESS_WINDOW=fail", error);
    return;
  }
  g_window = hwnd;
  g_window_state = WindowState::kShown;
  FCPLOG("first start: progress window shown");
  if (g_over) DestroyWindow(hwnd);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  DeleteObject(g_font);
  DeleteObject(g_heading_font);
}

}  // namespace

void Begin() {
  SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);  // no sleep while it works
  try {
    g_thread = std::thread(WindowThread);
  } catch (const std::exception&) {
    g_window_state = WindowState::kFailed;
    FCPLOG("first start: no thread for the progress window; the build goes on without it");
  }
}

void SetStage(Stage stage, const std::wstring& text) {
  {
    std::lock_guard lock(g_state.text_mutex);
    g_state.text = text;
  }
  g_state.stage_fraction = 0.0f;
  if (stage == Stage::kCompile && g_state.stage != Stage::kCompile) g_state.compile_start = NowMs();
  g_state.stage = stage;
}

void SetCompile(uint64_t done_bytes, uint64_t total_bytes, uint32_t done_files, uint32_t total_files,
                uint32_t first_wave) {
  g_state.bytes_done = done_bytes;
  g_state.bytes_total = total_bytes;
  g_state.files_done = done_files;
  g_state.files_total = total_files;
  g_state.first_wave = first_wave;
  std::lock_guard lock(g_state.text_mutex);
  g_state.text = L"Compiling the game code, file " + std::to_wstring(std::min(done_files + 1, total_files)) +
                 L" of " + std::to_wstring(total_files);
}

void SetStageFraction(float fraction) { g_state.stage_fraction = fraction; }

bool Cancelled() { return g_state.cancel.load(); }

void RequestCancel() { g_state.cancel = true; }

const char* WindowNote() {
  switch (g_window_state.load()) {
    case WindowState::kShown: return "";
    case WindowState::kFailed: return ", without the progress window";
    default: return ", the progress window not up yet";
  }
}

void End(bool ok) {
  if (ok) g_state.stage = Stage::kDone;
  g_over = true;
  if (HWND hwnd = g_window) PostMessageW(hwnd, kBuildDone, ok, 0);
  if (g_thread.joinable()) {
    // a window thread stuck in the window system is left behind (it holds nothing of the build)
    HANDLE thread = g_thread.native_handle();
    if (WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0) {
      g_thread.join();
    } else {
      FCPLOG("first start: the progress window's thread did not end; left behind");
      g_thread.detach();
    }
  }
  SetThreadExecutionState(ES_CONTINUOUS);
}

std::wstring LogTail(const std::filesystem::path& log) {
  constexpr size_t kTailBytes = 1500;
  std::ifstream f(log, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const std::streamoff size = f.tellg();
  const std::streamoff start = size > std::streamoff(kTailBytes) ? size - std::streamoff(kTailBytes) : 0;
  std::string tail(size_t(size - start), '\0');
  f.seekg(start);
  f.read(tail.data(), std::streamsize(tail.size()));
  if (start) tail.erase(0, tail.find('\n') + 1);
  const int length = MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), nullptr, 0);
  std::wstring text(size_t(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, tail.data(), int(tail.size()), text.data(), length);
  return text;
}

void ShowFailure(const std::wstring& message, const std::filesystem::path& build_log) {
  std::wstring text = L"Preparing the game for this PC failed. " + message;
  if (!build_log.empty()) {
    if (std::wstring tail = LogTail(build_log); !tail.empty()) text += L"\n\nThe last lines of the log:\n\n" + tail;
    text += L"\n\nThe full details are in " + build_log.wstring() + L".";
  }
  if (!fcp::env::Flag("FCP_NO_DIALOGS")) MessageBoxW(nullptr, text.c_str(), kTitle, MB_ICONERROR);
}

}  // namespace fcp::progress
