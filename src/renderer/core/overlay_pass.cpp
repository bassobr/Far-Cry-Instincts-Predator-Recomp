// OverlayPass (core/overlay_pass.h). This project's own code; flow as in ImGui's renderer backends (MIT):
// projection from DisplayPos/DisplaySize, one draw per command with a scissor rectangle, alpha blending, no depth.
#include "overlay_pass.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "fcshaders/compiler.h"

namespace fcp::nr {

namespace {

// Vulkan: texture on binding 0, sampler on 128 (NVRHI's default sampler offset), projection as push constants.
// D3D12: t0, s0, root constants b0. Vertex shader without -fvk-invert-y: NVRHI flips the viewport (like the blits).
constexpr const char* kShader = R"(
struct Projection { float2 scale; float2 translate; };
[[vk::push_constant]] ConstantBuffer<Projection> pc : register(b0);
[[vk::binding(0, 0)]] Texture2D<float4> tex : register(t0);
[[vk::binding(128, 0)]] SamplerState smp : register(s0);
struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VsOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };
VsOut vs_main(VsIn i) {
  VsOut o;
  o.pos = float4(i.pos * pc.scale + pc.translate, 0.0, 1.0);
  o.col = i.col;
  o.uv = i.uv;
  return o;
}
float4 ps_main(VsOut i) : SV_Target0 { return i.col * tex.Sample(smp, i.uv); }
)";

struct Projection {
  float scale[2];
  float translate[2];
};

}  // namespace

bool OverlayPass::Init(nvrhi::IDevice* device, HostApi api, std::string* error) {
  dev_ = device;
  fcshaders::Compiler compiler;
  if (!compiler.ok()) {
    if (error) *error = "DXC not loaded";
    return false;
  }
  const auto target = api == HostApi::kVulkan ? fcshaders::CompileTarget::Spirv : fcshaders::CompileTarget::Dxil;
  const fcshaders::CompileOutput vs = compiler.compileEntry(kShader, L"vs_6_0", L"vs_main", target, false);
  const fcshaders::CompileOutput ps = compiler.compileEntry(kShader, L"ps_6_0", L"ps_main", target, false);
  if (!vs.ok || !ps.ok) {
    if (error) *error = "overlay shader: " + vs.messages + ps.messages;
    return false;
  }
  vs_ = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName("vs_main").setDebugName("overlay vs"),
                           vs.blob.data(), vs.blob.size());
  ps_ = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName("ps_main").setDebugName("overlay ps"),
                           ps.blob.data(), ps.blob.size());
  if (!vs_ || !ps_) {
    if (error) *error = "overlay shader: createShader failed";
    return false;
  }
  const nvrhi::VertexAttributeDesc attrs[] = {
      nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(0).setElementStride(sizeof(OverlayVertex)),
      nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(8).setElementStride(sizeof(OverlayVertex)),
      nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(16).setElementStride(sizeof(OverlayVertex)),
  };
  input_ = dev_->createInputLayout(attrs, 3, vs_);
  nvrhi::BindingLayoutDesc ld;
  ld.setVisibility(nvrhi::ShaderType::All);
  ld.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Projection)));
  ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
  ld.addItem(nvrhi::BindingLayoutItem::Sampler(0));
  layout_ = dev_->createBindingLayout(ld);
  sampler_ = dev_->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
  ok_ = input_ && layout_ && sampler_;
  if (!ok_ && error) *error = "overlay: input layout/binding layout/sampler failed";
  return ok_;
}

uint64_t OverlayPass::CreateTexture(nvrhi::ICommandList* cl, uint32_t width, uint32_t height, const uint8_t* rgba) {
  if (!ok_ || !width || !height) return 0;
  nvrhi::TextureDesc d;
  d.width = width;
  d.height = height;
  d.format = nvrhi::Format::RGBA8_UNORM;
  d.initialState = nvrhi::ResourceStates::ShaderResource;
  d.keepInitialState = true;
  d.debugName = "overlay texture";
  Tex t;
  t.texture = dev_->createTexture(d);
  if (!t.texture) return 0;
  t.width = width;
  t.height = height;
  nvrhi::BindingSetDesc sd;
  sd.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Projection)));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, t.texture));
  sd.addItem(nvrhi::BindingSetItem::Sampler(0, sampler_));
  t.set = dev_->createBindingSet(sd, layout_);
  if (!t.set) return 0;
  cl->writeTexture(t.texture, 0, 0, rgba, size_t(width) * 4);
  const uint64_t id = next_id_++;
  textures_.emplace(id, std::move(t));
  return id;
}

void OverlayPass::UpdateTexture(nvrhi::ICommandList* cl, uint64_t id, const uint8_t* rgba) {
  auto it = textures_.find(id);
  if (it == textures_.end()) return;
  cl->writeTexture(it->second.texture, 0, 0, rgba, size_t(it->second.width) * 4);
}

void OverlayPass::DestroyTexture(uint64_t id) { textures_.erase(id); }  // NVRHI keeps it alive until the GPU is done

