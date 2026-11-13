// fcshaders: container adapter for the Xbox 360 shader containers 0x102A0E00/01 (XDK 2.0.2417).
//
// Taken from King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, commit e3e5d3f,
// native-renderer/shaders/include/kkshaders/container.h, MIT, see tools/shaders/LICENSE-KingKongRecompiled.txt).
// Changes for Predator (tools/shaders/xenosrecomp/CHANGES.md): namespace fcshaders; King Kong's database
// `xeshaders.bin` and the KK-specific switches (AO, water reflection, parallax) removed.
// Predator stores each container separately as an `.fxo` in shaders.dat/common.dat.
//
// Layout (big-endian words; checked on all 1,965 unique Predator containers):
//   word 0  0x102A0E00 pixel / 0x102A0E01 vertex
//   word 1  virtualSize: header and tables
//   word 2  physicalSize: the microcode after them
//   word 3  offset of the register block (0 = none): literal constants as {u16 offset into the device's
//           register image (+1152), u16 count, values}; 768-8959 float, 8960-8991 bool, 8992-9119 loop.
//   word 4  offset of the D3DX constant table (u32 size, then CTAB)
//   word 5  offset of the binding table: word 0 = SQ_PROGRAM_CNTL; VS: vfetch bindings
//           {address:12, usage:4, usageIndex:4, class:4} and interpolators; PS: interpolators, VPOS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fcshaders {

enum class ShaderKind : uint8_t { Vertex = 0, Pixel = 1 };

// D3DDECLUSAGE
enum class DeclUsage : uint8_t {
    Position = 0, BlendWeight, BlendIndices, Normal, PointSize, TexCoord, Tangent, Binormal,
    TessFactor, PositionT, Color, Fog, Depth, Sample
};
const char* declUsageName(DeclUsage usage);  // "POSITION" etc.

// D3DXREGISTER_SET
enum class RegisterSet : uint8_t { Bool = 0, Int4 = 1, Float4 = 2, Sampler = 3 };

struct ConstantInfo {
    std::string name;
    RegisterSet registerSet = RegisterSet::Float4;
    uint16_t registerIndex = 0;
    uint16_t registerCount = 0;
    uint16_t typeClass = 0;    // D3DXPARAMETER_CLASS
    uint16_t type = 0;         // D3DXPARAMETER_TYPE
    uint16_t rows = 0, columns = 0, elements = 0;
};

// A literal float4 constant the shader expects in its constant registers (the HLSL
// compiler's "def" constants), stage-relative register index.
struct LiteralConstant {
    uint16_t registerIndex = 0;
    uint32_t value[4] = {};
};

// One write of the register block, as the XDK applies it when the shader is set: `values`
// go to the device's register image at +1152 + offset (big-endian words as stored).
struct RegisterWrite {
    uint16_t offset = 0;
    std::vector<uint32_t> values;
};

// Vertex shader: which declaration element a vfetch instruction reads.
struct FetchBinding {
    uint16_t address = 0;      // instruction address (12-byte units) of the vfetch
    DeclUsage usage = DeclUsage::Position;
    uint8_t usageIndex = 0;
    uint8_t classHint = 0;     // unknown 4-bit field (0..3), kept for reference
};

// Vertex shader output or pixel shader input: a semantic bound to a register.
struct Interpolator {
    DeclUsage usage = DeclUsage::TexCoord;
    uint8_t usageIndex = 0;
    uint8_t reg = 0;           // vertex: export register o<reg>; pixel: input register r<reg>
    uint8_t mask = 0xF;        // components the source declared
};

struct ShaderInfo {
    ShaderKind kind = ShaderKind::Pixel;
    uint32_t virtualSize = 0;
    uint32_t physicalSize = 0;
    uint32_t programControl = 0;           // binding table word 0
    std::vector<uint32_t> ucode;           // microcode, host byte order
    uint64_t ucodeHash = 0;                // XXH3-64 of the microcode bytes as stored (big-endian)
    std::string target;                    // "vs_3_0" / "ps_3_0" (CTAB target)
    std::vector<ConstantInfo> constants;
    std::vector<LiteralConstant> literals;
    std::vector<std::pair<uint32_t, uint32_t>> loopLiterals;  // loop constant 0-31, packed value
    std::vector<std::pair<uint32_t, uint32_t>> boolLiterals;  // bool dword 0-7 (32 bools each), value
    std::vector<RegisterWrite> registerWrites;
    std::vector<FetchBinding> fetches;     // vertex only, in binding-table order
    std::vector<Interpolator> interpolators;
    bool readsPixelPosition = false;       // pixel: VPOS
    uint8_t pixelPositionRegister = 0;
    std::vector<uint32_t> bindingTable;    // raw, for reference
    std::vector<uint8_t> constantTable;    // the CTAB bytes (after its size word), for hashing
    // No container: microcode only (shaders the game's own compiler builds at run time and that
    // are captured from guest memory). Vertex fetches are taken as patched (instruction mode) and
    // interpolators are linked by register number.
    bool rawMicrocode = false;
};

struct ParseResult {
    bool ok = false;
    std::string error;
    ShaderInfo info;
};

// Parses one container. maxSize bounds the read (use SIZE_MAX for guest memory, where the
// size is only known from the header).
ParseResult parseContainer(const uint8_t* data, size_t maxSize);
inline ParseResult parseContainer(std::span<const uint8_t> bytes) { return parseContainer(bytes.data(), bytes.size()); }

// Bare microcode (big-endian bytes as in guest memory).
ParseResult parseMicrocode(ShaderKind kind, const uint8_t* data, size_t size);

// XXH3-64 over bytes (the cache key convention for microcode).
uint64_t hashBytes(const void* data, size_t size);

}  // namespace fcshaders
