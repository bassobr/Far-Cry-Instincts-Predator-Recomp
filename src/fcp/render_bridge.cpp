#include "fcp/render_bridge.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#include <cstring>

#include <imgui.h>
#include <rex/gpu/host_presenter.h>
#include <rex/module.h>
#include <rex/platform/file.h>

#include "fcp/button_prompts.h"
#include "fcp/env.h"
#include "fcp/framerate.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp/user_paths.h"
#include "renderer/diagnostics.h"
#include "renderer/game_renderer.h"
#include "renderer/nr_api.h"
#include "renderer/overlay.h"

namespace fcp::render {
namespace {

std::unique_ptr<nr::GameRenderer> g_renderer;
nr::NrApi g_api;
std::once_flag g_base_once;
std::atomic<bool> g_presenting{false};
const char* g_backend = "off";

void LogSink(const char* line) { fcp::log::Line(line); }

// [render] backend = "..." in renderer.toml (only that key; the rest of the file is the renderer's).
std::string BackendFromConfig(const std::filesystem::path& file) {
  std::ifstream in(file);
  std::string line;
  bool section = false;
  while (std::getline(in, line)) {
    const size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line[start] == '#') continue;
    if (line[start] == '[') {
      section = line.compare(start, 8, "[render]") == 0;
      continue;
    }
    if (section && line.compare(start, 7, "backend") == 0) {
      const size_t q1 = line.find('"'), q2 = line.find('"', q1 + 1);
      if (q1 != std::string::npos && q2 != std::string::npos) return line.substr(q1 + 1, q2 - q1 - 1);
    }
  }
  return {};
}

// Render scale: "1" (console), "auto" (scale only: output height / 720), "150%", "1.5", "2x". 0.5 to 4.
float ParseScale(std::string v, const char* name, bool allow_auto, bool* is_auto) {
  if (is_auto) *is_auto = false;
  for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  while (!v.empty() && (v.back() == ' ' || v.back() == 'x')) v.pop_back();
  if (v.empty()) return 1.0f;
  if (v == "auto") {
    if (allow_auto && is_auto) return *is_auto = true, 1.0f;
    FCPLOG("renderer: {}=auto not supported here, 1 taken", name);
    return 1.0f;
  }
  const bool percent = v.back() == '%';
  if (percent) v.pop_back();
  char* end = nullptr;
  double f = std::strtod(v.c_str(), &end);
  if (!end || end == v.c_str() || *end) {
    FCPLOG("renderer: {}={}: not a factor (1, 1.5, 150%{}), 1 taken", name, v, allow_auto ? ", auto" : "");
    return 1.0f;
  }
  if (percent) f /= 100.0;
  const double c = std::clamp(f, 0.5, 4.0);
  if (c != f) FCPLOG("renderer: {}={} out of 0.5..4, {} taken", name, f, c);
  return float(c);
}

// Hor+: "auto" (or empty) = the output's aspect when wider than 16:9; "16:9" / "off" = as
// before (pillarbox); "21:9", "32:9", "3440:1440", "64/27" or a ratio such as 2.39 = that shape for the 3D view.
float ParseAspect(std::string v) {
  for (char& ch : v) ch = char(std::tolower(static_cast<unsigned char>(ch)));
  while (!v.empty() && v.back() == ' ') v.pop_back();
  while (!v.empty() && v.front() == ' ') v.erase(v.begin());
  if (v.empty() || v == "auto") return 0.0f;
  if (v == "off" || v == "0" || v == "false") return 16.0f / 9.0f;
  double w = 0, h = 1;
  char sep = 0;
  if (std::sscanf(v.c_str(), "%lf%c%lf", &w, &sep, &h) == 3 && (sep == ':' || sep == '/' || sep == 'x')) {
  } else if (std::sscanf(v.c_str(), "%lf", &w) == 1) {
    h = 1;
  } else {
    w = 0;
  }
  if (!(w > 0) || !(h > 0) || w / h < 1.0 || w / h > 8.0) {
    FCPLOG("renderer: FCP_RENDER_ASPECT={}: not an aspect (auto, 16:9, 21:9, 32:9, 2.39), auto taken", v);
    return 0.0f;
  }
  return float(w / h);
}

bool CreateRenderer() {
  if (g_renderer) return true;
  nr::SetLogSink(&LogSink);
  const std::filesystem::path exe = rex::platform::GetExecutableFolder();
  nr::GameRendererOptions o;
  o.layout_path = (exe / "renderer.toml").string();
  std::string backend = fcp::env::Get("FCP_RENDER_BACKEND");
  const char* backend_from = "FCP_RENDER_BACKEND";
  if (backend.empty()) backend = BackendFromConfig(exe / "renderer.toml"), backend_from = "renderer.toml";
  if (backend.empty()) backend = "vulkan", backend_from = "default";
  o.api = backend == "d3d12" ? nr::HostApi::kD3D12 : nr::HostApi::kVulkan;
  const char* target = o.api == nr::HostApi::kVulkan ? "spirv" : "dxil";
  o.debug = fcp::env::Flag("FCP_GPU_DEBUG");
  // Over 60 frames/s (FCP_FPS=120/unlimited) the host present must not wait for the monitor: EndPresent holds
  // the renderer mutex during Present, a FIFO wait there stalls the game's draws (measured at 120 on a 120 Hz
  // monitor: every third frame 16 ms). The game paces itself (vblank wait or limiter, fcp/framerate.h).
  o.vsync = fcp::env::Flag("FCP_RENDER_VSYNC", !fcp::framerate::HostPresentImmediate());
  o.shader_pack = fcp::env::Get("FCP_SHADER_PACK");
  if (o.shader_pack.empty()) o.shader_pack = (exe / "shaders" / (std::string("predator.") + target + ".fcpack")).string();
  std::error_code ec;
  if (!std::filesystem::exists(o.shader_pack, ec)) {
    FCPLOG("renderer: no shader pack at {} (shaders are translated at run time and cached)", o.shader_pack);
    o.shader_pack.clear();
  }
  const std::filesystem::path cache = fcp::paths::Cache();  // user folder (fcp/user_paths.h)
  o.shader_cache = fcp::env::Get("FCP_SHADER_CACHE");
  if (o.shader_cache.empty()) o.shader_cache = (cache / "shaders").string();
  std::filesystem::create_directories(o.shader_cache, ec);
  o.dxc_path = exe.string();
  if (o.api == nr::HostApi::kVulkan) o.driver_pipeline_cache = (cache / "vk_pipelines.bin").string();
  o.pipeline_list = fcp::env::Get("FCP_PIPELINE_LIST");
  if (o.pipeline_list.empty()) o.pipeline_list = (cache / (std::string("pipelines.") + target + ".bin")).string();
  o.pipeline_prewarm = fcp::env::Flag("FCP_PIPELINE_PREWARM", true);
  o.dump_pattern = fcp::env::Get("FCP_DUMP_FRAMES");
  // "%g" = relaunch generation (chains of level transitions: every process writes its own files).
  for (size_t at; (at = o.dump_pattern.find("%g")) != std::string::npos;)
    o.dump_pattern.replace(at, 2, std::to_string(fcp::relaunch::Generation()));
  if (!o.dump_pattern.empty() && std::filesystem::path(o.dump_pattern).is_relative())
    o.dump_pattern = (fcp::log::Folder() / o.dump_pattern).string();
  o.dump_frames = fcp::env::Get("FCP_DUMP_FRAME_AT");
  o.dump_every = uint32_t(fcp::env::Int("FCP_DUMP_FRAME_EVERY", 0));
  o.dump_present = fcp::env::Flag("FCP_DUMP_PRESENT", false);
  if (const int64_t n = fcp::env::Int("FCP_DUMP_RESOLVES", -1); n >= 0) o.dump_resolves_frame = uint64_t(n);
  {
    const std::string list = fcp::env::Get("FCP_RENDER_SKIP_PS");
    for (size_t pos = 0; pos < list.size();) {
      const size_t comma = list.find(',', pos);
      const std::string part = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      if (!part.empty()) o.skip_ps.insert(std::strtoull(part.c_str(), nullptr, 16));
      if (comma == std::string::npos) break;
      pos = comma + 1;
    }
  }
  o.stats_every = uint32_t(fcp::env::Int("FCP_RENDER_STATS", 150));
  o.texture_check = fcp::env::Flag("FCP_RENDER_TEXTURE_CHECK", true);
  o.texture_watch = fcp::env::Flag("FCP_RENDER_TEXTURE_WATCH", true);
  {
    const std::string lr = fcp::env::Get("FCP_RENDER_LOCK_READBACK");
    o.lock_readback = lr != "0";
    o.lock_readback_async = lr != "sync";
  }
  o.buffer_watch = fcp::env::Flag("FCP_RENDER_BUFFER_WATCH", true);
  o.gamma_ramp = fcp::env::Flag("FCP_RENDER_GAMMA", true);
  {
    // [graphics] msaa: console (= 1/true, default) | off (= 0/false) | 2 | 4 | 8
    std::string m = fcp::env::Get("FCP_RENDER_MSAA");
    for (char& ch : m) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    o.msaa = !(m == "0" || m == "off" || m == "false" || m == "no");
    if (m == "2" || m == "4" || m == "8") o.msaa_samples = uint32_t(std::stoul(m));
    else if (!m.empty() && m != "1" && m != "true" && m != "on" && m != "yes" && m != "console" && o.msaa)
      FCPLOG("renderer: FCP_RENDER_MSAA={} unknown (console, off, 2, 4, 8): console", m);
  }
  o.smaa = fcp::env::Flag("FCP_RENDER_SMAA", false);
  o.constant_versions = uint32_t(std::max<int64_t>(0, fcp::env::Int("FCP_RENDER_CB_VERSIONS", 0)));
  if (!fcp::env::Get("FCP_TRACE_HITCHES").empty()) {
    const int64_t ms = fcp::env::Int("FCP_TRACE_HITCHES", 50);
    o.slow_frame_ms = double(ms <= 1 ? 50 : ms);  // 1 = 50 ms, as in gametime.cpp
  }
  o.element_endian = fcp::env::Flag("FCP_RENDER_ELEMENT_ENDIAN", true);
  // Asynchronous queries: the present thread waits for the GPU without the renderer lock; the game's
  // swap applies the back pressure instead (at most FCP_RENDER_FRAMES_AHEAD swaps open on the GPU).
  o.present_unlocked = fcp::env::Flag("FCP_RENDER_PRESENT_UNLOCKED", true);
  o.frames_ahead = uint32_t(std::clamp<int64_t>(fcp::env::Int("FCP_RENDER_FRAMES_AHEAD", 2), 0, 8));
  o.fast_constants = fcp::env::Flag("FCP_RENDER_FAST_CONSTANTS", true);
  o.index_cache = fcp::env::Flag("FCP_RENDER_INDEX_CACHE", true);
  o.ring_batch = fcp::env::Flag("FCP_RENDER_RING_BATCH", true);
  if (const std::string t = fcp::env::Get("FCP_RENDER_TRACE"); !t.empty()) {
    unsigned long long first = 0, count = 1;
    if (std::sscanf(t.c_str(), "%llu,%llu", &first, &count) >= 1) o.trace_first = first, o.trace_count = count ? count : 1;
  }
  if (const std::string pr = fcp::env::Get("FCP_RENDER_PROBE"); !pr.empty()) {
    int x = -1, y = -1;
    if (std::sscanf(pr.c_str(), "%d,%d", &x, &y) == 2) o.probe_x = x, o.probe_y = y;
  }
  o.trace_consts = std::strtoull(fcp::env::Get("FCP_TRACE_CONSTS").c_str(), nullptr, 16);
  o.conditional_mode = uint32_t(fcp::env::Int("FCP_RENDER_CONDITIONAL", 0));
  o.depth_resolve = fcp::env::Flag("FCP_RENDER_DEPTH_RESOLVE", true);
  {
    const std::string q = fcp::env::Get("FCP_RENDER_QUERY");
    o.query_mode = q == "original" ? 0u : q == "visible" ? 1u : 2u;  // default: host
  }
  // Graphics extras, [graphics] in predator.toml; default = console.
  {
    bool is_auto = false;
    o.scale = ParseScale(fcp::env::Get("FCP_RENDER_SCALE"), "FCP_RENDER_SCALE", true, &is_auto);
    o.scale_auto = is_auto;
    o.shadow_scale = ParseScale(fcp::env::Get("FCP_RENDER_SHADOW_SCALE"), "FCP_RENDER_SHADOW_SCALE", false, nullptr);
    o.scale_min_width = uint32_t(std::clamp<int64_t>(fcp::env::Int("FCP_RENDER_SCALE_MIN_WIDTH", 640), 1, 8192));
    o.aniso = uint32_t(std::clamp<int64_t>(fcp::env::Int("FCP_RENDER_ANISO", 1), 1, 16));
    o.aspect = ParseAspect(fcp::env::Get("FCP_RENDER_ASPECT"));
    o.trace_wide = fcp::env::Flag("FCP_TRACE_WIDE", false);
  }
  o.log = &LogSink;
  FCPLOG("renderer: backend {} ({}), debug {}, vsync {}, pack {}, cache {}", backend, backend_from, o.debug, o.vsync,
         o.shader_pack.empty() ? "-" : o.shader_pack, o.shader_cache);
  std::string error;
  g_renderer = nr::GameRenderer::Create(o, &error);
  if (!g_renderer) {
    FCPLOG("renderer: start failed: {} (the hooks run the originals only)", error);
    return false;
  }
  g_api = nr::MakeNrApi(*g_renderer);
  nr::SetNrApi(&g_api);
  // button prompts for the device in use: the renderer replaces the font pages
  fcp::prompts::Configure();
  if (nr::TextureReplaceSource* prompts = fcp::prompts::Source()) g_renderer->SetTextureReplaceSource(prompts);
  g_backend = o.api == nr::HostApi::kD3D12 ? "d3d12" : "vulkan";
  return true;
}

// Write watch of the SDK (rexglue change 12) for the renderer's texture cache.
class WriteListener final : public rex::IPhysicalWriteListener {
 public:
  rex::PhysicalRange OnPhysicalWrite(rex::PhysicalRange range, bool) override {
    if (g_renderer) g_renderer->OnGuestWrite(range.address, range.length);
    return range;
  }
};
WriteListener g_write_listener;
rex::IPhysicalWriteWatch* g_watch = nullptr;
rex::ScopeGuard g_watch_guard;

void WatchRange(void*, uint32_t physical, uint32_t bytes) {
  if (g_watch) g_watch->WatchPhysicalWrites(rex::PhysicalRange{physical, bytes});
}

// ---- SDK overlays (ImGui 1.92) into the renderer's neutral overlay frame (renderer/overlay.h) ----
// Present thread, between BeginPresent and EndPresent; the draw data is valid only during the call. Texture
// requests (the font atlas) first: create / update (whole texture again) / destroy, as ImGui's own backends.
// FCP_OVERLAY=0: the SDK overlays are not drawn (the state before this change).
struct OverlayScratch {
  std::vector<nr::OverlayVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<nr::OverlayCommand> commands;
  std::vector<nr::OverlayTextureOp> ops;
  std::vector<ImTextureData*> op_textures;
  std::vector<std::vector<uint8_t>> pixels;  // Alpha8 expanded to RGBA8
} g_overlay;

void DrawImGui(ImDrawData& draw) {
  static const bool enabled = fcp::env::Flag("FCP_OVERLAY", true);
  if (!g_renderer || !enabled) return;
  OverlayScratch& s = g_overlay;
  s.ops.clear();
  s.op_textures.clear();
  size_t px = 0;
  if (draw.Textures) {
    for (ImTextureData* t : *draw.Textures) {
      if (!t || t->Status == ImTextureStatus_OK || t->Status == ImTextureStatus_Destroyed) continue;
      nr::OverlayTextureOp op;
      op.id = uint64_t(t->TexID);
      op.width = uint32_t(t->Width);
      op.height = uint32_t(t->Height);
      if (t->Status == ImTextureStatus_WantDestroy) {
        if (t->UnusedFrames <= 0) continue;  // still used by a frame in flight
        op.kind = nr::OverlayTextureOp::Kind::kDestroy;
      } else {
        op.kind = t->Status == ImTextureStatus_WantCreate || t->TexID == ImTextureID_Invalid
                      ? nr::OverlayTextureOp::Kind::kCreate
                      : nr::OverlayTextureOp::Kind::kUpdate;
        if (t->Format == ImTextureFormat_RGBA32) {
          op.rgba = static_cast<const uint8_t*>(t->GetPixels());
        } else {
          if (s.pixels.size() <= px) s.pixels.resize(px + 1);
          std::vector<uint8_t>& out = s.pixels[px++];
          const auto* a = static_cast<const uint8_t*>(t->GetPixels());
          out.resize(size_t(t->Width) * t->Height * 4);
          for (size_t i = 0, n = size_t(t->Width) * t->Height; i < n; ++i) {
            out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = 255;
            out[i * 4 + 3] = a[i];
          }
          op.rgba = out.data();
        }
      }
      s.ops.push_back(op);
      s.op_textures.push_back(t);
    }
  }
  s.vertices.clear();
  s.indices.clear();
  s.commands.clear();
  for (int l = 0; l < draw.CmdListsCount; ++l) {
    const ImDrawList* list = draw.CmdLists[l];
    const uint32_t base = uint32_t(s.vertices.size());
    const size_t first = s.vertices.size();
    s.vertices.resize(first + size_t(list->VtxBuffer.Size));
    std::memcpy(s.vertices.data() + first, list->VtxBuffer.Data, size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
    for (const ImDrawCmd& c : list->CmdBuffer) {
      if (c.UserCallback || !c.ElemCount) continue;  // no callbacks in the SDK's dialogs
      nr::OverlayCommand oc;
      oc.clip[0] = c.ClipRect.x, oc.clip[1] = c.ClipRect.y, oc.clip[2] = c.ClipRect.z, oc.clip[3] = c.ClipRect.w;
      oc.texture = uint64_t(c.TexRef._TexData ? c.TexRef._TexData->TexID : c.TexRef._TexID);
      oc.first_index = uint32_t(s.indices.size());
      oc.index_count = c.ElemCount;
      for (uint32_t i = 0; i < c.ElemCount; ++i)
        s.indices.push_back(base + c.VtxOffset + uint32_t(list->IdxBuffer.Data[c.IdxOffset + i]));
      if (oc.texture) s.commands.push_back(oc);  // a texture not yet created: drawn from the next frame on
    }
  }
  nr::OverlayFrame f;
  f.display_pos[0] = draw.DisplayPos.x, f.display_pos[1] = draw.DisplayPos.y;
  f.display_size[0] = draw.DisplaySize.x, f.display_size[1] = draw.DisplaySize.y;
  f.vertices = s.vertices.data();
  f.vertex_count = s.vertices.size();
  f.indices = s.indices.data();
  f.index_count = s.indices.size();
  f.commands = s.commands.data();
  f.command_count = s.commands.size();
  g_renderer->RenderOverlay(f, s.ops.data(), s.ops.size());
  for (size_t i = 0; i < s.ops.size(); ++i) {
    if (!s.ops[i].done) continue;
    ImTextureData* t = s.op_textures[i];
    if (s.ops[i].kind == nr::OverlayTextureOp::Kind::kDestroy) {
      t->SetTexID(ImTextureID_Invalid);
      t->SetStatus(ImTextureStatus_Destroyed);
    } else {
      t->SetTexID(ImTextureID(s.ops[i].id));
      t->SetStatus(ImTextureStatus_OK);
    }
  }
}

class PresenterModule final : public rex::IModule, public rex::gpu::IHostPresenter {
 public:
  void AttachTitleMemory(rex::IPhysicalMemory&, rex::IPhysicalWriteWatch& watch) override {
    if (!CreateRenderer()) return;
    g_watch_guard.Reset();
    g_watch = &watch;
    g_watch_guard = watch.AddWriteListener(g_write_listener);
    g_renderer->SetWriteWatch(&WatchRange, nullptr);
  }
  void DetachTitle() override {
    if (g_renderer) g_renderer->SetWriteWatch(nullptr, nullptr);
    g_watch_guard.Reset();
    g_watch = nullptr;
  }
  REX_MODULE_SERVES(rex::gpu::IHostPresenter)

  std::string_view GetName() const override { return "fcp-renderer"; }
  [[nodiscard]] rex::Result<void> Connect(rex::IInterfaceFactory&) override { return rex::Ok(); }
  void Disconnect() override {}
  [[nodiscard]] rex::Result<void> Init() override {
    CreateRenderer();
    return rex::Ok();
  }
  void Shutdown() override {}
  rex::IInterface* QueryInterface(std::string_view name, rex::u32 version) override {
    return rex::FindModuleInterface<rex::gpu::IHostPresenter>(*this, name, version);
  }

  // IHostPresenter
  [[nodiscard]] rex::Result<void> SetSurface(const rex::NativeWindow& window) override {
    if (!CreateRenderer()) return rex::Ok();  // without a renderer the window stays empty, as in the boot
    if (window.type != rex::NativeWindow::Type::kWin32 || !window.window) {
      FCPLOG("renderer: window type {} not supported, no presentation", int(window.type));
      return rex::Ok();
    }
    HWND hwnd = static_cast<HWND>(window.window);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    std::string error;
    if (!g_renderer->AttachWindow(hwnd, uint32_t(rc.right - rc.left), uint32_t(rc.bottom - rc.top), &error))
      FCPLOG("renderer: window device failed: {}", error);
    else
      g_presenting = true;
    return rex::Ok();
  }
  void ResizeSurface(rex::u32 width, rex::u32 height) override {
    if (g_renderer) g_renderer->Resize(width, height);
  }
  [[nodiscard]] bool BeginPresent() override { return g_renderer && g_renderer->BeginPresent(); }
  void RenderOverlays(ImDrawData& draw) override { DrawImGui(draw); }
  void EndPresent() override {
    if (g_renderer) g_renderer->EndPresent();
  }
};

PresenterModule g_module;

}  // namespace

bool Enabled() {
  static const bool on = fcp::env::Flag("FCP_RENDERER", true);
  return on;
}

void RegisterModule(std::vector<rex::IModule*>& modules) {
  if (Enabled()) modules.push_back(&g_module);
}

void SetGuestBase(uint8_t* base) {
  if (!g_renderer || !base) return;
  std::call_once(g_base_once, [base] { g_renderer->SetGuestBase(base); });
}

void Shutdown() {
  if (g_renderer) g_renderer->Shutdown();
  fcp::prompts::WriteSummary();
}

bool Presenting() { return g_presenting.load(); }

const char* BackendName() { return g_backend; }

void RequestPresentDump(const std::string& path) {
  if (!g_renderer) return;
  std::filesystem::path p(path);
  if (p.is_relative()) p = fcp::log::Folder() / p;
  g_renderer->RequestPresentDump(p.string());
}

void SetGammaRamp(bool on) {
  if (g_renderer) g_renderer->SetGammaRamp(on);
}

void SetScale(const std::string& value) {
  if (!g_renderer) return;
  bool is_auto = false;
  const float f = ParseScale(value, "[graphics] scale", true, &is_auto);
  g_renderer->SetScale(f, is_auto);
}

void SetSmaa(bool on) {
  if (g_renderer) g_renderer->SetSmaa(on);
}

float HorPlusWide() { return g_renderer ? g_renderer->Wide() : 1.0f; }

}  // namespace fcp::render