bool OverlayPass::EnsureBuffers(size_t vertices, size_t indices) {
  auto make = [&](nvrhi::BufferHandle& b, size_t& cap, size_t bytes, bool index) {
    if (b && cap >= bytes) return true;
    size_t n = std::max<size_t>(cap ? cap : 64 * 1024, 64 * 1024);
    while (n < bytes) n *= 2;
    nvrhi::BufferDesc d;
    d.byteSize = n;
    d.isVertexBuffer = !index;
    d.isIndexBuffer = index;
    d.initialState = index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::VertexBuffer;
    d.keepInitialState = true;
    d.debugName = index ? "overlay indices" : "overlay vertices";
    b = dev_->createBuffer(d);
    cap = b ? n : 0;
    return bool(b);
  };
  return make(vb_, vb_capacity_, vertices * sizeof(OverlayVertex), false) &&
         make(ib_, ib_capacity_, indices * sizeof(uint32_t), true);
}

nvrhi::IGraphicsPipeline* OverlayPass::Pipeline(nvrhi::IFramebuffer* fb) {
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  const nvrhi::Format format = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
  auto it = pipelines_.find(format);
  if (it != pipelines_.end()) return it->second;
  nvrhi::GraphicsPipelineDesc d;
  d.setInputLayout(input_).setVertexShader(vs_).setPixelShader(ps_).addBindingLayout(layout_);
  d.primType = nvrhi::PrimitiveType::TriangleList;
  nvrhi::BlendState::RenderTarget rt;
  rt.enableBlend()
      .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
      .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOp(nvrhi::BlendOp::Add)
      .setSrcBlendAlpha(nvrhi::BlendFactor::One)
      .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
      .setBlendOpAlpha(nvrhi::BlendOp::Add);
  d.renderState.blendState.setRenderTarget(0, rt);
  d.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();
  d.renderState.rasterState.setCullNone().setScissorEnable(true);
  nvrhi::GraphicsPipelineHandle p = dev_->createGraphicsPipeline(d, info);
  pipelines_[format] = p;
  return p;
}

uint32_t OverlayPass::Draw(nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb, const OverlayFrame& f) {
  if (!ok_ || !f.vertex_count || !f.index_count || !f.command_count || f.display_size[0] <= 0 || f.display_size[1] <= 0)
    return 0;
  nvrhi::IGraphicsPipeline* pipeline = Pipeline(fb);
  if (!pipeline || !EnsureBuffers(f.vertex_count, f.index_count)) return 0;
  cl->writeBuffer(vb_, f.vertices, f.vertex_count * sizeof(OverlayVertex));
  cl->writeBuffer(ib_, f.indices, f.index_count * sizeof(uint32_t));
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  const float fw = float(info.width), fh = float(info.height);
  // Display coordinates -> framebuffer pixels (the window can have a different size than the display area,
  // e.g. with DPI scaling or during a resize)
  const float sx = fw / f.display_size[0], sy = fh / f.display_size[1];
  Projection proj;
  proj.scale[0] = 2.0f / f.display_size[0];
  proj.scale[1] = -2.0f / f.display_size[1];
  proj.translate[0] = -1.0f - f.display_pos[0] * proj.scale[0];
  proj.translate[1] = 1.0f - f.display_pos[1] * proj.scale[1];
  uint32_t draws = 0;
  for (size_t i = 0; i < f.command_count; ++i) {
    const OverlayCommand& c = f.commands[i];
    auto tex = textures_.find(c.texture);
    if (tex == textures_.end() || !c.index_count) continue;
    const int x0 = std::clamp(int(std::floor((c.clip[0] - f.display_pos[0]) * sx)), 0, int(fw));
    const int y0 = std::clamp(int(std::floor((c.clip[1] - f.display_pos[1]) * sy)), 0, int(fh));
    const int x1 = std::clamp(int(std::ceil((c.clip[2] - f.display_pos[0]) * sx)), 0, int(fw));
    const int y1 = std::clamp(int(std::ceil((c.clip[3] - f.display_pos[1]) * sy)), 0, int(fh));
    if (x1 <= x0 || y1 <= y0) continue;
    nvrhi::GraphicsState s;
    s.setPipeline(pipeline).setFramebuffer(fb);
    s.viewport.addViewport(nvrhi::Viewport(fw, fh));
    s.viewport.addScissorRect(nvrhi::Rect(x0, x1, y0, y1));
    s.addBindingSet(tex->second.set);
    s.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(vb_).setSlot(0).setOffset(0));
    s.setIndexBuffer(nvrhi::IndexBufferBinding().setBuffer(ib_).setFormat(nvrhi::Format::R32_UINT).setOffset(0));
    cl->setGraphicsState(s);
    cl->setPushConstants(&proj, sizeof(proj));
    cl->drawIndexed(nvrhi::DrawArguments().setVertexCount(c.index_count).setStartIndexLocation(c.first_index));
    ++draws;
  }
  return draws;
}

}  // namespace fcp::nr
