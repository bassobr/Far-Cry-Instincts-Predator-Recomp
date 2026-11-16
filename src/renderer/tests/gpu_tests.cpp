// GPU cases without a window and without the game: create a device, build the layout of the translated shaders, draw
// a test triangle into a target, read it back. Without a suitable device (or without DXC) the cases are skipped, not
// counted as failures. One case per API: Gpu_Vulkan_*, Gpu_D3D12_*.
#include <algorithm>
#include <cstdio>

#include "../core/ao_pass.h"
#include "../core/hdr_pass.h"
#include <cmath>
#include <cstdlib>
#include "fcshaders/compiler.h"
#include "kk/backend/tests/check.h"
#include "renderer/builtin_shaders.h"
#include "renderer/diagnostics.h"
#include "renderer/host_device.h"
#include "renderer/translated_layout.h"

using namespace fcp::nr;

namespace {

void DrawTriangle(HostApi api) {
  std::string error;
  if (!fcshaders::loadDxc("", &error)) {
    std::printf("  skipped: DXC missing (%s)\n", error.c_str());
    return;
  }
  HostOptions o;
  o.api = api;
  o.debug = true;  // NVRHI validation (and validation layers, if installed)
  auto host = CreateHostDevice(o, &error);
  if (!host) {
    std::printf("  skipped: no %s device (%s)\n", HostApiName(api), error.c_str());
    return;
  }
  std::printf("  %s\n", host->description().c_str());
  nvrhi::IDevice* dev = host->device();
  const uint64_t nvrhi_errors = Counters::Get().nvrhi_errors;
  TranslatedLayout layout;
  CHECK(layout.Init(dev, TableCapacities{}, &error));
  nvrhi::TextureDesc td;
  td.width = td.height = 64;
  td.format = nvrhi::Format::RGBA8_UNORM;
  td.isRenderTarget = true;
  td.initialState = nvrhi::ResourceStates::RenderTarget;
  td.keepInitialState = true;
  td.setClearValue(nvrhi::Color(0.0f, 0.0f, 0.25f, 1.0f));
  nvrhi::TextureHandle target = dev->createTexture(td);
  nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
  nvrhi::ShaderHandle vs = CreateBuiltinShader(dev, api, BuiltinShader::kTriangleVS, &error);
  nvrhi::ShaderHandle ps = CreateBuiltinShader(dev, api, BuiltinShader::kTrianglePS, &error);
  CHECK(vs && ps);
  if (!vs || !ps) return;
  FixedState state;
  state.raster.setCullNone();
  state.depth.depthTestEnable = false;
  state.depth.depthWriteEnable = false;
  nvrhi::GraphicsPipelineHandle pipeline = layout.CreatePipeline(vs, ps, fb->getFramebufferInfo(), state);
  CHECK(pipeline);
  if (!pipeline) return;
  nvrhi::CommandListHandle cl = dev->createCommandList();
  cl->open();
  cl->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.25f, 1.0f));
  nvrhi::GraphicsState gs;
  gs.pipeline = pipeline;
  gs.framebuffer = fb;
  gs.viewport.addViewportAndScissorRect(nvrhi::Viewport(64, 64));
  layout.Bind(gs);
  fcshaders::DrawConstants dc{};
  static const uint8_t zeros[4096] = {};
  layout.WriteConstants(cl, zeros, zeros, dc);
  cl->setGraphicsState(gs);
  cl->draw(nvrhi::DrawArguments().setVertexCount(3));
  cl->close();
  dev->executeCommandList(cl);
  std::vector<uint8_t> rgba;
  CHECK(ReadbackRgba8(dev, target, rgba, &error));
  if (rgba.size() != 64 * 64 * 4) return;
  auto px = [&](int x, int y) { return rgba.data() + (size_t(y) * 64 + x) * 4; };
  // Corner: clear colour (0, 0, 64); centre: inside the triangle, blended corner colours, not the clear colour.
  CHECK(px(1, 1)[0] == 0 && px(1, 1)[2] == 64);
  const uint8_t* c = px(32, 34);
  std::printf("  Mitte %u %u %u, Ecke %u %u %u\n", c[0], c[1], c[2], px(1, 1)[0], px(1, 1)[1], px(1, 1)[2]);
  CHECK(c[0] > 40 && c[1] > 40 && c[2] > 40);
  CHECK_EQ(Counters::Get().nvrhi_errors - nvrhi_errors, uint64_t(0));
}

