// HdrPass (core/hdr_pass.h). This project's own code; mapping and parameters after FCI src/gpu/present.cpp (MIT, same
// author; there adapted from SCDA-XBERecomp, MIT), bindings for NVRHI as in smaa_pass.cpp (Vulkan: texture 0,
// parameters as push constants; D3D12: t0, root constants b0).
#include "hdr_pass.h"

#include <algorithm>
#include <cmath>

#include "fcshaders/compiler.h"

namespace fcp::nr {

const char* const kHdrCommonHlsl = R"(
// SMPTE ST 2084 inverse EOTF: linear light / 10000 nits -> PQ code value
float3 Pq(float3 l) {
  const float3 y = pow(saturate(l), 0.1593017578125);
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}
// BT.709 -> BT.2020 primaries (ITU-R BT.2087), rows; white stays white
static const float3x3 k709To2020 = {0.627404, 0.329283, 0.043313, 0.069097, 0.919540, 0.011362, 0.016391, 0.088013, 0.895595};
)";

namespace {

constexpr const char* kShader = R"(
struct HdrParams {
  float4 a;  // paper white / 10000, peak / 10000, gain at SDR white (1 = off), knee
  float4 b;  // gamma, test pattern (> 0: bars), 0, 0
};
[[vk::push_constant]] ConstantBuffer<HdrParams> pc : register(b0);
[[vk::binding(0, 0)]] Texture2D<float4> src : register(t0);
struct VsOut { float4 pos : SV_Position; };
VsOut vs_main(uint id : SV_VertexID) {
  const float2 t = float2((id << 1) & 2, id & 2);
  VsOut o;
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
static const float3 kLum = float3(0.2126, 0.7152, 0.0722);
float4 ps_main(VsOut i) : SV_Target0 {
  float3 c = saturate(src.Load(int3(int2(i.pos.xy), 0)).rgb);
  if (pc.b.y > 0.0) {
    // test pattern: white, 75 %, 50 %, 25 % grey, black, red, green, blue
    uint w, h;
    src.GetDimensions(w, h);
    const uint bar = min(7u, uint(i.pos.x * 8.0 / float(w)));
    const float3 bars[8] = {float3(1, 1, 1), float3(0.75, 0.75, 0.75), float3(0.5, 0.5, 0.5), float3(0.25, 0.25, 0.25),
                            float3(0, 0, 0), float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1)};
    c = bars[bar];
  }
  float3 lin = pow(c, pc.b.x);
  if (pc.a.z > 1.0) {
    // highlight stretch: above the knee the luminance bends up to gain x at SDR white, the hue stays
    const float l = dot(lin, kLum);
    const float t = saturate((l - pc.a.w) / (1.0 - pc.a.w));
    lin *= (l + (pc.a.z - 1.0) * t * t) / max(l, 1e-6);
  }
  return float4(Pq(min(mul(k709To2020, lin) * pc.a.x, pc.a.y)), 1.0);
}
)";

struct Params {
  float a[4];
  float b[4];
};

}  // namespace

bool HdrPass::Init(nvrhi::IDevice* device, HostApi api, std::string* error) {
  dev_ = device;
  fcshaders::Compiler compiler;
  if (!compiler.ok()) {
    if (error) *error = "DXC not loaded";
    return false;
  }
  const auto target = api == HostApi::kVulkan ? fcshaders::CompileTarget::Spirv : fcshaders::CompileTarget::Dxil;
  const std::string hlsl = std::string(kHdrCommonHlsl) + kShader;
  const fcshaders::CompileOutput vs = compiler.compileEntry(hlsl, L"vs_6_0", L"vs_main", target, false);
  const fcshaders::CompileOutput ps = compiler.compileEntry(hlsl, L"ps_6_0", L"ps_main", target, false);
  if (!vs.ok || !ps.ok) {
    if (error) *error = "HDR10 shader: " + vs.messages + ps.messages;
    return false;
  }
  vs_ = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName("vs_main").setDebugName("hdr10 vs"),
                           vs.blob.data(), vs.blob.size());
  ps_ = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName("ps_main").setDebugName("hdr10 ps"),
                           ps.blob.data(), ps.blob.size());
  nvrhi::BindingLayoutDesc ld;
  ld.setVisibility(nvrhi::ShaderType::All);
  ld.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Params)));
  ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
  layout_ = dev_->createBindingLayout(ld);
  ok_ = vs_ && ps_ && layout_;
  if (!ok_ && error) *error = "HDR10: shader/layout failed";
  return ok_;
}

