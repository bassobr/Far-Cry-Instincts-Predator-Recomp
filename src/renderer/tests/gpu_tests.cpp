// GPU cases without a window and without the game: create a device, build the layout of the translated shaders, draw
// a test triangle into a target, read it back. Without a suitable device (or without DXC) the cases are skipped, not
// counted as failures. One case per API: Gpu_Vulkan_*, Gpu_D3D12_*.
#include <cstdio>

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

}  // namespace

TEST(Gpu_Vulkan_TriangleReadback) { DrawTriangle(HostApi::kVulkan); }
TEST(Gpu_D3D12_TriangleReadback) { DrawTriangle(HostApi::kD3D12); }
TEST(Gpu_Vulkan_TableSlots) { TableSlots(HostApi::kVulkan); }
TEST(Gpu_D3D12_TableSlots) { TableSlots(HostApi::kD3D12); }