// Bindless table slots: a full table refuses (0, counted), a freed slot is handed out again, slot 0 (fallback) never.
void TableSlots(HostApi api) {
  std::string error;
  HostOptions o;
  o.api = api;
  auto host = CreateHostDevice(o, &error);
  if (!host) {
    std::printf("  skipped: no %s device (%s)\n", HostApiName(api), error.c_str());
    return;
  }
  nvrhi::IDevice* dev = host->device();
  TableCapacities caps;
  caps.slots[size_t(Table::kTex2D)] = 16;
  caps.slots[size_t(Table::kBuffers)] = 16;
  TranslatedLayout layout;
  CHECK(layout.Init(dev, caps, &error));
  nvrhi::TextureDesc td;
  td.width = td.height = 4;
  td.format = nvrhi::Format::RGBA8_UNORM;
  td.initialState = nvrhi::ResourceStates::ShaderResource;
  td.keepInitialState = true;
  nvrhi::TextureHandle tex = dev->createTexture(td);
  nvrhi::BufferDesc bd;
  bd.byteSize = 256;
  bd.canHaveRawViews = true;
  bd.initialState = nvrhi::ResourceStates::ShaderResource;
  bd.keepInitialState = true;
  nvrhi::BufferHandle buf = dev->createBuffer(bd);
  CHECK(tex && buf);
  if (!tex || !buf) return;
  CHECK_EQ(layout.used(Table::kTex2D), 1u);  // the fallback
  std::vector<uint32_t> slots;
  for (int i = 0; i < 15; ++i) slots.push_back(layout.AddTextureView(Table::kTex2D, tex, nvrhi::ComponentMapping()));
  for (uint32_t s : slots) CHECK(s != 0);
  CHECK_EQ(layout.used(Table::kTex2D), 16u);
  CHECK_EQ(layout.AddTextureView(Table::kTex2D, tex, nvrhi::ComponentMapping()), 0u);
  CHECK_EQ(layout.refused(Table::kTex2D), uint64_t(1));
  layout.FreeSlot(Table::kTex2D, slots[3]);
  layout.FreeSlot(Table::kTex2D, 0);  // never freed
  CHECK_EQ(layout.used(Table::kTex2D), 15u);
  CHECK_EQ(layout.AddTextureView(Table::kTex2D, tex, nvrhi::ComponentMapping()), slots[3]);
  CHECK_EQ(layout.AddTextureView(Table::kTex2D, tex, nvrhi::ComponentMapping()), 0u);
  uint32_t last = 0;
  for (int i = 0; i < 15; ++i) last = layout.AddBuffer(buf);
  CHECK(last != 0);
  CHECK_EQ(layout.AddBuffer(buf), 0u);
  layout.FreeSlot(Table::kBuffers, last);
  CHECK_EQ(layout.AddBuffer(buf), last);
}

