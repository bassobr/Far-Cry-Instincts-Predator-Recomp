// SmaaPass (core/smaa_pass.h). This project's own code; shader configuration and entry points after FCI
// src/gpu/present.cpp (MIT, same author; kSmaaConfig, kSmaaPasses), bindings here for NVRHI (Vulkan:
// textures 0-2, samplers from 128 as NVRHI's default offset, metrics as push constants; D3D12: t0-t2, s0-s1, b0).
#include "smaa_pass.h"

#include <cstring>

#include "fcshaders/compiler.h"
#include "smaa/smaa_shader.h"
#include "smaa/smaa_textures.h"

namespace fcp::nr {

namespace {

constexpr const char* kConfig = R"(
struct SmaaMetrics { float4 metrics; };
[[vk::push_constant]] ConstantBuffer<SmaaMetrics> smaa_pc : register(b0);
#define SMAA_RT_METRICS smaa_pc.metrics
#define SMAA_CUSTOM_SL
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 16
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#define SMAA_CORNER_ROUNDING 25
[[vk::binding(128, 0)]] SamplerState LinearSampler : register(s0);
[[vk::binding(129, 0)]] SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(LinearSampler, coord, offset)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
)";

// after the SMAA source: edges from luma, weights from edges and tables, then the blended image
constexpr const char* kPasses = R"(
[[vk::binding(0, 0)]] Texture2D tex0 : register(t0);
[[vk::binding(1, 0)]] Texture2D tex1 : register(t1);
[[vk::binding(2, 0)]] Texture2D tex2 : register(t2);
struct SmaaEdgeV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o[3] : TEXCOORD1; };
struct SmaaWeightV { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o[3] : TEXCOORD2; };
struct SmaaBlendV { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
float4 corner(uint id, out float2 uv) {
  uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
SmaaEdgeV vs_edges(uint id : SV_VertexID) {
  SmaaEdgeV o;
  o.p = corner(id, o.uv);
  SMAAEdgeDetectionVS(o.uv, o.o);
  return o;
}
SmaaWeightV vs_weights(uint id : SV_VertexID) {
  SmaaWeightV o;
  o.p = corner(id, o.uv);
  SMAABlendingWeightCalculationVS(o.uv, o.pix, o.o);
  return o;
}
SmaaBlendV vs_blend(uint id : SV_VertexID) {
  SmaaBlendV o;
  o.p = corner(id, o.uv);
  SMAANeighborhoodBlendingVS(o.uv, o.o);
  return o;
}
float4 smaa_edges(SmaaEdgeV i) : SV_Target0 { return float4(SMAALumaEdgeDetectionPS(i.uv, i.o, tex0), 0, 0); }
float4 smaa_weights(SmaaWeightV i) : SV_Target0 { return SMAABlendingWeightCalculationPS(i.uv, i.pix, i.o, tex0, tex1, tex2, 0); }
float4 smaa_blend(SmaaBlendV i) : SV_Target0 { return SMAANeighborhoodBlendingPS(i.uv, i.o, tex0, tex1); }
)";

constexpr const wchar_t* kVs[3] = {L"vs_edges", L"vs_weights", L"vs_blend"};
constexpr const wchar_t* kPs[3] = {L"smaa_edges", L"smaa_weights", L"smaa_blend"};
constexpr const char* kVsA[3] = {"vs_edges", "vs_weights", "vs_blend"};
constexpr const char* kPsA[3] = {"smaa_edges", "smaa_weights", "smaa_blend"};

constexpr uint32_t kAreaW = 160, kAreaH = 560, kSearchW = 64, kSearchH = 16;
static_assert(sizeof(smaa::kSmaaArea) == kAreaW * kAreaH * 2 && sizeof(smaa::kSmaaSearch) == kSearchW * kSearchH);

}  // namespace

bool SmaaPass::Init(nvrhi::IDevice* device, HostApi api, std::string* error) {
  dev_ = device;
  fcshaders::Compiler compiler;
  if (!compiler.ok()) {
    if (error) *error = "DXC not loaded";
    return false;
  }
  const auto target = api == HostApi::kVulkan ? fcshaders::CompileTarget::Spirv : fcshaders::CompileTarget::Dxil;
  const std::string hlsl = std::string(kConfig) + smaa::kSmaaHlsl + kPasses;
  for (int i = 0; i < 3; ++i) {
    const fcshaders::CompileOutput vs = compiler.compileEntry(hlsl, L"vs_6_0", kVs[i], target, false);
    const fcshaders::CompileOutput ps = compiler.compileEntry(hlsl, L"ps_6_0", kPs[i], target, false);
    if (!vs.ok || !ps.ok) {
      if (error) *error = std::string("SMAA shader ") + kPsA[i] + ": " + vs.messages + ps.messages;
      return false;
    }
    vs_[i] = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName(kVsA[i]).setDebugName("smaa vs"),
                                vs.blob.data(), vs.blob.size());
    ps_[i] = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName(kPsA[i]).setDebugName("smaa ps"),
                                ps.blob.data(), ps.blob.size());
    if (!vs_[i] || !ps_[i]) {
      if (error) *error = "SMAA shader: createShader failed";
      return false;
    }
  }
  nvrhi::BindingLayoutDesc ld;
  ld.setVisibility(nvrhi::ShaderType::All);
  ld.addItem(nvrhi::BindingLayoutItem::PushConstants(0, 16));
  for (uint32_t t = 0; t < 3; ++t) ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
  ld.addItem(nvrhi::BindingLayoutItem::Sampler(0));
  ld.addItem(nvrhi::BindingLayoutItem::Sampler(1));
  layout_ = dev_->createBindingLayout(ld);
  linear_ = dev_->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
  point_ = dev_->createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
  auto lookup = [&](nvrhi::Format f, uint32_t w, uint32_t h, const char* name) {
    nvrhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = f;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = name;
    return dev_->createTexture(d);
  };
  area_ = lookup(nvrhi::Format::RG8_UNORM, kAreaW, kAreaH, "smaa area");
  search_ = lookup(nvrhi::Format::R8_UNORM, kSearchW, kSearchH, "smaa search");
  ok_ = layout_ && linear_ && point_ && area_ && search_;
  if (!ok_ && error) *error = "SMAA: layout/sampler/tables failed";
  return ok_;
}

