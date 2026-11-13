// Button prompts that follow the input device (fcp/button_prompts.h).
//
// Taken over from FCI (Far Cry Instincts Recomp, own MIT project code of the same author): the device logic of
// src/hle/prompts.cpp (auto = last used, mouse motion with a threshold, a connected pad decides before any use)
// and the drawing of src/gpu/prompt_icons.cpp (Canvas, signed distance shapes, GDI text, key caps, mouse, arrows,
// PlayStation symbols, pills, discs). Adapted: Predator's two font pages and glyph boxes (from the font file),
// silver caps with dark labels as the game's own icons, the renderer's neutral replacement interface instead of
// Direct3D 11, pc_input's bindings.
#include "fcp/button_prompts.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fcp/env.h"
#include "fcp/log.h"
#include "fcp/pc_input.h"
#include "renderer/diagnostics.h"
#include "renderer/texture_replace.h"

namespace fcp::prompts {
namespace {

// ---------------------------------------------------------------------------------------------------------------
// Which style
// ---------------------------------------------------------------------------------------------------------------

enum class Setting : uint8_t { kAuto, kXbox, kKeyboard, kPlayStation, kSwitch };

bool g_enabled = false;
std::atomic<Setting> g_setting{Setting::kAuto};
std::atomic<Style> g_last{Style::kKeyboard};   // auto: the device used last
std::atomic<bool> g_used{false};               // something was used (else a connected pad decides)
std::atomic<int> g_ps_generation{4};           // PlayStation 3/4/5: Share/Create/Select and Start/Options
std::atomic<uint32_t> g_revision{1};           // what a style's glyphs show changed (PlayStation generation)
std::atomic<uint64_t> g_switches{0};
bool g_ps_forced = false;                      // FCP_PROMPTS_PS set

const char* SettingName(Setting s) {
  switch (s) {
    case Setting::kXbox: return "xbox";
    case Setting::kKeyboard: return "keyboard";
    case Setting::kPlayStation: return "playstation";
    case Setting::kSwitch: return "switch";
    case Setting::kAuto: break;
  }
  return "auto";
}

Setting Parse(std::string v) {
  for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '"')) v.pop_back();
  while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '"')) v.erase(v.begin());
  if (v == "xbox" || v == "console") return Setting::kXbox;
  if (v == "keyboard" || v == "kbm" || v == "pc") return Setting::kKeyboard;
  if (v == "playstation" || v == "ps" || v == "ps4" || v == "ps5") return Setting::kPlayStation;
  if (v == "switch" || v == "nintendo") return Setting::kSwitch;
  if (v != "auto" && !v.empty()) FCPLOG("prompts: unknown button_prompts '{}', using auto", v);
  return Setting::kAuto;
}

void Use(Style s, const char* why) {
  g_used.store(true, std::memory_order_relaxed);
  const Style was = g_last.exchange(s);
  if (was == s) return;
  g_switches.fetch_add(1, std::memory_order_relaxed);
  if (g_setting.load() == Setting::kAuto) FCPLOG("prompts: {} ({})", Name(s), why);
}

double Now() {
  static const double frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart);
  }();
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart) / frequency;
}

// mouse motion: this many counts within kMouseWindow seconds switch to keyboard and mouse; a nudge of the desk or
// a mouse's jitter stays below (FCI's values)
constexpr double kMouseWindow = 0.25;
constexpr double kMouseCounts = 80;
std::mutex g_mouse_lock;
double g_mouse_from = 0, g_mouse_sum = 0;

Style StyleOf(PadFamily f) {
  switch (f) {
    case PadFamily::kPlayStation3:
    case PadFamily::kPlayStation4:
    case PadFamily::kPlayStation5: return Style::kPlayStation;
    case PadFamily::kSwitch: return Style::kSwitch;
    case PadFamily::kXbox: break;
  }
  return Style::kXbox;
}

void NoteGeneration(PadFamily f) {
  if (g_ps_forced) return;
  const int g = f == PadFamily::kPlayStation3 ? 3 : f == PadFamily::kPlayStation5 ? 5 : f == PadFamily::kPlayStation4 ? 4 : 0;
  if (g && g_ps_generation.exchange(g) != g) g_revision.fetch_add(1);
}

// ---------------------------------------------------------------------------------------------------------------
// The glyphs: two pages of the font "Geometr706 BdCn BT Bold 20(_hud)" (common.dat, Magma font with
// "SlimSansSerif_symbol_360.ttf" for the symbols). Boxes in texels from the font's glyph table (UV x page size),
// x1/y1 exclusive; the same in all four font files of the disc.
// ---------------------------------------------------------------------------------------------------------------

enum Role : uint8_t {
  kA, kB, kX, kY, kWhite, kBlack, kLT, kRT, kStart, kBack, kLS, kRS,
  kUp, kDown, kLeft, kRight, kLeftRight, kUpDown, kAll, kUpLeftRight, kDownLeftRight,
};
// the binding behind each single role (keyboard [bindings] / [pad] pad_bind_<name> of predator_input.toml)
const char* const kTargetOf[] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "start", "back", "ls", "rs",
                                 "up", "down", "left", "right"};
constexpr int kTargets = int(std::size(kTargetOf));