// Ambient occlusion pass (core/ao_pass.h): shaders compile and bind on the API, single- and multisampled; the sky
// (cleared depth) and a flat wall facing the camera keep their colour (white), and nothing raises an NVRHI error.
void AmbientOcclusion(HostApi api, uint32_t samples) {
  std::string error;
  if (!fcshaders::loadDxc("", &error)) {
    std::printf("  skipped: DXC missing (%s)\n", error.c_str());
    return;
  }
  HostOptions o;
  o.api = api;
  o.debug = true;
  auto host = CreateHostDevice(o, &error);
  if (!host) {
    std::printf("  skipped: no %s device (%s)\n", HostApiName(api), error.c_str());
    return;
  }
  nvrhi::IDevice* dev = host->device();
  const uint64_t nvrhi_errors = Counters::Get().nvrhi_errors;
  AoPass ao;
  CHECK(ao.Init(dev, api, &error));
  if (!ao.ok()) {
    std::printf("  %s\n", error.c_str());
    return;
  }
  const uint32_t w = 160, h = 96;
  nvrhi::TextureDesc cd;
  cd.width = w, cd.height = h;
  cd.format = nvrhi::Format::RGBA8_UNORM;
  cd.sampleCount = samples;
  cd.dimension = samples > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D;
  cd.isRenderTarget = true;
  cd.initialState = nvrhi::ResourceStates::RenderTarget;
  cd.keepInitialState = true;
  cd.setClearValue(nvrhi::Color(1.0f));
  nvrhi::TextureHandle color = dev->createTexture(cd);
  nvrhi::TextureDesc dd = cd;
  dd.format = nvrhi::Format::D32S8;
  dd.initialState = nvrhi::ResourceStates::DepthWrite;
  dd.setClearValue(nvrhi::Color(0.0f));
  nvrhi::TextureHandle depth = dev->createTexture(dd);
  nvrhi::TextureDesc rd = cd;
  rd.sampleCount = 1;
  rd.dimension = nvrhi::TextureDimension::Texture2D;
  nvrhi::TextureHandle single = dev->createTexture(rd);
  CHECK(color && depth && single);
  if (!color || !depth || !single) return;
  // PerspectiveFovRH, near 0.25, far 1000, reversed depth through the viewport (stored = 1 - z / w), as the game.
  const float n = 0.25f, f = 1000.0f, p22 = f / (n - f), p32 = n * f / (n - f);
  AoPass::Params p;
  p.depth = depth, p.color = color, p.samples = samples, p.width = w, p.height = h;
  p.left = 0, p.top = 0, p.right = int32_t(w), p.bottom = int32_t(h);
  p.view_x = 0, p.view_y = 0, p.view_w = float(w), p.view_h = float(h);
  p.x_scale = 1.0f, p.y_scale = float(w) / float(h);
  p.k0 = -p32, p.k1 = -p22 - 1.0f;
  p.far_plane = f;
  const float wall = 1.0f - (-p22 + p32 / 5.0f);  // a wall 5 m ahead (z / w = -p22 + p32 / depth)
  for (int run = 0; run < 8; ++run) {
      const int mode = 1 + (run & 1), k = (run >> 1) & 1;
      p.mode = mode;
      p.half = (run & 4) != 0;
      nvrhi::CommandListHandle cl = dev->createCommandList();
      cl->open();
      cl->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(1.0f));
      cl->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, k ? wall : 0.0f, true, 0);
      CHECK(ao.Run(cl, p));
      if (samples > 1) cl->resolveTexture(single, nvrhi::AllSubresources, color, nvrhi::AllSubresources);
      else cl->copyTexture(single, nvrhi::TextureSlice(), color, nvrhi::TextureSlice());
      cl->close();
      dev->executeCommandList(cl);
      std::vector<uint8_t> rgba;
      CHECK(ReadbackRgba8(dev, single, rgba, &error));
      if (rgba.size() != size_t(w) * h * 4) return;
      uint32_t lowest = 255, dark = 0, at = 0;
      for (size_t i = 0; i < rgba.size(); ++i)
        if (i % 4 != 3) {
          if (rgba[i] < lowest) at = uint32_t(i / 4);
          lowest = std::min<uint32_t>(lowest, rgba[i]);
          dark += rgba[i] < 245;
        }
      std::printf("  %s %s, %s, %ux: lowest channel %u (at %u,%u), %u channels below 245\n", mode == 2 ? "GTAO" : "SSAO",
                  p.half ? "half" : "full", k ? "flat wall at 5 m" : "sky", samples, lowest, at % w, at / w, dark);
      CHECK(lowest >= (k ? 245u : 255u));
    }
  CHECK_EQ(Counters::Get().nvrhi_errors - nvrhi_errors, uint64_t(0));
}

