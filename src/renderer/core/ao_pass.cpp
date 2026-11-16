// AoPass (core/ao_pass.h). The HLSL kernels are adapted from Far Cry Instincts Recomp src/gpu/ao.cpp (MIT, same
// author), there adapted from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa); GTAO after Jimenez et al.
// 2016 and Intel XeGTAO (MIT, Copyright (c) 2016-2021 Intel Corporation). The NVRHI implementation and the projected
// depth reconstruction are this project's own code. Bindings: Vulkan textures 0-2, constants as push constants;
// D3D12 t0-t2, b0.
#include "ao_pass.h"

#include <algorithm>
#include <cstring>

#include "fcshaders/compiler.h"

namespace fcp::nr {

namespace {

// CryEngine units are metres.
constexpr float kRadius = 1.0f;     // world radius of the occlusion
constexpr float kMaxSpan = 0.12f;   // most of the view's height the radius may span on screen
constexpr float kNear = 0.05f;      // closer than this: first person view (hands, weapon), no occlusion
constexpr float kFadeStart = 35.0f;  // view depth where the occlusion starts to fade ...
constexpr float kFadeEnd = 90.0f;    // ... and where it is gone

struct Constants {
  float proj[4];  // 1 / x scale, 1 / y scale, k0, k1 (view depth = k0 / (stored + k1))
  float vp[4];    // the view in occlusion pixels: x, y, width, height (clipped)
  float host[4];  // the viewport in host pixels: x, y, width, height (ndc -1..1)
  float opt[4];   // world radius, most occlusion pixels it may span, blur step x, y
  float misc[4];  // host pixels per occlusion pixel, fade start, 1 / fade length, strength
  float lim[4];   // near, sky depth (view), debug split column (host x, 0 = none), host clip (left, packed below)
  float clip[4];  // the view clipped, host pixels: left, top, right, bottom
};
static_assert(sizeof(Constants) <= 128, "push constants");

constexpr const char* kHlsl = R"(
struct AoConstants { float4 proj, vp, host, opt, misc, lim, clip; };
[[vk::push_constant]] ConstantBuffer<AoConstants> c : register(b0);
#ifdef MS
[[vk::binding(0, 0)]] Texture2DMS<float> depth : register(t0);
float Stored(int2 p, uint s) { return depth.Load(p, s); }
#else
[[vk::binding(0, 0)]] Texture2D<float> depth : register(t0);
float Stored(int2 p, uint s) { return depth.Load(int3(p, 0)); }
#endif
[[vk::binding(1, 0)]] Texture2D<float> lin : register(t1);    // view depth per occlusion pixel
[[vk::binding(2, 0)]] Texture2D<float2> aot : register(t2);   // occlusion, view depth

float4 vs_main(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}

static const float kFar = 60000;  // sky; stays finite in 16 bit floats
// view depth of a stored depth (z / w of the game's projection)
float ViewDepth(float d) {
  float z = c.proj.z / (d + c.proj.w);
  return (z > 0 && z < c.lim.y) ? z : kFar;
}
int2 HostClamp(int2 p) { return clamp(p, int2(c.clip.xy), int2(c.clip.zw) - 1); }

// the view depth of an occlusion pixel: the host pixel at its centre, sample 0
float ps_linearize(float4 pos : SV_Position) : SV_Target {
  return ViewDepth(Stored(HostClamp(int2(pos.xy * c.misc.x)), 0));
}

// view space position at an occlusion pixel position (x right, y up, z forward): depth of the pixel it falls in,
// x and y at the exact position (a sample 1.3 pixels away in a slanted direction keeps its direction; snapping to
// the pixel centre turned flat walls darker off the view's centre)
float3 View(float2 px) {
  float2 lo = c.vp.xy + 0.5, hi = c.vp.xy + c.vp.zw - 0.5;
  px = clamp(px, lo, hi);
  float z = lin.Load(int3(int2(px), 0));
  float2 ndc = (px * c.misc.x - c.host.xy) / c.host.zw * float2(2, -2) + float2(-1, 1);
  return float3(ndc * c.proj.xy * z, z);
}
// samples outside the view count as nothing (not as the clamped edge: a dark frame along the screen's border)
bool Inside(float2 px) { return all(px >= c.vp.xy) && all(px < c.vp.xy + c.vp.zw); }
static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
// normal from the nearer neighbour each way: no bend over depth edges; at the view's border the side inside
float3 Normal(float2 px, float3 ctr) {
  float3 l = View(px - float2(1, 0)), r = View(px + float2(1, 0));
  float3 u = View(px - float2(0, 1)), d = View(px + float2(0, 1));
  float3 dr = r - ctr, dl = ctr - l, dd = d - ctr, du = ctr - u;
  bool use_r = (abs(dr.z) < abs(dl.z) && dot(dr, dr) > 0) || dot(dl, dl) == 0;
  bool use_d = (abs(dd.z) < abs(du.z) && dot(dd, dd) > 0) || dot(du, du) == 0;
  float3 n = cross(use_r ? dr : dl, use_d ? dd : du);
  return dot(n, n) > 1e-20 ? normalize(n) : normalize(-ctr);
}
// the radius in occlusion pixels, x and y
float2 Radius(float z) { return c.opt.x * 0.5 * c.host.zw / (c.proj.xy * z) / c.misc.x; }

// out: occlusion, view z
float2 ps_ssao(float4 pos : SV_Position) : SV_Target {
  float3 ctr = View(pos.xy);
  if (ctr.z < c.lim.x || ctr.z >= kFar) return float2(1, ctr.z);
  float2 rad = Radius(ctr.z);
  float span = max(rad.x, rad.y);
  if (span < 1) return float2(1, ctr.z);
  float3 n = Normal(pos.xy, ctr);
  float k = min(1, c.opt.y / span), R = c.opt.x * k;
  int2 cell = int2(pos.xy) & 3;
  float turn = kDither[cell.y * 4 + cell.x] * (6.2831853 / 16), a = 0;
  for (int i = 0; i < 16; i++) {
    float f = (i + 0.5) / 16, t = i * 2.3999632 + turn;
    float2 at = pos.xy + float2(cos(t), sin(t)) * rad * (k * f);
    float3 v = (View(at) - ctr) / R;
    float vv = dot(v, v), w = Inside(at) ? max(1 - vv, 0) : 0;
    a += w * w * w * max((dot(v, n) - 0.05) / (vv + 0.01), 0);
  }
  return float2(max(1 - a * 0.25, 0), ctr.z);
}

static const float kHalfPi = 1.5707963;
// GTAO at XeGTAO's high preset: 3 slices, 3 steps a side, squared step spread, falloff over the outer 61.5% of the
// radius, power 2.2. Per slice the highest horizon each side, the visible arc between integrated against the normal
// projected into the slice.
float2 ps_gtao(float4 pos : SV_Position) : SV_Target {
  float3 ctr = View(pos.xy);
  if (ctr.z < c.lim.x || ctr.z >= kFar) return float2(1, ctr.z);
  float2 rad = Radius(ctr.z);
  float span = max(rad.x, rad.y);
  if (span < 1) return float2(1, ctr.z);
  float3 n = Normal(pos.xy, ctr);
  float3 v = normalize(-ctr);
  float k = min(1, c.opt.y / span), R = c.opt.x * k;
  int2 cell = int2(pos.xy) & 3;
  float slice_noise = kDither[cell.y * 4 + cell.x] / 16;
  float step_noise = kDither[cell.x * 4 + cell.y] / 16;
  float vis = 0, wsum = 0;
  for (int s = 0; s < 3; s++) {
    float phi = (s + slice_noise) * (kHalfPi * 2 / 3);
    float3 dir = float3(cos(phi), sin(phi), 0);
    float2 omega = float2(dir.x, -dir.y) * rad * k;  // pixels; screen y runs down, view y up
    float3 ortho = dir - dot(dir, v) * v;
    float3 axis = normalize(cross(ortho, v));
    float3 pn = n - axis * dot(n, axis);
    float pn_len = max(length(pn), 1e-4);
    float cos_n = saturate(dot(pn, v) / pn_len);
    float an = sign(dot(ortho, pn)) * acos(cos_n);
    float2 low = float2(cos(an + kHalfPi), cos(an - kHalfPi));  // x: the +omega side, y: the -omega side
    float2 hc = low;
    for (int j = 0; j < 3; j++) {
      float f = (j + step_noise) / 3;
      float2 off = omega * (f * f + 1.3 / length(omega));  // at least 1.3 pixels out, off the centre pixel
      float3 d0 = View(pos.xy + off) - ctr;
      float3 d1 = View(pos.xy - off) - ctr;
      float2 len = max(float2(length(d0), length(d1)), 1e-4);
      float2 w = saturate((R - len) / (R * 0.615)) * float2(Inside(pos.xy + off), Inside(pos.xy - off));
      float2 sc = float2(dot(d0, v), dot(d1, v)) / len;
      hc = max(hc, lerp(low, sc, w));
    }
    float h0 = an + clamp(-acos(hc.y) - an, -kHalfPi, kHalfPi);
    float h1 = an + clamp(acos(hc.x) - an, -kHalfPi, kHalfPi);
    float sin_n = sin(an);
    float arc0 = (cos_n + 2 * h0 * sin_n - cos(2 * h0 - an)) / 4;
    float arc1 = (cos_n + 2 * h1 * sin_n - cos(2 * h1 - an)) / 4;
    // Slices are evenly spread on the screen, not around the view ray: away from the view's centre that weighted
    // some directions more and darkened open surfaces (5-7 % at 30-45 degrees off axis). Weight = d(angle around
    // the ray) / d(screen angle) = |v.z| / (1 - (dir . v)^2). Own addition.
    float wj = abs(v.z) / max(1 - dot(dir, v) * dot(dir, v), 1e-4);
    vis += wj * pn_len * (arc0 + arc1);
    wsum += wj;
  }
  // Unclamped (up to 2): three slices per pixel scatter around the true value, about 1 on an open surface; clamping
  // here, before the blur averages the 4x4 noise, made open surfaces darker. Clamp and power (2.2) in AoAt.
  return float2(clamp(vis / max(wsum, 1e-4), 0, 2), ctr.z);
}

// opt.zw: step; 5 taps average the 4 pixel dither, depth edges left out
float2 ps_blur(float4 pos : SV_Position) : SV_Target {
  int2 p = int2(pos.xy), lo = int2(c.vp.xy), hi = int2(c.vp.xy + c.vp.zw) - 1;
  float z = aot.Load(int3(p, 0)).y;
  float sum = 0, n = 0;
  for (int i = -2; i <= 2; i++) {
    float2 s = aot.Load(int3(clamp(p + i * int2(c.opt.zw), lo, hi), 0));
    float w = (abs(i) == 2 ? 0.5 : 1) * saturate(2 - abs(s.y - z) / (z * 0.02));
    sum += s.x * w;
    n += w;
  }
  return float2(sum / max(n, 1e-4), z);
}

// into the scene: the nearest occlusion pixels of a similar depth, faded with distance, for view depth z
float4 AoAt(float2 pos, float z) {
  if (z < c.lim.x || z >= kFar) return 1;
  float2 q = pos / c.misc.x - 0.5;
  int2 b = int2(floor(q)), lo = int2(c.vp.xy), hi = int2(c.vp.xy + c.vp.zw) - 1;
  float2 f = q - b;
  float sum = 0, n = 0, nearest = 1, gap = 1e30;
  for (int i = 0; i < 4; i++) {
    int2 o = int2(i & 1, i >> 1);
    float2 t = aot.Load(int3(clamp(b + o, lo, hi), 0));
    float dz = abs(t.y - z);
    float w = (o.x ? f.x : 1 - f.x) * (o.y ? f.y : 1 - f.y) * saturate(1 - dz / (z * 0.04));
    sum += t.x * w;
    n += w;
    if (dz < gap) gap = dz, nearest = t.x;
  }
  float a = n > 1e-3 ? sum / n : nearest;
  a = pow(saturate(a), c.lim.w);  // GTAO: XeGTAO's final power 2.2; SSAO: 1
  a = lerp(a, 1, saturate((z - c.misc.y) * c.misc.z));
  a = pow(saturate(a), c.misc.w);
  if (pos.x < c.lim.z) a = 1;
  return float4(a.xxx, 1);
}
#ifdef MS
// per sample: each sample at its own depth (edges of the 2x/4x MSAA picture)
float4 ps_apply(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target {
  return AoAt(pos.xy, ViewDepth(Stored(int2(pos.xy), s)));
}
float4 ps_depth_view(float4 pos : SV_Position, uint s : SV_SampleIndex) : SV_Target {
  float z = ViewDepth(Stored(int2(pos.xy), s));
  return float4(saturate(z / 2), saturate(z / 20), saturate(z / 200), 1);
}
#else
float4 ps_apply(float4 pos : SV_Position) : SV_Target { return AoAt(pos.xy, ViewDepth(Stored(int2(pos.xy), 0))); }
// FCP_AO_DEBUG=2: view depth, red to 2 m, green to 20 m, blue to 200 m
float4 ps_depth_view(float4 pos : SV_Position) : SV_Target {
  float z = ViewDepth(Stored(int2(pos.xy), 0));
  return float4(saturate(z / 2), saturate(z / 20), saturate(z / 200), 1);
}
#endif
)";

struct PsInfo {
  const wchar_t* wentry;
  const char* entry;
  bool ms;
};
constexpr PsInfo kPs[] = {
    {L"ps_linearize", "ps_linearize", false}, {L"ps_linearize", "ps_linearize", true}, {L"ps_ssao", "ps_ssao", false},
    {L"ps_gtao", "ps_gtao", false},           {L"ps_blur", "ps_blur", false},           {L"ps_apply", "ps_apply", false},
    {L"ps_apply", "ps_apply", true},          {L"ps_depth_view", "ps_depth_view", false}, {L"ps_depth_view", "ps_depth_view", true},
};

}  // namespace

bool AoPass::Init(nvrhi::IDevice* device, HostApi api, std::string* error) {
  dev_ = device;
  fcshaders::Compiler compiler;
  if (!compiler.ok()) {
    if (error) *error = "DXC not loaded";
    return false;
  }
  const auto target = api == HostApi::kVulkan ? fcshaders::CompileTarget::Spirv : fcshaders::CompileTarget::Dxil;
  const std::string plain = kHlsl, ms = std::string("#define MS 1\n") + kHlsl;
  const fcshaders::CompileOutput vs = compiler.compileEntry(plain, L"vs_6_0", L"vs_main", target, false);
  if (!vs.ok) {
    if (error) *error = "AO vertex shader: " + vs.messages;
    return false;
  }
  vs_ = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Vertex).setEntryName("vs_main").setDebugName("ao vs"),
                           vs.blob.data(), vs.blob.size());
  for (int i = 0; i < kPsCount; ++i) {
    const fcshaders::CompileOutput ps = compiler.compileEntry(kPs[i].ms ? ms : plain, L"ps_6_0", kPs[i].wentry, target, false);
    if (!ps.ok) {
      if (error) *error = std::string("AO shader ") + kPs[i].entry + (kPs[i].ms ? " (MS)" : "") + ": " + ps.messages;
      return false;
    }
    ps_[i] = dev_->createShader(nvrhi::ShaderDesc().setShaderType(nvrhi::ShaderType::Pixel).setEntryName(kPs[i].entry).setDebugName("ao ps"),
                                ps.blob.data(), ps.blob.size());
    if (!ps_[i]) {
      if (error) *error = "AO shader: createShader failed";
      return false;
    }
  }
  nvrhi::BindingLayoutDesc ld;
  ld.setVisibility(nvrhi::ShaderType::All);
  ld.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Constants)));
  for (uint32_t t = 0; t < 3; ++t) ld.addItem(nvrhi::BindingLayoutItem::Texture_SRV(t));
  layout_ = dev_->createBindingLayout(ld);
  // Stand-ins for the slots a pass does not read (a descriptor must be there).
  nvrhi::TextureDesc dd;
  dd.width = dd.height = 1;
  dd.format = nvrhi::Format::R32_FLOAT;
  dd.isRenderTarget = true;
  dd.initialState = nvrhi::ResourceStates::ShaderResource;
  dd.keepInitialState = true;
  dd.debugName = "ao dummy";
  dummy_ = dev_->createTexture(dd);
  ok_ = layout_ && vs_ && dummy_;
  if (!ok_ && error) *error = "AO: layout or shaders failed";
  return ok_;
}