struct Cell {
  uint8_t page;
  float x0, y0, x1, y1;
  Role role;
};
// token -> character (0x82053C70) -> glyph (font table). Stick and trigger glyphs come with the language's
// letter (L/G/I/S left, R/D right); all of one side stand for the same stick or trigger. Left alone: {AS} (the
// unlabelled stick, used for movement), {TRIG} (unlabelled trigger) and every non-button glyph.
constexpr Cell kCells[] = {
    {0, 462.5f, 68.5f, 495.5f, 100.5f, kA},          // {A}  U+03A9
    {0, 462.5f, 100.5f, 495.5f, 132.5f, kB},         // {B}  U+03C0
    {0, 469.5f, 0.5f, 502.5f, 32.5f, kX},            // {X}  U+2202
    {0, 462.5f, 35.5f, 495.5f, 68.5f, kY},           // {Y}  U+2206
    {0, 421.5f, 156.5f, 459.5f, 183.5f, kBlack},     // {BL} U+2211 (RB)
    {0, 399.5f, 78.5f, 437.5f, 105.5f, kWhite},      // {WH} U+220F (LB)
    {1, 0.5f, 0.5f, 33.5f, 29.5f, kStart},           // {START} U+00A2
    {1, 0.5f, 29.5f, 33.5f, 58.5f, kBack},           // {BACK}  U+00A3
    {0, 434.5f, 0.5f, 469.5f, 35.5f, kLS},           // {ASL} U+2022
    {0, 427.5f, 39.5f, 462.5f, 74.5f, kLS},          // {ASG} U+00B6
    {0, 456.5f, 218.5f, 491.5f, 253.5f, kLS},        // {ASI} U+2260
    {0, 427.5f, 105.5f, 462.5f, 140.5f, kLS},        // {ASS} U+00D8
    {0, 456.5f, 183.5f, 491.5f, 218.5f, kRS},        // {ASR} U+221E
    {0, 421.5f, 218.5f, 456.5f, 253.5f, kRS},        // {ASD} U+00B1
    {0, 388.5f, 117.5f, 427.5f, 156.5f, kAll},       // {DP}  U+2264
    {0, 395.5f, 0.5f, 434.5f, 39.5f, kLeftRight},    // {DPLR} U+2265
    {0, 265.5f, 194.5f, 304.5f, 233.5f, kUpDown},    // {DPUD} U+00A5
    {0, 388.5f, 39.5f, 427.5f, 78.5f, kUp},          // {DPU} U+222B
    {0, 271.5f, 42.5f, 310.5f, 81.5f, kDown},        // {DPD} U+00AA
    {0, 321.5f, 78.5f, 360.5f, 117.5f, kLeft},       // {DPL} U+00BA
    {0, 356.5f, 0.5f, 395.5f, 39.5f, kRight},        // {DPR} U+00F8
    {0, 278.5f, 0.5f, 317.5f, 39.5f, kUpLeftRight},  // {DPLRU} U+00AC
    {0, 382.5f, 195.5f, 421.5f, 234.5f, kDownLeftRight},  // {DPLRD} U+221A
    {1, 33.5f, 0.5f, 60.5f, 38.5f, kLT},             // {TRIGL} U+2248
    {1, 33.5f, 38.5f, 60.5f, 76.5f, kLT},            // {TRIGG} U+25CA
    {1, 0.5f, 202.5f, 27.5f, 240.5f, kLT},           // {TRIGI} U+00A4
    {1, 0.5f, 124.5f, 27.5f, 163.5f, kLT},           // {TRIGS} U+00DE
    {1, 27.5f, 120.5f, 54.5f, 158.5f, kRT},          // {TRIGR} U+00FE
    {1, 27.5f, 82.5f, 54.5f, 120.5f, kRT},           // {TRIGD} U+00B9
};

// The pages as the renderer sees them after the conversion (standard BC2 blocks of level 0): FNV-1a 64 of the
// blocks. Two encodings of each page are on the disc (common.dat; the same glyph layout).
constexpr uint64_t kPage0Keys[] = {0x1f41960846a13e04ull, 0x9587af073ce2d6c7ull};  // 512x256
constexpr uint64_t kPage1Keys[] = {0xdb1d038a03b39cb7ull, 0xd79a4d49b9513880ull};  // 256x256
constexpr uint32_t kPageW[2] = {512, 256}, kPageH[2] = {256, 256};

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// ---------------------------------------------------------------------------------------------------------------
// Drawing (FCI src/gpu/prompt_icons.cpp)
// ---------------------------------------------------------------------------------------------------------------

struct Color {
  float r, g, b;
};
// the game's own button icons: silver caps with a dark rim, a lighter top and black letters
constexpr Color kRim{0.08f, 0.08f, 0.08f};
constexpr Color kCap{0.62f, 0.63f, 0.63f};
constexpr Color kCapTop{0.88f, 0.89f, 0.89f};
constexpr Color kLabel{0.06f, 0.06f, 0.06f};
constexpr Color kAccent{0.96f, 0.60f, 0.12f};  // the lit mouse button
constexpr Color kDisc{0.12f, 0.12f, 0.13f};
constexpr Color kDiscRim{0.70f, 0.70f, 0.70f};
constexpr Color kDiscLabel{0.95f, 0.95f, 0.93f};
constexpr Color kPsCross{0.49f, 0.64f, 0.96f}, kPsCircle{0.96f, 0.38f, 0.42f}, kPsSquare{0.90f, 0.55f, 0.82f},
    kPsTriangle{0.27f, 0.82f, 0.66f};

struct Canvas {
  int w = 0, h = 0;
  std::vector<std::array<float, 4>> px;  // straight alpha
  std::array<float, 4>& At(int x, int y) { return px[size_t(y) * w + x]; }
  void Blend(int x, int y, Color c, float a) {
    if (a <= 0 || x < 0 || y < 0 || x >= w || y >= h) return;
    a = std::min(a, 1.0f);
    auto& d = At(x, y);
    const float out = a + d[3] * (1 - a);
    if (out <= 0) return;
    const float k = d[3] * (1 - a);
    d[0] = (c.r * a + d[0] * k) / out, d[1] = (c.g * a + d[1] * k) / out, d[2] = (c.b * a + d[2] * k) / out;
    d[3] = out;
  }
};

struct Box {
  float x0, y0, x1, y1;
  float W() const { return x1 - x0; }
  float H() const { return y1 - y0; }
  float Cx() const { return (x0 + x1) / 2; }
  float Cy() const { return (y0 + y1) / 2; }
  Box Inset(float d) const { return {x0 + d, y0 + d, x1 - d, y1 - d}; }
};

template <class F>
void Fill(Canvas& c, const Box& b, Color col, F sd, float alpha = 1) {
  const int x0 = std::max(0, int(std::floor(b.x0)) - 1), x1 = std::min(c.w, int(std::ceil(b.x1)) + 1);
  const int y0 = std::max(0, int(std::floor(b.y0)) - 1), y1 = std::min(c.h, int(std::ceil(b.y1)) + 1);
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) c.Blend(x, y, col, std::clamp(0.5f - sd(x + 0.5f, y + 0.5f), 0.0f, 1.0f) * alpha);
}