// HDR10 pass (core/hdr_pass.h): SDR sample values through the GPU into an R10G10B10A2 target, read back raw and
// compared with the CPU reference (at most 1 of 1023); fixed points: SDR white at 200 nits = PQ code 592 (FCI's
// figure), black = 0, white with full boost = the peak (1000 nits = code 769), greys stay grey.
void Hdr10Mapping(HostApi api) {
  std::string error;
  if (!fcshaders::loadDxc("", &error)) {
    std::printf("  skipped: DXC missing (%s)\n", error.c_str());
    return;
  }
  HostOptions o;
  o.api = api;
  o.debug = true;
  auto host = CreateHostDevice(o, &error);
  if (!host) {
    std::printf("  skipped: no %s device (%s)\n", HostApiName(api), error.c_str());
    return;
  }
  nvrhi::IDevice* dev = host->device();
  const uint64_t nvrhi_errors = Counters::Get().nvrhi_errors;
  HdrPass pass;
  CHECK(pass.Init(dev, api, &error));
  if (!pass.ok()) {
    std::printf("  %s\n", error.c_str());
    return;
  }
  // SDR code values (8 bit): white, 75 %, 50 %, 25 % grey, black, red, green, blue, 1/255, a skin tone
  static const uint8_t kSdr[10][3] = {{255, 255, 255}, {191, 191, 191}, {128, 128, 128}, {64, 64, 64}, {0, 0, 0},
                                      {255, 0, 0},     {0, 255, 0},     {0, 0, 255},     {1, 1, 1},   {224, 172, 140}};
  constexpr uint32_t n = 10;
  std::vector<uint8_t> rgba(n * 4);
  for (uint32_t i = 0; i < n; ++i) rgba[i * 4] = kSdr[i][0], rgba[i * 4 + 1] = kSdr[i][1], rgba[i * 4 + 2] = kSdr[i][2], rgba[i * 4 + 3] = 255;
  nvrhi::TextureDesc sd;
  sd.width = n, sd.height = 1;
  sd.format = nvrhi::Format::RGBA8_UNORM;
  sd.initialState = nvrhi::ResourceStates::ShaderResource;
  sd.keepInitialState = true;
  nvrhi::TextureHandle src = dev->createTexture(sd);
  nvrhi::TextureDesc td = sd;
  td.format = nvrhi::Format::R10G10B10A2_UNORM;
  td.isRenderTarget = true;
  td.initialState = nvrhi::ResourceStates::RenderTarget;
  td.setClearValue(nvrhi::Color(0.0f));
  nvrhi::TextureHandle dst = dev->createTexture(td);
  CHECK(src && dst);
  if (!src || !dst) return;
  nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(dst));
  struct Case {
    const char* name;
    HdrLevels levels;
  };
  Case cases[3] = {{"paper white 200, no boost", HdrLevelsFor(200, 0, 0, 0, 0, 2.2f)},
                   {"paper white 300, peak 1000, boost 1", HdrLevelsFor(300, 1000, 1, 0, 0, 2.2f)},
                   {"paper white 200, peak 600, boost 0.5", HdrLevelsFor(200, 600, 0.5f, 0, 0, 2.2f)}};
  for (int c = 0; c < 3; ++c) {
    nvrhi::CommandListHandle cl = dev->createCommandList();
    cl->open();
    cl->writeTexture(src, 0, 0, rgba.data(), n * 4);
    cl->clearTextureFloat(dst, nvrhi::AllSubresources, nvrhi::Color(0.0f));
    CHECK(pass.Run(cl, src, fb, cases[c].levels));
    cl->close();
    dev->executeCommandList(cl);
    std::vector<uint32_t> words;
    CHECK(ReadbackRaw32(dev, dst, words, &error));
    if (words.size() != n) return;
    int worst = 0;
    std::printf("  %s %s (gain x%.2f):", HostApiName(api), cases[c].name, cases[c].levels.gain);
    for (uint32_t i = 0; i < n; ++i) {
      const int got[3] = {int(words[i] & 1023), int((words[i] >> 10) & 1023), int((words[i] >> 20) & 1023)};
      const float s[3] = {kSdr[i][0] / 255.0f, kSdr[i][1] / 255.0f, kSdr[i][2] / 255.0f};
      float ref[3];
      HdrEncodeReference(s, cases[c].levels, ref);
      for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(got[k] - int(std::lround(ref[k] * 1023.0f))));
      if (i == 0 || i == 2 || i == 4)
        std::printf(" [%u] %d %d %d (%.0f nits)", i, got[0], got[1], got[2], NitsFromPq(got[1] / 1023.0f));
      if (i < 5) CHECK(got[0] == got[1] && got[1] == got[2]);  // greys stay grey
    }
    std::printf("; largest difference to the CPU reference %d of 1023\n", worst);
    CHECK(worst <= 1);
    const int white = int(words[0] & 1023), black = int(words[4] & 1023);
    CHECK_EQ(black, 0);
    if (c == 0) CHECK(white >= 591 && white <= 593);                            // 200 nits
    if (c == 1) CHECK(std::abs(white - int(std::lround(PqFromNits(1000) * 1023))) <= 1);  // boost 1: the peak
  }
  CHECK(std::abs(int(std::lround(PqFromNits(200) * 1023)) - 592) <= 0);
  CHECK(std::abs(NitsFromPq(PqFromNits(200)) - 200.0f) < 0.05f);
  CHECK_EQ(Counters::Get().nvrhi_errors - nvrhi_errors, uint64_t(0));
}

}  // namespace

TEST(Gpu_Vulkan_Hdr10Mapping) { Hdr10Mapping(HostApi::kVulkan); }
TEST(Gpu_D3D12_Hdr10Mapping) { Hdr10Mapping(HostApi::kD3D12); }
TEST(Gpu_Vulkan_AmbientOcclusion) { AmbientOcclusion(HostApi::kVulkan, 1); AmbientOcclusion(HostApi::kVulkan, 2); }
TEST(Gpu_D3D12_AmbientOcclusion) { AmbientOcclusion(HostApi::kD3D12, 1); AmbientOcclusion(HostApi::kD3D12, 2); }
TEST(Gpu_Vulkan_TriangleReadback) { DrawTriangle(HostApi::kVulkan); }
TEST(Gpu_D3D12_TriangleReadback) { DrawTriangle(HostApi::kD3D12); }
TEST(Gpu_Vulkan_TableSlots) { TableSlots(HostApi::kVulkan); }
TEST(Gpu_D3D12_TableSlots) { TableSlots(HostApi::kD3D12); }