bool AoPass::EnsureTargets(uint32_t w, uint32_t h) {
  if (lin_ && w_ == w && h_ == h) return true;
  auto target = [&](nvrhi::Format f, const char* name) {
    nvrhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = f;
    d.isRenderTarget = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.setClearValue(nvrhi::Color(1.0f));
    d.debugName = name;
    return dev_->createTexture(d);
  };
  lin_ = target(nvrhi::Format::R32_FLOAT, "ao depth");
  ao_[0] = target(nvrhi::Format::RG16_FLOAT, "ao occlusion");
  ao_[1] = target(nvrhi::Format::RG16_FLOAT, "ao occlusion 2");
  if (!lin_ || !ao_[0] || !ao_[1]) return false;
  lin_fb_ = dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(lin_));
  ao_fb_[0] = dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(ao_[0]));
  ao_fb_[1] = dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(ao_[1]));
  sets_.clear();  // NVRHI keeps the old textures alive until the GPU is done with them
  w_ = w;
  h_ = h;
  return lin_fb_ && ao_fb_[0] && ao_fb_[1];
}

nvrhi::IGraphicsPipeline* AoPass::Pipeline(Ps ps, nvrhi::IFramebuffer* fb, bool multiply) {
  const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
  const nvrhi::Format format = info.colorFormats.empty() ? nvrhi::Format::UNKNOWN : info.colorFormats[0];
  const auto key = std::make_tuple(int(ps), format, info.sampleCount, multiply);
  if (auto it = pipelines_.find(key); it != pipelines_.end()) return it->second;
  nvrhi::GraphicsPipelineDesc d;
  d.setVertexShader(vs_).setPixelShader(ps_[ps]).addBindingLayout(layout_);
  d.primType = nvrhi::PrimitiveType::TriangleList;
  d.renderState.depthStencilState.disableDepthTest().disableDepthWrite().disableStencil();
  d.renderState.rasterState.setCullNone();
  d.renderState.rasterState.scissorEnable = true;
  nvrhi::BlendState::RenderTarget& b = d.renderState.blendState.targets[0];
  if (multiply) {
    // colour times the occlusion; the alpha the game's later blends may read stays
    b.blendEnable = true;
    b.srcBlend = nvrhi::BlendFactor::Zero;
    b.destBlend = nvrhi::BlendFactor::SrcColor;
    b.blendOp = nvrhi::BlendOp::Add;
    b.srcBlendAlpha = nvrhi::BlendFactor::Zero;
    b.destBlendAlpha = nvrhi::BlendFactor::One;
    b.blendOpAlpha = nvrhi::BlendOp::Add;
    b.colorWriteMask = nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
  } else if (ps == kApply || ps == kApplyMs || ps == kDepthView || ps == kDepthViewMs) {
    b.colorWriteMask = nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;  // debug: replace
  }
  nvrhi::GraphicsPipelineHandle p = dev_->createGraphicsPipeline(d, info);
  pipelines_[key] = p;
  return p;
}

