// Built-in renderer shaders (test triangle, signal color for shader marking and missing shaders), compiled at
// runtime with DXC. They use the same layout as the translated game shaders (translated_layout.h) so that a
// replacement pipeline fits any draw.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "renderer/host_device.h"

namespace fcp::nr {

enum class BuiltinShader : uint8_t {
  kTriangleVS,  // triangle from SV_VertexID, color per vertex (no vertex buffer)
  kTrianglePS,
  kSolidPS,     // signal color from DrawConstants::clipPlanes[0] (shader marking, missing shader)
  kBlitVS,      // full-screen triangle; UV = clipPlanes[1].xy + uv01 * clipPlanes[1].zw (resolve, present)
  kBlitPS,      // Tex2D[textureIndex[0]] with sampler pixelSamplers[0]
  kBlitGammaPS, // like kBlitPS, then per channel the gamma ramp Tex2D[textureIndex[1]] (256x1, linearly interpolated)
  kCount
};

// Compiles (once per process and API) and creates the shader object. DXC must be loaded
// (fcshaders::loadDxc). null on failure (error).
nvrhi::ShaderHandle CreateBuiltinShader(nvrhi::IDevice* device, HostApi api, BuiltinShader which, std::string* error);

}  // namespace fcp::nr