nvrhi::IGraphicsPipeline* HdrPass::Pipeline(nvrhi::IFramebuffer* fb) {
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  const nvrhi::Format format = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
  if (auto it = pipelines_.find(format); it != pipelines_.end()) return it->second;
  nvrhi::GraphicsPipelineDesc d;
  d.setVertexShader(vs_).setPixelShader(ps_).addBindingLayout(layout_);
  d.primType = nvrhi::PrimitiveType::TriangleList;
  d.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();
  d.renderState.rasterState.setCullNone();
  nvrhi::GraphicsPipelineHandle p = dev_->createGraphicsPipeline(d, info);
  pipelines_[format] = p;
  return p;
}

nvrhi::IBindingSet* HdrPass::Set(nvrhi::ITexture* src) {
  if (auto it = sets_.find(src); it != sets_.end()) return it->second.second;
  if (sets_.size() > 8) sets_.clear();  // the source changes only with the swapchain's size
  nvrhi::BindingSetDesc sd;
  sd.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Params)));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, src));
  nvrhi::BindingSetHandle set = dev_->createBindingSet(sd, layout_);
  if (!set) return nullptr;
  return sets_.emplace(src, std::make_pair(nvrhi::TextureHandle(src), set)).first->second.second;
}

bool HdrPass::Run(nvrhi::ICommandList* cl, nvrhi::ITexture* src, nvrhi::IFramebuffer* dst, const HdrLevels& l) {
  if (!ok_ || !src || !dst) return false;
  nvrhi::IGraphicsPipeline* p = Pipeline(dst);
  nvrhi::IBindingSet* set = Set(src);
  if (!p || !set) return false;
  const nvrhi::FramebufferInfoEx& info = dst->getFramebufferInfo();
  const Params params = {{l.paper_white / 10000.0f, l.peak / 10000.0f, l.gain, l.knee},
                         {l.gamma, l.test_pattern ? 1.0f : 0.0f, 0.0f, 0.0f}};
  cl->setTextureState(src, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  nvrhi::GraphicsState s;
  s.setPipeline(p).setFramebuffer(dst);
  s.viewport.addViewportAndScissorRect(nvrhi::Viewport(float(info.width), float(info.height)));
  s.addBindingSet(set);
  cl->setGraphicsState(s);
  cl->setPushConstants(&params, sizeof(params));
  cl->draw(nvrhi::DrawArguments().setVertexCount(3));
  return true;
}

float PqFromNits(float nits) {
  const double y = std::pow(std::clamp(double(nits) / 10000.0, 0.0, 1.0), 0.1593017578125);
  return float(std::pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375));
}

float NitsFromPq(float code) {
  const double p = std::pow(std::clamp(double(code), 0.0, 1.0), 1.0 / 78.84375);
  return float(10000.0 * std::pow(std::max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125));
}

void HdrEncodeReference(const float sdr[3], const HdrLevels& l, float pq[3]) {
  static constexpr double m[3][3] = {{0.627404, 0.329283, 0.043313}, {0.069097, 0.919540, 0.011362}, {0.016391, 0.088013, 0.895595}};
  double lin[3];
  for (int i = 0; i < 3; ++i) lin[i] = std::pow(std::clamp(double(sdr[i]), 0.0, 1.0), double(l.gamma));
  if (l.gain > 1.0f) {
    const double lum = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
    const double t = std::clamp((lum - l.knee) / (1.0 - l.knee), 0.0, 1.0);
    const double k = (lum + (l.gain - 1.0) * t * t) / std::max(lum, 1e-6);
    for (double& v : lin) v *= k;
  }
  for (int i = 0; i < 3; ++i) {
    const double v = m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2];
    pq[i] = PqFromNits(float(std::min(v * l.paper_white, double(l.peak))));
  }
}

HdrLevels HdrLevelsFor(float paper_white, float peak, float boost, float display_white, float display_peak, float gamma) {
  HdrLevels l;
  l.paper_white = paper_white > 0.0f ? paper_white : display_white > 0.0f ? display_white : 200.0f;
  l.paper_white = std::clamp(l.paper_white, 80.0f, 1000.0f);
  l.peak = peak > 0.0f ? peak : display_peak > 0.0f ? display_peak : 1000.0f;
  l.peak = std::clamp(std::max(l.peak, l.paper_white), 80.0f, 10000.0f);
  l.gain = 1.0f + std::clamp(boost, 0.0f, 1.0f) * (l.peak / l.paper_white - 1.0f);  // boost 1: SDR white reaches the peak
  l.gamma = std::clamp(gamma, 1.0f, 3.0f);
  return l;
}

}  // namespace fcp::nr