nvrhi::IBindingSet* AoPass::Set(nvrhi::ITexture* t0, nvrhi::ITexture* t1, nvrhi::ITexture* t2) {
  if (!t0) t0 = dummy_;
  if (!t1) t1 = dummy_;
  if (!t2) t2 = dummy_;
  const auto key = std::make_tuple(t0, t1, t2);
  if (auto it = sets_.find(key); it != sets_.end()) return it->second.set;
  if (sets_.size() > 48) sets_.clear();  // depth and colour targets rarely change (resize, level)
  nvrhi::BindingSetDesc sd;
  sd.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(Constants)));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(0, t0));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(1, t1));
  sd.addItem(nvrhi::BindingSetItem::Texture_SRV(2, t2));
  SetEntry e;
  e.keep[0] = t0, e.keep[1] = t1, e.keep[2] = t2;
  e.set = dev_->createBindingSet(sd, layout_);
  if (!e.set) return nullptr;
  return sets_.emplace(key, std::move(e)).first->second.set;
}

void AoPass::Pass(nvrhi::ICommandList* cl, Ps ps, nvrhi::IFramebuffer* fb, bool multiply, nvrhi::IBindingSet* set,
                  const nvrhi::Rect& scissor, uint32_t vw, uint32_t vh, const void* constants) {
  nvrhi::IGraphicsPipeline* p = Pipeline(ps, fb, multiply);
  if (!p || !set) return;
  nvrhi::GraphicsState s;
  s.setPipeline(p).setFramebuffer(fb);
  s.viewport.addViewport(nvrhi::Viewport(float(vw), float(vh)));
  s.viewport.addScissorRect(scissor);
  s.addBindingSet(set);
  cl->setGraphicsState(s);
  cl->setPushConstants(constants, sizeof(Constants));
  cl->draw(nvrhi::DrawArguments().setVertexCount(3));
}

