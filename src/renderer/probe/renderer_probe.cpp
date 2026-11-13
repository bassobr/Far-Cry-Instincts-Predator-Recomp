// renderer_probe: standalone test program of the renderer framework.
//
// Opens a window (not full screen), creates NVRHI on Vulkan or D3D12 and draws every frame
//   left:  the built-in test triangle,
//   right: a quad with a real translated shader pair from the disc (default simpleworldspacenofog:
//          VS = position * c0..c3, texture coordinate, colour * c130; PS = texture * colour), the vertex data via
//          vertex pulling from a big-endian buffer as the game writes it, and a texture from the game data
//          through the guest path (tiled + big endian -> King Kong's untiling).
// The shaders go through ShaderLibrary::OnShaderCreated as in the create hook: pack, disk cache, else translation.
//
//   renderer_probe [--api vulkan|d3d12] [--frames N] [--dump image.bmp|png] [--width W --height H]
//                  [--pack file] [--fxo folder] [--vs name] [--ps name] [--texture file.dds|xbt] [--no-vsync]
// Switches: FCP_RENDER_BACKEND, FCP_GPU_DEBUG, FCP_GPU_WARP, FCP_RENDER_SOLID_PS/_VS (shader marking),
// FCP_DUMP_FRAMES, FCP_STATS (renderer/diagnostics.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tests/disc_util.h"
#include "kk/backend/guest_memory.h"
#include "renderer/builtin_shaders.h"
#include "renderer/diagnostics.h"
#include "renderer/host_device.h"
#include "renderer/resource_cache.h"
#include "renderer/shader_library.h"
#include "renderer/translated_layout.h"

using namespace fcp::nr;
using namespace fcp::test;

