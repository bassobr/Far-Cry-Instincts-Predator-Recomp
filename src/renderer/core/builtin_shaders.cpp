// Built-in shaders (renderer/builtin_shaders.h). This project's own code.
#include "renderer/builtin_shaders.h"

#include <mutex>

#include "fcshaders/compiler.h"

namespace fcp::nr {

namespace {

// Same binding model as fcshaders/abi.h (b2 = DrawConstants); only the first fields are needed.
constexpr const char* kTriangle = R"(
struct VsOut { float4 pos : SV_Position; float4 color : COLOR0; };
VsOut vs_main(uint id : SV_VertexID) {
  const float2 p[3] = { float2(0.0, 0.75), float2(0.7, -0.6), float2(-0.7, -0.6) };
  const float3 c[3] = { float3(1, 0.1, 0.1), float3(0.1, 1, 0.1), float3(0.1, 0.2, 1) };
  VsOut o;
  o.pos = float4(p[id], 0.5, 1.0);
  o.color = float4(c[id], 1.0);
  return o;
}
float4 ps_main(VsOut i) : SV_Target0 { return i.color; }
)";

constexpr const char* kSolid = R"(
cbuffer DrawConstantsCB : register(b2, space0) {
  uint4 kk_Bool[2]; uint4 kk_Loop[8]; uint4 kk_TextureIndex[8]; uint4 kk_VSSampler[8]; uint4 kk_PSSampler[8];
  uint4 kk_VertexFetch[16]; float4 kk_ClipPlane[6];
};
float4 main() : SV_Target0 { return kk_ClipPlane[0]; }
)";

constexpr const char* kBlit = R"(
cbuffer DrawConstantsCB : register(b2, space0) {
  uint4 kk_Bool[2]; uint4 kk_Loop[8]; uint4 kk_TextureIndex[8]; uint4 kk_VSSampler[8]; uint4 kk_PSSampler[8];
  uint4 kk_VertexFetch[16]; float4 kk_ClipPlane[6];
};
Texture2D<float4> kk_Tex2D[] : register(t0, space1);
SamplerState kk_Sampler[] : register(s0, space5);
struct VsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VsOut vs_main(uint id : SV_VertexID) {
  const float2 t = float2((id << 1) & 2, id & 2);
  VsOut o;
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  o.uv = kk_ClipPlane[1].xy + t * kk_ClipPlane[1].zw;
  return o;
}
float4 ps_main(VsOut i) : SV_Target0 {
  return kk_Tex2D[kk_TextureIndex[0].x].Sample(kk_Sampler[kk_PSSampler[0].x], i.uv);
}
float ramp(uint ch, float v) {
  const float x = saturate(v) * 255.0;
  const uint i0 = (uint)floor(x), i1 = min(i0 + 1, 255u);
  const float a = kk_Tex2D[kk_TextureIndex[0].y].Load(int3(i0, 0, 0))[ch];
  const float b = kk_Tex2D[kk_TextureIndex[0].y].Load(int3(i1, 0, 0))[ch];
  return lerp(a, b, x - (float)i0);
}
float4 ps_gamma(VsOut i) : SV_Target0 {
  const float4 c = kk_Tex2D[kk_TextureIndex[0].x].Sample(kk_Sampler[kk_PSSampler[0].x], i.uv);
  return float4(ramp(0, c.r), ramp(1, c.g), ramp(2, c.b), c.a);
}
)";

struct Cache {
  std::mutex mutex;
  std::vector<uint8_t> blobs[2][size_t(BuiltinShader::kCount)];
};

}  // namespace

nvrhi::ShaderHandle CreateBuiltinShader(nvrhi::IDevice* device, HostApi api, BuiltinShader which, std::string* error) {
  static Cache cache;
  const int a = api == HostApi::kVulkan ? 1 : 0;
  const int w = int(which);
  std::vector<uint8_t> blob;
  {
    std::lock_guard lock(cache.mutex);
    if (cache.blobs[a][w].empty()) {
      fcshaders::Compiler compiler;
      if (!compiler.ok()) {
        if (error) *error = "DXC not loaded";
        return nullptr;
      }
      const auto target = api == HostApi::kVulkan ? fcshaders::CompileTarget::Spirv : fcshaders::CompileTarget::Dxil;
      fcshaders::CompileOutput out;
      switch (which) {
        case BuiltinShader::kTriangleVS: out = compiler.compileEntry(kTriangle, L"vs_6_0", L"vs_main", target, false); break;  // without -fvk-invert-y: NVRHI flips the viewport
        case BuiltinShader::kTrianglePS: out = compiler.compileEntry(kTriangle, L"ps_6_0", L"ps_main", target); break;
        case BuiltinShader::kSolidPS: out = compiler.compileEntry(kSolid, L"ps_6_0", L"main", target); break;
        case BuiltinShader::kBlitVS: out = compiler.compileEntry(kBlit, L"vs_6_0", L"vs_main", target, false); break;
        case BuiltinShader::kBlitPS: out = compiler.compileEntry(kBlit, L"ps_6_0", L"ps_main", target); break;
        case BuiltinShader::kBlitGammaPS: out = compiler.compileEntry(kBlit, L"ps_6_0", L"ps_gamma", target); break;
        default: break;
      }
      if (!out.ok) {
        if (error) *error = "built-in shader: " + out.messages;
        return nullptr;
      }
      cache.blobs[a][w] = std::move(out.blob);
    }
    blob = cache.blobs[a][w];
  }
  nvrhi::ShaderDesc d;
  const bool vertex = which == BuiltinShader::kTriangleVS || which == BuiltinShader::kBlitVS;
  d.shaderType = vertex ? nvrhi::ShaderType::Vertex : nvrhi::ShaderType::Pixel;
  d.entryName = vertex ? "vs_main"
                : (which == BuiltinShader::kTrianglePS || which == BuiltinShader::kBlitPS) ? "ps_main"
                : which == BuiltinShader::kBlitGammaPS                                     ? "ps_gamma"
                                                                                           : "main";
  d.debugName = "builtin";
  return device->createShader(d, blob.data(), blob.size());
}

}  // namespace fcp::nr