bool AoPass::Run(nvrhi::ICommandList* cl, const Params& p) {
  if (!ok_ || !p.depth || !p.color || !p.width || !p.height || p.mode < 1 || p.mode > 2) return false;
  const int32_t left = std::max(p.left, 0), top = std::max(p.top, 0);
  const int32_t right = std::min<int32_t>(p.right, int32_t(p.width)), bottom = std::min<int32_t>(p.bottom, int32_t(p.height));
  if (right - left < 8 || bottom - top < 8 || p.view_w < 8 || p.view_h < 8) return false;
  const uint32_t s = p.half ? 2 : 1;
  if (!EnsureTargets((p.width + s - 1) / s, (p.height + s - 1) / s)) return false;
  auto it = color_fbs_.find(p.color);
  if (it == color_fbs_.end()) {
    if (color_fbs_.size() > 8) color_fbs_.clear();
    it = color_fbs_.emplace(p.color, std::make_pair(nvrhi::TextureHandle(p.color),
                                                    dev_->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(p.color)))).first;
  }
  nvrhi::IFramebuffer* color_fb = it->second.second;
  if (!color_fb) return false;
  const int32_t vl = left / int32_t(s), vt = top / int32_t(s);
  const int32_t vr = (right + int32_t(s) - 1) / int32_t(s), vb = (bottom + int32_t(s) - 1) / int32_t(s);
  Constants c{};
  c.proj[0] = 1.0f / p.x_scale, c.proj[1] = 1.0f / p.y_scale, c.proj[2] = p.k0, c.proj[3] = p.k1;
  c.vp[0] = float(vl), c.vp[1] = float(vt), c.vp[2] = float(vr - vl), c.vp[3] = float(vb - vt);
  c.host[0] = p.view_x, c.host[1] = p.view_y, c.host[2] = p.view_w, c.host[3] = p.view_h;
  c.opt[0] = kRadius, c.opt[1] = kMaxSpan * p.view_h / float(s);
  c.misc[0] = float(s), c.misc[1] = kFadeStart, c.misc[2] = 1.0f / (kFadeEnd - kFadeStart), c.misc[3] = p.strength;
  c.lim[0] = kNear, c.lim[1] = p.far_plane * 0.999f, c.lim[2] = p.debug == 3 ? float(left + right) * 0.5f : 0.0f;
  c.lim[3] = p.mode == 2 ? 2.2f : 1.0f;
  c.clip[0] = float(left), c.clip[1] = float(top), c.clip[2] = float(right), c.clip[3] = float(bottom);
  const bool ms = p.samples > 1;
  const nvrhi::Rect ao_rect(vl, vr, vt, vb), host_rect(left, right, top, bottom);
  cl->setTextureState(p.depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  Pass(cl, ms ? kLinearizeMs : kLinearize, lin_fb_, false, Set(p.depth, nullptr, nullptr), ao_rect, w_, h_, &c);
  Pass(cl, p.mode == 2 ? kGtao : kSsao, ao_fb_[0], false, Set(nullptr, lin_, nullptr), ao_rect, w_, h_, &c);
  c.opt[2] = 1, c.opt[3] = 0;
  Pass(cl, kBlur, ao_fb_[1], false, Set(nullptr, nullptr, ao_[0]), ao_rect, w_, h_, &c);
  c.opt[2] = 0, c.opt[3] = 1;
  Pass(cl, kBlur, ao_fb_[0], false, Set(nullptr, nullptr, ao_[1]), ao_rect, w_, h_, &c);
  const bool replace = p.debug == 1 || p.debug == 2;
  const Ps apply = p.debug == 2 ? (ms ? kDepthViewMs : kDepthView) : (ms ? kApplyMs : kApply);
  Pass(cl, apply, color_fb, !replace, Set(p.depth, nullptr, ao_[0]), host_rect, p.width, p.height, &c);
  return true;
}

}  // namespace fcp::nr
