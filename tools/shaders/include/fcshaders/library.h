// fcshaders as a library: a container in, reflection data plus DXIL and/or SPIR-V out.
//
// This is the layer shared by the renderer (src/renderer, ShaderLibrary) and the tool (tool/main.cpp).
// Original code of this project; the parser, translator and DXC binding below it come from King Kong
// Recompiled (MIT) and XenosRecomp (MIT), see xenosrecomp/CHANGES.md.
//
// Key everywhere: containerHash = XXH3-64 over the container's bytes (virtualSize + physicalSize from the
// header). It is unique for what the game passes to CreateVertexShader/CreatePixelShader; the
// microcode hash (ucodeHash) and the translator's input hash (inputHash) are in the reflection and feed
// the pipeline keys (src/renderer/kk/backend/pipeline_cache.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fcshaders/compiler.h"
#include "fcshaders/container.h"
#include "fcshaders/translator.h"

namespace fcshaders {

// Container bytes according to the header (virtualSize + physicalSize), 0 if not a container 0x102A0E00/01 or
// larger than maxSize.
size_t containerSize(const uint8_t* data, size_t maxSize);
uint64_t containerHash(const uint8_t* data, size_t size);

// What the renderer needs to bind a translated shader (in addition to the code).
struct Reflection {
    ShaderKind kind = ShaderKind::Pixel;
    uint64_t containerHash = 0;
    uint64_t ucodeHash = 0;        // XXH3-64 of the microcode (ShaderInfo::ucodeHash)
    uint64_t inputHash = 0;        // translationInputHash
    uint32_t instructions = 0;     // microcode instructions (physicalSize / 12)
    // Order = DrawConstants::vertexFetch[i] (usage + index of the declaration element).
    std::vector<VertexBinding> vertexBindings;
    // Order = DrawConstants::vertexSamplers[i] / pixelSamplers[i]; slot = fetch constant.
    std::vector<SamplerBinding> samplers;
    std::vector<TextureUse> textures;
    // CTAB entries (float4, bool, sampler registers, stage-relative); names for diagnostics only.
    std::vector<ConstantInfo> constants;
    // Bool constants the shader reads, as a mask in the format of DrawConstants::boolConstants
    // (vertex 0-127, pixel 128-255).
    uint32_t boolMask[8] = {};
    std::vector<uint16_t> floatLiterals;     // registers the shader answers itself
    std::vector<RegisterWrite> registerWrites;  // into the device shadow when the shader is set (abi.h)
    uint32_t pixelOutputs = 0;               // bits 0-3 colour targets, bit 4 depth
    uint32_t interpolators = 0;              // interpolator slots written (vertex)
    bool readsVPos = false;
    uint32_t tempRegisters = 0;
    bool generalControlFlow = false;
    bool dynamicRegisters = false;
    std::vector<std::string> warnings;
};

Reflection makeReflection(const ShaderInfo& info, const ShaderBindings& bindings, uint64_t containerHash);
std::vector<uint8_t> serializeReflection(const Reflection& r);
bool deserializeReflection(const uint8_t* data, size_t size, Reflection& out);

struct BuildOptions {
    bool dxil = true;
    bool spirv = true;
    bool keepHlsl = false;
};

struct BuildResult {
    bool ok = false;
    std::string stage = "ok";  // parse | translate | dxil | spirv | ok
    std::string error;
    Reflection reflection;
    std::string hlsl;          // only with keepHlsl (or on a DXC error)
    std::vector<uint8_t> dxil, spirv;
    double translateMs = 0, dxilMs = 0, spirvMs = 0;
};

// Container -> reflection + code. `compiler` may be null if no target is requested (reflection only).
// One compiler per thread (compiler.h).
BuildResult buildShader(const uint8_t* container, size_t size, Compiler* compiler, const BuildOptions& options);

// Stamp for packs and the disk cache: translator (translatorHash) and DXC identity (dxcIdentity). A different
// stamp means: the code would be different today, the entry is stale. Needs a loaded DXC.
uint64_t buildStamp();

}  // namespace fcshaders