bool SmaaPass::EnsureTargets(uint32_t w, uint32_t h) {
  if (edges_ && w_ == w && h_ == h) return true;
  auto target = [&](nvrhi::Format f, const char* name) {
    nvrhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = f;
    d.isRenderTarget = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.setClearValue(nvrhi::Color(0.0f));
    d.debugName = name;
    return dev_->createTexture(d);
  };
  edges_ = target(nvrhi::Format::RG8_UNORM, "smaa edges");
  weights_ = target(nvrhi::Format::RGBA8_UNORM, "smaa weights");
  if (!edges_ || !weights_) return false;
  edges_fb_ = dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(edges_));
  weights_fb_ = dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(weights_));
  // drop sets and targets with the old textures (NVRHI keeps them alive until the GPU is done)
  sets_.clear();
  dst_fbs_.clear();
  w_ = w;
  h_ = h;
  return edges_fb_ && weights_fb_;
}

nvrhi::IGraphicsPipeline* SmaaPass::Pipeline(int pass, nvrhi::IFramebuffer* fb) {
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  const nvrhi::Format format = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
  auto it = pipelines_.find({pass, format});
  if (it != pipelines_.end()) return it->second;
  nvrhi::GraphicsPipelineDesc d;
  d.setVertexShader(vs_[pass]).setPixelShader(ps_[pass]).addBindingLayout(layout_);
  d.primType = nvrhi::PrimitiveType::TriangleList;
  d.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();
  d.renderState.rasterState.setCullNone();
  nvrhi::GraphicsPipelineHandle p = dev_->createGraphicsPipeline(d, info);
  pipelines_[{pass, format}] = p;
  return p;
}

nvrhi::IBindingSet* SmaaPass::Set(nvrhi::ITexture* t0, nvrhi::ITexture* t1, nvrhi::ITexture* t2) {
  if (!t1) t1 = area_;
  if (!t2) t2 = search_;
  const std::array<nvrhi::ITexture*, 3> key{t0, t1, t2};
  auto it = sets_.find(key);
  if (it != sets_.end()) return it->second.set;
  if (sets_.size() > 32) sets_.clear();  // sources rarely change (resize)
  nvrhi::BindingSetDesc sd;
  sd.addItem(nvrhi::BindingSetItem::PushConstants(0, 16));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, t0));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, t1));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, t2));
  sd.addItem(nvrhi::BindingSetItem::Sampler(0, linear_));
  sd.addItem(nvrhi::BindingSetItem::Sampler(1, point_));
  SetEntry e;
  e.keep[0] = t0, e.keep[1] = t1, e.keep[2] = t2;
  e.set = dev_->createBindingSet(sd, layout_);
  if (!e.set) return nullptr;
  return sets_.emplace(key, std::move(e)).first->second.set;
}

void SmaaPass::Pass(nvrhi::ICommandList* cl, int pass, nvrhi::IFramebuffer* fb, nvrhi::IBindingSet* set, const float metrics[4]) {
  nvrhi::IGraphicsPipeline* p = Pipeline(pass, fb);
  if (!p || !set) return;
  nvrhi::GraphicsState s;
  s.setPipeline(p).setFramebuffer(fb);
  s.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(w_), float(h_)));
  s.addBindingSet(set);
  cl->setGraphicsState(s);
  cl->setPushConstants(metrics, 16);
  cl->draw(nvrhi::DrawArguments().setVertexCount(3));
}

bool SmaaPass::Run(nvrhi::ICommandList* cl, nvrhi::ITexture* src, nvrhi::ITexture* dst) {
  if (!ok_ || !src || !dst) return false;
  const nvrhi::TextureDesc& sd = src->getDesc();
  if (!EnsureTargets(sd.width, sd.height)) return false;
  if (!uploaded_) {
    cl->writeTexture(area_, 0, 0, smaa::kSmaaArea, kAreaW * 2);
    cl->writeTexture(search_, 0, 0, smaa::kSmaaSearch, kSearchW);
    uploaded_ = true;
  }
  auto dst_it = dst_fbs_.find(dst);
  if (dst_it == dst_fbs_.end()) {
    if (dst_fbs_.size() > 8) dst_fbs_.clear();
    dst_it = dst_fbs_.emplace(dst, std::make_pair(nvrhi::TextureHandle(dst),
                                                  dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(dst)))).first;
  }
  nvrhi::IFramebuffer* dst_fb = dst_it->second.second;
  if (!dst_fb) return false;
  const float metrics[4] = {1.0f / float(w_), 1.0f / float(h_), float(w_), float(h_)};
  // Edges: where there are none, the shader discards (0 stays there)
  cl->clearTextureFloat(edges_, nvrhi::AllSubresources, nvrhi::Color(0.0f));
  cl->clearTextureFloat(weights_, nvrhi::AllSubresources, nvrhi::Color(0.0f));
  Pass(cl, 0, edges_fb_, Set(src, nullptr, nullptr), metrics);
  Pass(cl, 1, weights_fb_, Set(edges_, area_, search_), metrics);
  Pass(cl, 2, dst_fb, Set(src, weights_, nullptr), metrics);
  return true;
}

}  // namespace fcp::nr