namespace {

bool g_quit = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_CLOSE || (msg == WM_KEYDOWN && wp == VK_ESCAPE)) {
    g_quit = true;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

double MsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void PutBEFloat(std::vector<uint8_t>& v, float f) {
  uint8_t b[4];
  StoreBEFloat(b, f);
  v.insert(v.end(), b, b + 4);
}

// Formatwort eines Deklarationselements (fcshaders/abi.h, vertex_format).
uint32_t FormatWord(uint32_t xenos_format, uint32_t swizzle_x, uint32_t sy, uint32_t sz, uint32_t sw) {
  using namespace fcshaders::vertex_format;
  const uint32_t swizzle = swizzle_x | (sy << 3) | (sz << 6) | (sw << 9);
  return xenos_format | (2u << kEndianShift) | (swizzle << kSwizzleShift);
}

struct Pipelines {
  nvrhi::GraphicsPipelineHandle triangle, rect;
};

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // output even on abort
  const Switches& sw = Switches::Get();
  HostApi api = sw.backend == "d3d12" ? HostApi::kD3D12 : HostApi::kVulkan;
  uint32_t frames = 120, width = 960, height = 540;
  bool vsync = true;
  std::string dump = sw.dump_frames;
  std::string pack, fxo = RepoPath("work/shaders/disc/fxo"), vs_name = "simpleworldspacenofog_mainvs_0",
                    ps_name = "simpleworldspacenofog_mainps_0", texture = RepoPath("work/renderer/common/esrb_fci.xbt");
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--api") api = next() == "d3d12" ? HostApi::kD3D12 : HostApi::kVulkan;
    else if (a == "--frames") frames = uint32_t(std::max(1, std::atoi(next().c_str())));
    else if (a == "--dump") dump = next();
    else if (a == "--width") width = uint32_t(std::atoi(next().c_str()));
    else if (a == "--height") height = uint32_t(std::atoi(next().c_str()));
    else if (a == "--pack") pack = next();
    else if (a == "--fxo") fxo = next();
    else if (a == "--vs") vs_name = next();
    else if (a == "--ps") ps_name = next();
    else if (a == "--texture") texture = next();
    else if (a == "--no-vsync") vsync = false;
    else return std::fprintf(stderr, "unknown option %s\n", a.c_str()), 2;
  }
  const auto target = api == HostApi::kVulkan ? fcshaders::PackTarget::Spirv : fcshaders::PackTarget::Dxil;
  if (pack.empty()) pack = RepoPath(std::string("work/shaders/predator.") + fcshaders::packTargetName(target) + ".fcpack");

  // --- window and device ---
  const auto t_start = std::chrono::steady_clock::now();
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"fcp_renderer_probe";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);
  RECT r{0, 0, LONG(width), LONG(height)};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  const std::wstring title = std::wstring(L"renderer_probe (") + (api == HostApi::kVulkan ? L"Vulkan" : L"D3D12") + L")";
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 80, 80, r.right - r.left,
                              r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  std::string error, note;

  ShaderLibrary library;
  ShaderLibrary::Options so;
  so.target = target;
  so.pack_path = pack;
  so.cache_dir = RepoPath("work/renderer/shader-cache");
  so.accept_stale_pack = sw.shader_pack_stale_ok;
  if (!library.Open(so, &error, &note)) return std::fprintf(stderr, "ShaderLibrary: %s\n", error.c_str()), 1;
  if (!note.empty()) std::printf("ShaderLibrary: %s\n", note.c_str());

  HostOptions ho;
  ho.api = api;
  ho.window = hwnd;
  ho.width = width;
  ho.height = height;
  ho.vsync = vsync;
  ho.debug = sw.gpu_debug;
  ho.warp = sw.gpu_warp;
  if (api == HostApi::kVulkan) ho.pipeline_cache_path = RepoPath("work/renderer/probe_vk_pipeline_cache.bin");
  auto host = CreateHostDevice(ho, &error);
  if (!host) return std::fprintf(stderr, "device: %s\n", error.c_str()), 1;
  nvrhi::IDevice* dev = host->device();
  std::printf("device: %s (%.0f ms so far)\n", host->description().c_str(), MsSince(t_start));

  TranslatedLayout layout;
  if (!layout.Init(dev, TableCapacities{}, &error)) return std::fprintf(stderr, "Layout: %s\n", error.c_str()), 1;
  ResourceCache resources(dev, layout);

  // --- shaders as in the create hook ---
  auto t0 = std::chrono::steady_clock::now();
  const std::vector<uint8_t> vs_bytes = ReadFile(fxo + "/" + vs_name + ".fxo");
  const std::vector<uint8_t> ps_bytes = ReadFile(fxo + "/" + ps_name + ".fxo");
  if (vs_bytes.empty() || ps_bytes.empty())
    return std::fprintf(stderr, "containers missing under %s (fxo.py extract)\n", fxo.c_str()), 1;
  const ShaderEntry* vs_entry = library.OnShaderCreated(0x80001000u, vs_bytes.data(), vs_bytes.size());
  const ShaderEntry* ps_entry = library.OnShaderCreated(0x80002000u, ps_bytes.data(), ps_bytes.size());
  if (!vs_entry || !ps_entry || vs_entry->source == ShaderSource::kFailed || ps_entry->source == ShaderSource::kFailed)
    return std::fprintf(stderr, "Shader: %s %s\n", vs_entry ? vs_entry->error.c_str() : "?", ps_entry ? ps_entry->error.c_str() : "?"), 1;
  std::printf("Shader: VS %016llX (%s, %zu bytes, %zu vertex bindings), PS %016llX (%s, %zu bytes, %zu samplers), %.1f ms\n",
              (unsigned long long)vs_entry->container_hash, ShaderSourceName(vs_entry->source), vs_entry->code.size(),
              vs_entry->reflection.vertexBindings.size(), (unsigned long long)ps_entry->container_hash,
              ShaderSourceName(ps_entry->source), ps_entry->code.size(), ps_entry->reflection.samplers.size(), MsSince(t0));
  nvrhi::ShaderHandle vs = layout.CreateShader(*vs_entry), ps = layout.CreateShader(*ps_entry);
  nvrhi::ShaderHandle tri_vs = CreateBuiltinShader(dev, api, BuiltinShader::kTriangleVS, &error);
  nvrhi::ShaderHandle tri_ps = CreateBuiltinShader(dev, api, BuiltinShader::kTrianglePS, &error);
  nvrhi::ShaderHandle solid_ps = CreateBuiltinShader(dev, api, BuiltinShader::kSolidPS, &error);
  if (!vs || !ps || !tri_vs || !tri_ps || !solid_ps) return std::fprintf(stderr, "shader objects: %s\n", error.c_str()), 1;
  const bool marked = sw.IsMarked(vs_entry->container_hash, ps_entry->container_hash);
  if (marked) std::printf("shader marking active: quad in signal colour %08X\n", sw.solid_color);

  // --- texture from the game data through the guest path ---
  nvrhi::CommandListHandle cl = dev->createCommandList();
  cl->open();
  uint32_t tex_index = 0;
  {
    DdsImage dds;
    RoundTrip rt;
    t0 = std::chrono::steady_clock::now();
    if (!LoadDds(texture, dds, &error) || !GuestRoundTrip(dds, true, rt, &error))
      return std::fprintf(stderr, "texture %s: %s\n", texture.c_str(), error.c_str()), 1;
    tex_index = resources.Upload(rt.host, cl, "probe texture");
    std::printf("texture: %s, %s %ux%u, %u levels, tiled+big endian -> kk ConvertTexture %.2f ms, byte-identical %s, index %u\n",
                texture.c_str(), dds.fourcc.c_str(), dds.width, dds.height, dds.levels, rt.convert_ms,
                rt.bytes_equal ? "yes" : "no", tex_index);
  }
  nvrhi::SamplerHandle sampler = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
  const uint32_t sampler_index = layout.AddSampler(sampler);

  // --- vertex data big endian, as the game writes it: position float3, UV float2, colour float4 ---
  struct V { float x, y, z, u, v, r, g, b, a; };
  const V quad[4] = {{0.08f, 0.62f, 0.5f, 0, 0, 1, 1, 1, 1},
                     {0.92f, 0.62f, 0.5f, 1, 0, 1, 1, 1, 1},
                     {0.08f, -0.62f, 0.5f, 0, 1, 1, 0.85f, 0.85f, 1},
                     {0.92f, -0.62f, 0.5f, 1, 1, 0.85f, 0.85f, 1, 1}};
  std::vector<uint8_t> vb;
  for (const V& q : quad)
    for (float f : {q.x, q.y, q.z, q.u, q.v, q.r, q.g, q.b, q.a}) PutBEFloat(vb, f);
  const uint32_t stride = 9 * 4;
  const uint32_t vb_index = resources.Buffer(0x00200000u, vb.data(), uint32_t(vb.size()), cl);
  cl->close();
  dev->executeCommandList(cl);

  fcshaders::DrawConstants dc{};
  dc.ndcScale[0] = dc.ndcScale[1] = dc.ndcScale[2] = 1.0f;
  TranslatedLayout::AdjustNdcForApi(dev->getGraphicsAPI(), dc);
  // For each VS binding (in reflection order) the matching element of our "declaration".
  for (size_t i = 0; i < vs_entry->reflection.vertexBindings.size() && i < 16; ++i) {
    const auto& b = vs_entry->reflection.vertexBindings[i];
    uint32_t* e = dc.vertexFetch[i];
    e[0] = vb_index, e[2] = stride;
    if (b.usage == fcshaders::DeclUsage::Position && b.usageIndex == 0) e[1] = 0, e[3] = FormatWord(57, 0, 1, 2, 5);
    else if (b.usage == fcshaders::DeclUsage::TexCoord && b.usageIndex == 0) e[1] = 12, e[3] = FormatWord(37, 0, 1, 4, 5);
    else if (b.usage == fcshaders::DeclUsage::Color && b.usageIndex == 0) e[1] = 20, e[3] = FormatWord(38, 0, 1, 2, 3);
    else e[3] = 0;  // missing: (0, 0, 0, 1)
  }
  for (const auto& s : ps_entry->reflection.samplers) dc.textureIndex[s.slot] = tex_index;
  for (size_t i = 0; i < ps_entry->reflection.samplers.size() && i < 32; ++i) dc.pixelSamplers[i] = sampler_index;
  const float solid[4] = {((sw.solid_color >> 24) & 255) / 255.0f, ((sw.solid_color >> 16) & 255) / 255.0f,
                          ((sw.solid_color >> 8) & 255) / 255.0f, (sw.solid_color & 255) / 255.0f};
  std::memcpy(dc.clipPlanes[0], solid, sizeof(solid));
  // VS constants as in the device shadow (little-endian floats): c0..c3 identity matrix, c130 colour 1.
  std::vector<float> vsc(1024, 0.0f), psc(1024, 0.0f);
  for (int k = 0; k < 4; ++k) vsc[k * 4 + k] = 1.0f;
  for (int k = 0; k < 4; ++k) vsc[130 * 4 + k] = 1.0f;

  // --- pipelines per target format (swapchain, dump target) ---
  auto make_pipelines = [&](const nvrhi::FramebufferInfo& info) {
    Pipelines p;
    FixedState s;
    s.raster.setCullNone();
    s.depth.depthTestEnable = false;
    s.depth.depthWriteEnable = false;
    p.triangle = layout.CreatePipeline(tri_vs, tri_ps, info, s);
    s.primitive = nvrhi::PrimitiveType::TriangleStrip;
    p.rect = layout.CreatePipeline(vs, marked ? solid_ps.Get() : ps.Get(), info, s);
    return p;
  };
  auto record = [&](nvrhi::ICommandList* c, nvrhi::IFramebuffer* fb, nvrhi::ITexture* color, const Pipelines& p) {
    c->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0.05f, 0.06f, 0.09f, 1.0f));
    const auto& d = color->getDesc();
    const float w = float(d.width), h = float(d.height);
    nvrhi::GraphicsState gs;
    gs.framebuffer = fb;
    layout.Bind(gs);
    layout.WriteConstants(c, vsc.data(), psc.data(), dc);
    gs.pipeline = p.triangle;
    gs.viewport.addViewportAndScissorRect(nvrhi::Viewport(0, w * 0.5f, 0, h, 0, 1));
    c->setGraphicsState(gs);
    c->draw(nvrhi::DrawArguments().setVertexCount(3));
    Counters::Get().draws++;
    gs.pipeline = p.rect;
    gs.viewport = nvrhi::ViewportState().addViewportAndScissorRect(nvrhi::Viewport(w * 0.5f - w * 0.5f, w, 0, h, 0, 1));
    c->setGraphicsState(gs);
    c->draw(nvrhi::DrawArguments().setVertexCount(4));
    Counters::Get().draws++;
    if (marked) Counters::Get().draws_marked++;
  };

  // Dump target (RGBA8, window size).
  nvrhi::TextureDesc dd;
  dd.width = width;
  dd.height = height;
  dd.format = nvrhi::Format::RGBA8_UNORM;
  dd.isRenderTarget = true;
  dd.initialState = nvrhi::ResourceStates::RenderTarget;
  dd.keepInitialState = true;
  dd.debugName = "dump target";
  nvrhi::TextureHandle dump_target = dev->createTexture(dd);
  nvrhi::FramebufferHandle dump_fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(dump_target));
  t0 = std::chrono::steady_clock::now();
  const Pipelines dump_pipes = make_pipelines(dump_fb->getFramebufferInfo());
  Pipelines swap_pipes;
  std::vector<nvrhi::FramebufferHandle> fbs;
  const double pipeline_ms = MsSince(t0);
  if (!dump_pipes.triangle || !dump_pipes.rect) return std::fprintf(stderr, "pipeline failed\n"), 1;

  // --- Bildschleife ---
  const auto loop_start = std::chrono::steady_clock::now();
  uint32_t frame = 0;
  for (; frame < frames && !g_quit; ++frame) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    nvrhi::ITexture* back = host->BeginFrame();
    if (!back) break;
    nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(back));
    if (!swap_pipes.rect) swap_pipes = make_pipelines(fb->getFramebufferInfo());
    cl->open();
    record(cl, fb, back, swap_pipes);
    const bool dump_now = !dump.empty() && (sw.dump_frame_at >= 0 ? frame == uint32_t(sw.dump_frame_at) : frame + 1 == frames);
    if (dump_now) record(cl, dump_fb, dump_target, dump_pipes);
    cl->close();
    dev->executeCommandList(cl);
    host->Present();
    if (dump_now) {
      std::vector<uint8_t> rgba;
      const std::string path = DumpPath(dump, frame);
      if (!ReadbackRgba8(dev, dump_target, rgba, &error) || !WriteImage(path, width, height, rgba, &error))
        std::fprintf(stderr, "Dump: %s\n", error.c_str());
      else
        std::printf("Dump: %s (frame %u)\n", path.c_str(), frame);
    }
    Counters::Get().swaps++;
  }
  const double loop_ms = MsSince(loop_start);
  host->WaitIdle();
  host->SavePipelineCache();
  const auto st = library.stats();
  std::printf("frames: %u in %.0f ms (%.2f ms per frame, vsync %s); pipelines (dump target) %.1f ms\n", frame, loop_ms,
              frame ? loop_ms / frame : 0, vsync ? "on" : "off", pipeline_ms);
  std::printf("ShaderLibrary: pack %llu, cache %llu, translated %llu, errors %llu, translating %.1f ms\n",
              (unsigned long long)st.pack_hits, (unsigned long long)st.cache_hits, (unsigned long long)st.translated,
              (unsigned long long)st.failed, st.translate_ms);
  std::printf("counters: %s\n", Counters::Get().Summary().c_str());
  const bool ok = Counters::Get().nvrhi_errors == 0 && Counters::Get().validation_errors == 0;
  // The NVRHI objects above are released before the device (host, created earlier).
  return ok ? 0 : 3;
}