float RoundRect(float x, float y, const Box& b, float r) {
  r = std::min({r, b.W() / 2, b.H() / 2});
  const float qx = std::fabs(x - b.Cx()) - (b.W() / 2 - r), qy = std::fabs(y - b.Cy()) - (b.H() / 2 - r);
  return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

float Circle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }

float Segment(float x, float y, float ax, float ay, float bx, float by, float half_width) {
  const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
  const float t = std::clamp((px * dx + py * dy) / std::max(dx * dx + dy * dy, 1e-6f), 0.0f, 1.0f);
  return std::hypot(px - dx * t, py - dy * t) - half_width;
}

// a convex polygon given clockwise in screen space (y down)
float Polygon(float x, float y, const float (*p)[2], int n) {
  float d = -1e9f;
  for (int i = 0; i < n; i++) {
    const float* a = p[i];
    const float* b = p[(i + 1) % n];
    const float ex = b[0] - a[0], ey = b[1] - a[1], len = std::max(std::hypot(ex, ey), 1e-6f);
    d = std::max(d, ((x - a[0]) * ey - (y - a[1]) * ex) / len);
  }
  return d;
}

// GDI's antialiased glyphs of a system font as coverage: the label centred in the box, as large as fits (height
// share hs of the box), condensed before it shrinks
std::mutex g_gdi_lock;
void Text(Canvas& c, const Box& b, const std::string& label, Color col, float hs = 0.62f) {
  const int w = std::max(1, int(std::lround(b.W()))), h = std::max(1, int(std::lround(b.H())));
  std::lock_guard lock(g_gdi_lock);
  HDC dc = CreateCompatibleDC(nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w, bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
  if (!dc || !bmp || !bits) {
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
    return;
  }
  HGDIOBJ old_bmp = SelectObject(dc, bmp);
  HGDIOBJ old_font = GetCurrentObject(dc, OBJ_FONT);
  std::memset(bits, 0, size_t(w) * h * 4);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  const std::wstring text(label.begin(), label.end());
  const float avail = w * 0.86f;
  int height = std::max(4, int(h * hs));
  HFONT font = nullptr;
  SIZE size{};
  TEXTMETRICW tm{};
  for (int attempt = 0; attempt < 12 && !font; attempt++) {
    LOGFONTW lf{};
    lf.lfHeight = -height;
    lf.lfWeight = FW_BOLD;
    lf.lfQuality = ANTIALIASED_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Arial");
    HFONT f = CreateFontIndirectW(&lf);
    SelectObject(dc, f);
    GetTextMetricsW(dc, &tm);
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
    if (size.cx > avail) {
      const float squeeze = std::max(0.6f, avail / float(size.cx));
      lf.lfWidth = std::max(1, int(tm.tmAveCharWidth * squeeze));
      SelectObject(dc, old_font);
      DeleteObject(f);
      f = CreateFontIndirectW(&lf);
      SelectObject(dc, f);
      GetTextMetricsW(dc, &tm);
      GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
    }
    if (size.cx <= avail || height <= 4) {
      font = f;
    } else {
      SelectObject(dc, old_font);
      DeleteObject(f);
      height = std::max(4, int(height * 0.88f));
    }
  }
  if (font) {
    const int cap = tm.tmAscent - tm.tmInternalLeading;  // capitals centred
    const int y = (h - cap) / 2 - tm.tmInternalLeading;
    TextOutW(dc, (w - size.cx) / 2, y, text.c_str(), int(text.size()));
    GdiFlush();
    const uint8_t* p = static_cast<const uint8_t*>(bits);
    const int ox = int(std::lround(b.x0)), oy = int(std::lround(b.y0));
    for (int yy = 0; yy < h; yy++)
      for (int xx = 0; xx < w; xx++) {
        const uint8_t* q = p + (size_t(yy) * w + xx) * 4;
        const float a = std::max({q[0], q[1], q[2]}) / 255.0f;
        if (a > 0) c.Blend(ox + xx, oy + yy, col, a);
      }
    SelectObject(dc, old_font);
    DeleteObject(font);
  }
  SelectObject(dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(dc);
}

// a key cap filling the box: dark rim, silver cap with a lighter top face (a key seen from slightly above)
void KeyCap(Canvas& c, const Box& b, float s) {
  const float r = 3.2f * s;
  Fill(c, b, kRim, [&](float x, float y) { return RoundRect(x, y, b, r); });
  const Box body = b.Inset(1.0f * s);
  Fill(c, body, kCap, [&](float x, float y) { return RoundRect(x, y, body, r - s); });
  const Box top{body.x0 + 0.9f * s, body.y0 + 0.6f * s, body.x1 - 0.9f * s, body.y1 - 2.0f * s};
  Fill(c, top, kCapTop, [&](float x, float y) { return RoundRect(x, y, top, r - 1.5f * s); });
}

Box KeyFace(const Box& b, float s) { return {b.x0 + 1.9f * s, b.y0 + 1.6f * s, b.x1 - 1.9f * s, b.y1 - 3.0f * s}; }

// a filled triangle pointing in a direction (0 up, 1 down, 2 left, 3 right) around a centre
void Arrow(Canvas& c, float cx, float cy, float size, int dir, Color col) {
  const float h = size * 0.5f;
  float p[3][2];
  switch (dir) {
    case 0: p[0][0] = cx, p[0][1] = cy - h, p[1][0] = cx + h, p[1][1] = cy + h * 0.6f, p[2][0] = cx - h, p[2][1] = cy + h * 0.6f; break;
    case 1: p[0][0] = cx, p[0][1] = cy + h, p[1][0] = cx - h, p[1][1] = cy - h * 0.6f, p[2][0] = cx + h, p[2][1] = cy - h * 0.6f; break;
    case 2: p[0][0] = cx - h, p[0][1] = cy, p[1][0] = cx + h * 0.6f, p[1][1] = cy - h, p[2][0] = cx + h * 0.6f, p[2][1] = cy + h; break;
    default: p[0][0] = cx + h, p[0][1] = cy, p[1][0] = cx - h * 0.6f, p[1][1] = cy + h, p[2][0] = cx - h * 0.6f, p[2][1] = cy - h; break;
  }
  const Box bb{cx - h - 1, cy - h - 1, cx + h + 1, cy + h + 1};
  Fill(c, bb, col, [&](float x, float y) { return Polygon(x, y, p, 3); });
}

// a cap with arrows for these directions (the arrow keys): one large arrow, or a cross of small ones
void ArrowCap(Canvas& c, const Box& b, float s, const std::vector<int>& dirs) {
  KeyCap(c, b, s);
  const Box f = KeyFace(b, s);
  const float m = std::min(f.W(), f.H());
  if (dirs.size() == 1) {
    Arrow(c, f.Cx(), f.Cy(), m * 0.62f, dirs[0], kLabel);
    return;
  }
  if (dirs.size() == 2 && ((dirs[0] >= 2) == (dirs[1] >= 2))) {  // left/right or up/down: side by side
    const bool horizontal = dirs[0] >= 2;
    const float off = m * 0.24f;
    for (int d : dirs) {
      const float sign = (d == 0 || d == 2) ? -1.0f : 1.0f;
      Arrow(c, f.Cx() + (horizontal ? sign * off : 0), f.Cy() + (horizontal ? 0 : sign * off), m * 0.42f, d, kLabel);
    }
    return;
  }
  const float off = m * 0.29f;
  const float dx[] = {0, 0, -off, off}, dy[] = {-off, off, 0, 0};
  for (int d : dirs) Arrow(c, f.Cx() + dx[d], f.Cy() + dy[d], m * 0.34f, d, kLabel);
}

// a mouse with the bound button lit: 1 left, 2 right, 3 middle (wheel), 4 wheel up, 5 wheel down
void Mouse(Canvas& c, const Box& b, float s, int button) {
  const float w = std::min(b.W() * 0.86f, b.H() * 0.68f), h = std::min(b.H() * 0.96f, w * 1.45f);
  const Box body{b.Cx() - w / 2, b.Cy() - h / 2, b.Cx() + w / 2, b.Cy() + h / 2};
  const float r = w * 0.46f;
  Fill(c, body, kRim, [&](float x, float y) { return RoundRect(x, y, body, r); });
  const Box in = body.Inset(1.0f * s);
  const float split = in.y0 + in.H() * 0.42f;
  auto inside = [&](float x, float y) { return RoundRect(x, y, in, r - s); };
  Fill(c, in, kCapTop, inside);
  if (button == 1 || button == 2) {
    Fill(c, in, kAccent, [&](float x, float y) {
      const float side = button == 1 ? x - in.Cx() : in.Cx() - x;
      return std::max({inside(x, y), y - split, side});
    });
  }
  const float seam = std::max(0.5f, 0.45f * s);
  Fill(c, in, kRim, [&](float x, float y) { return std::max(std::fabs(y - split) - seam, inside(x, y)); });
  Fill(c, in, kRim, [&](float x, float y) { return std::max({std::fabs(x - in.Cx()) - seam, y - split, inside(x, y)}); });
  const float ww = std::max(1.6f * s, w * 0.2f), wh = in.H() * 0.24f;
  const Box wheel{in.Cx() - ww / 2, in.y0 + in.H() * 0.1f, in.Cx() + ww / 2, in.y0 + in.H() * 0.1f + wh};
  const bool lit = button >= 3;
  Fill(c, wheel, lit ? kAccent : kCap, [&](float x, float y) { return RoundRect(x, y, wheel, ww / 2); });
  if (button == 4 || button == 5) Arrow(c, in.Cx(), split + (in.y1 - split) * 0.48f, w * 0.42f, button == 4 ? 0 : 1, kAccent);
}

// a round button: dark (face buttons of the pads) or silver (stick clicks, as the game's stick icons)
void Disc(Canvas& c, const Box& b, float s, bool dark) {
  const float r = std::min(b.W(), b.H()) / 2 - 0.3f * s;
  Fill(c, b, kRim, [&](float x, float y) { return Circle(x, y, b.Cx(), b.Cy(), r); });
  Fill(c, b, dark ? kDiscRim : kCap, [&](float x, float y) { return Circle(x, y, b.Cx(), b.Cy(), r - 0.9f * s); });
  Fill(c, b, dark ? kDisc : kCapTop, [&](float x, float y) { return Circle(x, y, b.Cx(), b.Cy(), r - 1.9f * s); });
}

void LabelDisc(Canvas& c, const Box& b, float s, const std::string& label, bool dark) {
  Disc(c, b, s, dark);
  const float r = std::min(b.W(), b.H()) / 2;
  const Box inner{b.Cx() - r * 0.72f, b.Cy() - r * 0.72f, b.Cx() + r * 0.72f, b.Cy() + r * 0.72f};
  Text(c, inner, label, dark ? kDiscLabel : kLabel, label.size() == 1 ? 0.78f : 0.62f);
}

// a shoulder button or pill (options/share, L1/R2): silver rounded box with a dark label, as the game's LB/RB
void LabelPill(Canvas& c, const Box& b, float s, const std::string& label) {
  const float h = std::min(b.H(), std::max(b.W() * 0.62f, 8.0f * s));
  const Box p{b.x0, b.Cy() - h / 2, b.x1, b.Cy() + h / 2};
  const float r = h * 0.38f;
  Fill(c, p, kRim, [&](float x, float y) { return RoundRect(x, y, p, r); });
  const Box in = p.Inset(0.9f * s);
  Fill(c, in, kCap, [&](float x, float y) { return RoundRect(x, y, in, r - 0.9f * s); });
  const Box in2 = in.Inset(0.9f * s);
  Fill(c, in2, kCapTop, [&](float x, float y) { return RoundRect(x, y, in2, r - 1.8f * s); });
  Text(c, in2.Inset(0.3f * s), label, kLabel, 0.72f);
}

// PlayStation's face buttons: the symbol in its colour on a dark disc
void PsFace(Canvas& c, const Box& b, float s, int which) {  // 0 cross, 1 circle, 2 square, 3 triangle
  Disc(c, b, s, true);
  const float r = std::min(b.W(), b.H()) / 2;
  const float k = r * 0.42f, line = std::max(0.7f, 0.95f * s);
  const float cx = b.Cx(), cy = b.Cy();
  switch (which) {
    case 0:
      Fill(c, b, kPsCross, [&](float x, float y) {
        return std::min(Segment(x, y, cx - k, cy - k, cx + k, cy + k, line), Segment(x, y, cx - k, cy + k, cx + k, cy - k, line));
      });
      break;
    case 1:
      Fill(c, b, kPsCircle, [&](float x, float y) { return std::fabs(Circle(x, y, cx, cy, k * 1.05f)) - line; });
      break;
    case 2: {
      const Box sq{cx - k * 0.9f, cy - k * 0.9f, cx + k * 0.9f, cy + k * 0.9f};
      Fill(c, b, kPsSquare, [&](float x, float y) { return std::fabs(RoundRect(x, y, sq, 0.3f * s)) - line; });
      break;
    }
    default: {
      const float p[3][2] = {{cx, cy - k * 1.1f}, {cx + k * 1.05f, cy + k * 0.72f}, {cx - k * 1.05f, cy + k * 0.72f}};
      Fill(c, b, kPsTriangle, [&](float x, float y) { return std::fabs(Polygon(x, y, p, 3)) - line; });
      break;
    }
  }
}

// ---- what a role shows ----

struct Inputs {
  int keys[kTargets] = {};       // first bound key per target (0: none)
  std::string pad[kTargets];     // the controller button behind it ("a", "lt", ...; "": none)
  int generation = 4;
};

Inputs Gather() {
  Inputs in;
  for (int t = 0; t < kTargets; t++) {
    for (int i = 0; i < fcp::pc_input::BindingCount(); i++) {
      if (fcp::pc_input::BindingName(i) != kTargetOf[t]) continue;
      const std::vector<int> keys = fcp::pc_input::BindingCodes(i);
      in.keys[t] = keys.empty() ? 0 : keys[0];
    }
    in.pad[t] = fcp::pc_input::PadSourceName(kTargetOf[t]);
  }
  in.generation = g_ps_generation.load();
  return in;
}

int ArrowDir(int vk) { return vk == VK_UP ? 0 : vk == VK_DOWN ? 1 : vk == VK_LEFT ? 2 : vk == VK_RIGHT ? 3 : -1; }
int MouseButton(int vk) {
  if (vk == VK_LBUTTON) return 1;
  if (vk == VK_RBUTTON) return 2;
  if (vk == VK_MBUTTON) return 3;
  if (vk == fcp::pc_input::WheelCode(false)) return 4;
  if (vk == fcp::pc_input::WheelCode(true)) return 5;
  return 0;
}

// what a key cap says: short names in capitals ("SPACE", "SHIFT", "ESC", "F5", "E")
std::string KeyText(int vk) {
  struct Short {
    int vk;
    const char* text;
  };
  static const Short kShort[] = {
      {VK_SPACE, "SPACE"}, {VK_RETURN, "ENTER"}, {VK_ESCAPE, "ESC"}, {VK_TAB, "TAB"}, {VK_BACK, "BKSP"},
      {VK_CAPITAL, "CAPS"}, {VK_SHIFT, "SHIFT"}, {VK_LSHIFT, "SHIFT"}, {VK_RSHIFT, "RSHIFT"}, {VK_CONTROL, "CTRL"},
      {VK_LCONTROL, "CTRL"}, {VK_RCONTROL, "RCTRL"}, {VK_MENU, "ALT"}, {VK_LMENU, "ALT"}, {VK_RMENU, "ALT GR"},
      {VK_INSERT, "INS"}, {VK_DELETE, "DEL"}, {VK_HOME, "HOME"}, {VK_END, "END"}, {VK_PRIOR, "PG UP"},
      {VK_NEXT, "PG DN"}, {VK_OEM_MINUS, "-"}, {VK_OEM_PLUS, "="}, {VK_OEM_COMMA, ","}, {VK_OEM_PERIOD, "."},
      {VK_OEM_2, "/"}, {VK_OEM_1, ";"}, {VK_OEM_7, "'"}, {VK_OEM_4, "["}, {VK_OEM_6, "]"}, {VK_OEM_5, "\\"},
      {VK_OEM_3, "`"}, {VK_MULTIPLY, "NUM *"}, {VK_ADD, "NUM +"}, {VK_SUBTRACT, "NUM -"}, {VK_DECIMAL, "NUM ."},
      {VK_DIVIDE, "NUM /"}, {VK_XBUTTON1, "M4"}, {VK_XBUTTON2, "M5"},
  };
  for (const Short& k : kShort)
    if (k.vk == vk) return k.text;
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
  std::string name = fcp::pc_input::KeyName(vk);
  for (char& ch : name) ch = char(std::toupper(static_cast<unsigned char>(ch)));
  return name;
}

// the directions of the D-pad roles (0 up, 1 down, 2 left, 3 right), in the order a label lists them (W A S D)
std::vector<int> Directions(Role r) {
  switch (r) {
    case kUp: return {0};
    case kDown: return {1};
    case kLeft: return {2};
    case kRight: return {3};
    case kLeftRight: return {2, 3};
    case kUpDown: return {0, 1};
    case kUpLeftRight: return {0, 2, 3};
    case kDownLeftRight: return {2, 1, 3};
    default: return {0, 2, 1, 3};
  }
}

void KeyGlyph(Canvas& c, const Box& b, float s, int vk) {
  if (const int m = MouseButton(vk)) {
    Mouse(c, b, s, m);
  } else if (const int a = ArrowDir(vk); a >= 0) {
    ArrowCap(c, b, s, {a});
  } else {
    KeyCap(c, b, s);
    Text(c, KeyFace(b, s), KeyText(vk), kLabel);
  }
}

// keyboard and mouse: true when drawn (false: nothing bound, the game's glyph stays)
bool DrawKeyboard(Canvas& c, const Box& b, float s, Role role, const Inputs& in) {
  if (role >= kUp) {
    const std::vector<int> dirs = Directions(role);
    std::vector<int> keys;
    bool arrows = true;
    for (int d : dirs) {
      const int vk = in.keys[int(kUp) + d];
      if (!vk) return false;
      keys.push_back(vk);
      arrows = arrows && ArrowDir(vk) == d;
    }
    if (arrows) {
      ArrowCap(c, b, s, dirs);
      return true;
    }
    if (keys.size() == 1) {
      KeyGlyph(c, b, s, keys[0]);
      return true;
    }
    // several keys: their names together (W A S D -> "WASD", else "1/2")
    std::string label;
    bool letters = true;
    for (int vk : keys) letters = letters && KeyText(vk).size() == 1;
    for (size_t i = 0; i < keys.size(); i++) label += (i && !letters ? "/" : "") + KeyText(keys[i]);
    KeyCap(c, b, s);
    Text(c, KeyFace(b, s), label, kLabel);
    return true;
  }
  const int vk = in.keys[role];
  if (!vk) return false;
  KeyGlyph(c, b, s, vk);
  return true;
}

// PlayStation and Switch: the controller button bound to the Xbox button, in that pad's names and symbols. The
// D-pad looks alike on every pad: its glyphs stay the game's
bool DrawPad(Canvas& c, const Box& b, float s, Role role, Style style, const Inputs& in) {
  if (role >= kUp) return false;
  const std::string& button = in.pad[role];
  if (button.empty()) return false;
  const bool ps = style == Style::kPlayStation;
  const float m = std::min(b.W(), b.H());
  const Box sq{b.Cx() - m / 2, b.Cy() - m / 2, b.Cx() + m / 2, b.Cy() + m / 2};
  static const char* const kFaces[] = {"a", "b", "x", "y"};
  for (int f = 0; f < 4; f++)
    if (button == kFaces[f]) {
      if (ps) PsFace(c, sq, s, f);                                                  // cross circle square triangle
      else LabelDisc(c, sq, s, f == 0 ? "B" : f == 1 ? "A" : f == 2 ? "Y" : "X", true);  // Switch: by position
      return true;
    }
  struct Name {
    const char* button;
    const char* ps;
    const char* nintendo;
    bool round;
  };
  static const Name kNames[] = {
      {"lb", "L1", "L", false}, {"rb", "R1", "R", false}, {"lt", "L2", "ZL", false}, {"rt", "R2", "ZR", false},
      {"ls", "L3", "LS", true}, {"rs", "R3", "RS", true},
  };
  std::string label;
  bool round = false;
  if (button == "start") {
    label = ps ? (in.generation == 3 ? "START" : "OPTIONS") : "+", round = !ps;
  } else if (button == "back") {
    label = ps ? (in.generation == 3 ? "SELECT" : in.generation == 5 ? "CREATE" : "SHARE") : "-", round = !ps;
  } else if (button == "up" || button == "down" || button == "left" || button == "right") {
    return false;  // a D-pad direction on a button: the game's glyph stays
  }
  for (const Name& n : kNames)
    if (button == n.button) label = ps ? n.ps : n.nintendo, round = n.round;
  if (label.empty()) return false;
  if (round) {
    LabelDisc(c, sq, s, label, false);
  } else if (b.H() > b.W() * 1.2f) {
    // the triggers' boxes are tall: a cap as the game's LT/RT, the name on its face
    KeyCap(c, b, s);
    Text(c, KeyFace(b, s), label, kLabel, 0.5f);
  } else {
    LabelPill(c, b, s, label);
  }
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The pages
// ---------------------------------------------------------------------------------------------------------------

struct Original {
  int page = 0;
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> rgba;
  uint64_t key = 0;
};

constexpr int kStyles = 4;
struct Built {
  std::shared_ptr<const nr::ReplacementImage> image;
  uint64_t revision = 0;
  int scale = 0;
  bool busy = false;
};
struct Kind {
  std::shared_ptr<const Original> original;
  Built built[kStyles];  // per style (kXbox stays empty: the game's own page)
};

std::mutex g_lock;
std::vector<Kind> g_kinds;
std::atomic<uint64_t> g_next_id{1};
std::atomic<uint64_t> g_pages_drawn{0};
bool g_dump = false, g_log_candidates = false;

// the original at s times its size, bilinear (premultiplied while filtering, so transparent texels do not darken)
std::vector<uint8_t> Upscaled(const Original& o, int s, bool nearest) {
  const int w = int(o.w) * s, h = int(o.h) * s;
  std::vector<uint8_t> out(size_t(w) * h * 4);
  if (s == 1) return o.rgba;
  if (nearest) {
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        std::memcpy(out.data() + (size_t(y) * w + x) * 4, o.rgba.data() + (size_t(y / s) * o.w + x / s) * 4, 4);
    return out;
  }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      const float fxp = (x + 0.5f) / s - 0.5f, fyp = (y + 0.5f) / s - 0.5f;
      const int x0 = int(std::floor(fxp)), y0 = int(std::floor(fyp));
      const float fx = fxp - x0, fy = fyp - y0;
      float acc[4] = {};
      for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
          const int xx = std::clamp(x0 + i, 0, int(o.w) - 1), yy = std::clamp(y0 + j, 0, int(o.h) - 1);
          const uint8_t* p = o.rgba.data() + (size_t(yy) * o.w + xx) * 4;
          const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy), a = p[3] / 255.0f;
          acc[0] += wgt * a * p[0], acc[1] += wgt * a * p[1], acc[2] += wgt * a * p[2], acc[3] += wgt * a;
        }
      uint8_t* d = out.data() + (size_t(y) * w + x) * 4;
      if (acc[3] <= 0) continue;
      for (int k = 0; k < 3; k++) d[k] = uint8_t(std::clamp(acc[k] / acc[3], 0.0f, 255.0f) + 0.5f);
      d[3] = uint8_t(std::clamp(acc[3] * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  return out;
}

std::shared_ptr<const nr::ReplacementImage> Build(const Original& o, Style style, int s, bool nearest, const Inputs& in,
                                                  int kind) {
  const auto start = std::chrono::steady_clock::now();
  const int w = int(o.w) * s, h = int(o.h) * s;
  std::vector<uint8_t> base = Upscaled(o, s, nearest);
  Canvas c;
  c.w = w, c.h = h;
  c.px.assign(size_t(w) * h, {0, 0, 0, 0});
  int drawn = 0, cells = 0;
  for (const Cell& cell : kCells) {
    if (cell.page != o.page) continue;
    ++cells;
    const Box b{cell.x0 * s, cell.y0 * s, cell.x1 * s, cell.y1 * s};
    const bool done = style == Style::kKeyboard ? DrawKeyboard(c, b, float(s), cell.role, in)
                                                : DrawPad(c, b, float(s), cell.role, style, in);
    if (!done) continue;
    ++drawn;
    // the game's glyph goes; around the box (the glyph's quad reaches a little further) its faint antialiased
    // rim too; other glyphs nearby are opaque there and stay
    const int m = 2 * s;
    const int bx0 = int(std::floor(b.x0)), by0 = int(std::floor(b.y0)), bx1 = int(std::ceil(b.x1)), by1 = int(std::ceil(b.y1));
    for (int y = std::max(0, by0 - m); y < std::min(h, by1 + m); y++)
      for (int x = std::max(0, bx0 - m); x < std::min(w, bx1 + m); x++) {
        const bool inside = x >= bx0 && x < bx1 && y >= by0 && y < by1;
        uint8_t* p = base.data() + (size_t(y) * w + x) * 4;
        if (inside || p[3] < 64) std::memset(p, 0, 4);
      }
  }
  auto img = std::make_shared<nr::ReplacementImage>();
  img->width = uint32_t(w), img->height = uint32_t(h);
  img->rgba = std::move(base);
  for (size_t i = 0; i < c.px.size(); i++) {
    const auto& p = c.px[i];
    if (p[3] <= 0) continue;
    uint8_t* d = img->rgba.data() + i * 4;
    for (int k = 0; k < 4; k++) d[k] = uint8_t(std::clamp(p[k], 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  img->id = g_next_id.fetch_add(1);
  g_pages_drawn.fetch_add(1, std::memory_order_relaxed);
  if (g_dump) {
    const std::filesystem::path path = fcp::log::Folder() / std::format("prompts_{}_{}.png", Name(style), kind);
    std::string error;
    if (!nr::WriteImage(path.string(), img->width, img->height, img->rgba, &error))
      FCPLOG("prompts: dump {} failed: {}", path.string(), error);
  }
  FCPLOG("prompts: {} glyphs of page {} (kind {}) drawn: {} of {}, {}x{} in {:.1f} ms", Name(style), o.page, kind, drawn,
         cells, w, h, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
  return img;
}

uint64_t RevisionNow() {
  return (uint64_t(fcp::pc_input::SettingsGeneration()) << 32) | g_revision.load(std::memory_order_relaxed);
}

// the page's resolution: page 0 (icons only) follows the render scale, at least twice the game's size (our
// glyphs drawn finer, the game's icons copied texel for texel, so they look as before); page 1 holds the alphabet
// of the menu font and stays at the game's size (any resampling would change every letter)
int ScaleFor(int page, float scale) {
  return page == 0 ? std::clamp(int(std::ceil(scale - 0.01f)), 2, 4) : 1;
}
// texel replication (instead of bilinear) where the game shows the page at about its own size
bool NearestFor(int s, float scale) { return std::ceil(scale - 0.01f) <= 1.0f && s == 2; }

class PageSource final : public nr::TextureReplaceSource {
 public:
  bool Wants(uint32_t width, uint32_t height) const override {
    return (width == kPageW[0] && height == kPageH[0]) || (width == kPageW[1] && height == kPageH[1]);
  }

  int Classify(uint32_t width, uint32_t height, const uint8_t* blocks, size_t block_bytes, const uint8_t* rgba) override {
    const uint64_t key = blocks ? Fnv(blocks, block_bytes) : 0;
    int page = -1;
    const char* how = "key";
    if (width == kPageW[0] && height == kPageH[0]) {
      for (uint64_t k : kPage0Keys) page = k == key ? 0 : page;
      if (page < 0 && Signature0(rgba)) page = 0, how = "signature";
    } else if (width == kPageW[1] && height == kPageH[1]) {
      for (uint64_t k : kPage1Keys) page = k == key ? 1 : page;
      if (page < 0 && Signature1(rgba)) page = 1, how = "signature";
    }
    if (g_log_candidates) FCPLOG("prompts: candidate {}x{} key {:016x}: {}", width, height, key, page >= 0 ? "glyph page" : "no");
    if (page < 0) return -1;
    const uint64_t content = Fnv(rgba, size_t(width) * height * 4);
    std::lock_guard lock(g_lock);
    for (size_t i = 0; i < g_kinds.size(); ++i)
      if (g_kinds[i].original->key == content) return int(i);
    auto o = std::make_shared<Original>();
    o->page = page, o->w = width, o->h = height, o->key = content;
    o->rgba.assign(rgba, rgba + size_t(width) * height * 4);
    g_kinds.push_back({});
    g_kinds.back().original = std::move(o);
    FCPLOG("prompts: font page {} with button glyphs found ({}x{}, by {}, kind {}); button_prompts {}, showing {}",
           page, width, height, how, g_kinds.size() - 1, SettingName(g_setting.load()), Name(prompts::Current()));
    return int(g_kinds.size() - 1);
  }

  std::shared_ptr<const nr::ReplacementImage> Current(int kind, float scale) override {
    const Style style = prompts::Current();
    std::lock_guard lock(g_lock);
    if (kind < 0 || size_t(kind) >= g_kinds.size()) return nullptr;
    Kind& k = g_kinds[size_t(kind)];
    const int s = ScaleFor(k.original->page, scale);
    const bool nearest = k.original->page == 0 && NearestFor(s, scale);
    const int variant = s * 2 + (nearest ? 1 : 0);
    const uint64_t revision = RevisionNow();
    // every style's page is drawn ahead on a worker (the current style first) at the first use of the page and
    // whenever the bindings changed, so a device switch only swaps the image; until a page is ready the older one
    // (or the game's own) is bound
    std::vector<Style> todo;
    for (int i = 1; i < kStyles; i++) {
      Built& b = k.built[i];
      if (b.busy || (b.image && b.revision == revision && b.scale == variant)) continue;
      b.busy = true;
      if (Style(i) == style) todo.insert(todo.begin(), Style(i));
      else todo.push_back(Style(i));
    }
    if (!todo.empty()) {
      std::thread([original = k.original, todo, s, nearest, variant, revision, kind] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const Inputs in = Gather();
        for (Style st : todo) {
          std::shared_ptr<const nr::ReplacementImage> img = Build(*original, st, s, nearest, in, kind);
          std::lock_guard lock(g_lock);
          Built& b = g_kinds[size_t(kind)].built[int(st)];
          b.image = std::move(img), b.revision = revision, b.scale = variant, b.busy = false;
        }
      }).detach();
    }
    if (style == Style::kXbox) return nullptr;
    return k.built[int(style)].image;
  }

 private:
  static bool Near(const uint8_t* p, int r, int g, int b) {
    return p[3] > 200 && std::abs(p[0] - r) < 70 && std::abs(p[1] - g) < 70 && std::abs(p[2] - b) < 70;
  }
  // page 0: the coloured A/B/X/Y in their places (another encoding of the same page)
  static bool Signature0(const uint8_t* rgba) {
    auto at = [&](int x, int y) { return rgba + (size_t(y) * 512 + x) * 4; };
    const uint8_t* a = at(470, 84);   // green ring of A
    const uint8_t* b = at(470, 116);  // red of B
    const uint8_t* x = at(477, 16);   // blue of X
    const uint8_t* y = at(470, 52);   // yellow of Y
    return a[1] > a[0] + 40 && a[1] > a[2] + 40 && a[3] > 200 && b[0] > b[1] + 60 && b[3] > 200 &&
           x[2] > x[0] + 60 && x[3] > 200 && y[0] > y[2] + 80 && y[1] > y[2] + 60 && y[3] > 200;
  }
  // page 1: START, BACK, LT and RT opaque silver in their places
  static bool Signature1(const uint8_t* rgba) {
    auto at = [&](int x, int y) { return rgba + (size_t(y) * 256 + x) * 4; };
    return Near(at(6, 15), 200, 200, 200) && Near(at(6, 44), 200, 200, 200) && Near(at(37, 8), 200, 200, 200) &&
           Near(at(31, 128), 200, 200, 200);
  }
};
PageSource g_source;

}  // namespace

const char* Name(Style s) {
  switch (s) {
    case Style::kXbox: return "xbox";
    case Style::kKeyboard: return "keyboard";
    case Style::kPlayStation: return "playstation";
    case Style::kSwitch: return "switch";
  }
  return "?";
}

void Configure() {
  g_enabled = fcp::env::Flag("FCP_PROMPTS", true);
  g_dump = fcp::env::Flag("FCP_PROMPTS_DUMP");
  g_log_candidates = fcp::env::Flag("FCP_PROMPTS_LOG");
  if (const int64_t g = fcp::env::Int("FCP_PROMPTS_PS", 0); g >= 3 && g <= 5) g_ps_generation = int(g), g_ps_forced = true;
  g_setting = Parse(fcp::env::Get("FCP_BUTTON_PROMPTS"));
  FCPLOG("prompts: {}, button_prompts = {} (FCP_BUTTON_PROMPTS){}", g_enabled ? "on" : "off (FCP_PROMPTS=0)",
         SettingName(g_setting.load()), g_ps_forced ? std::format(", PlayStation {}", g_ps_generation.load()) : "");
}

nr::TextureReplaceSource* Source() { return g_enabled ? &g_source : nullptr; }

Style Current() {
  switch (g_setting.load(std::memory_order_relaxed)) {
    case Setting::kXbox: return Style::kXbox;
    case Setting::kKeyboard: return Style::kKeyboard;
    case Setting::kPlayStation: return Style::kPlayStation;
    case Setting::kSwitch: return Style::kSwitch;
    case Setting::kAuto: break;
  }
  return g_last.load(std::memory_order_relaxed);
}

void NoteKeyboard() {
  if (g_last.load(std::memory_order_relaxed) != Style::kKeyboard || !g_used.load(std::memory_order_relaxed))
    Use(Style::kKeyboard, "key or mouse button");
}

void NoteMouseMotion(double dx, double dy) {
  if (g_last.load(std::memory_order_relaxed) == Style::kKeyboard) return;
  std::lock_guard lock(g_mouse_lock);
  const double now = Now();
  if (now - g_mouse_from > kMouseWindow) g_mouse_from = now, g_mouse_sum = 0;
  g_mouse_sum += std::fabs(dx) + std::fabs(dy);
  if (g_mouse_sum >= kMouseCounts) {
    g_mouse_sum = 0;
    Use(Style::kKeyboard, "mouse motion");
  }
}

void NotePad(PadFamily family) {
  NoteGeneration(family);
  const Style s = StyleOf(family);
  if (g_last.load(std::memory_order_relaxed) != s || !g_used.load(std::memory_order_relaxed)) Use(s, "controller");
}

void NotePadPresent(PadFamily family) {
  // before anything was used, a connected controller decides (a Steam Deck starts with its pad's prompts)
  if (g_used.load(std::memory_order_relaxed)) return;
  NoteGeneration(family);
  const Style s = StyleOf(family);
  if (g_last.exchange(s) != s && g_setting.load() == Setting::kAuto)
    FCPLOG("prompts: {} (a controller is connected, nothing used yet)", Name(s));
}

void Reload() {
  const Setting before = g_setting.load();
  g_setting = Parse(fcp::env::Get("FCP_BUTTON_PROMPTS"));
  if (before != g_setting.load())
    FCPLOG("prompts: button_prompts = {}, showing {}", SettingName(g_setting.load()), Name(Current()));
}

void WriteSummary() {
  if (!g_enabled) return;
  std::lock_guard lock(g_lock);
  FCPLOG("prompts: summary: {} glyph page(s) found, {} pages drawn, {} device switches, showing {} at the end",
         g_kinds.size(), g_pages_drawn.load(), g_switches.load(), Name(Current()));
}

}  // namespace fcp::prompts
