// fcshaders: container -> reflection + DXIL/SPIR-V (library.h). Original code of this project.
#include "fcshaders/library.h"

#include <chrono>
#include <cstring>

#include <xxh3.h>

namespace fcshaders {

namespace {

uint32_t loadBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint32_t v) { bytes.push_back(uint8_t(v)); }
    void u16(uint32_t v) { u8(v), u8(v >> 8); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) bytes.push_back(uint8_t(v >> (i * 8)));
    }
    void u64(uint64_t v) { u32(uint32_t(v)), u32(uint32_t(v >> 32)); }
    void str(const std::string& s) {
        u32(uint32_t(s.size()));
        bytes.insert(bytes.end(), s.begin(), s.end());
    }
};

struct Reader {
    const uint8_t* p;
    size_t size;
    size_t at = 0;
    bool ok = true;
    uint32_t u8() { return at + 1 <= size ? p[at++] : (ok = false, 0u); }
    uint32_t u16() {
        uint32_t a = u8();
        return a | (u8() << 8);
    }
    uint32_t u32() {
        if (at + 4 > size) return ok = false, 0;
        uint32_t v;
        std::memcpy(&v, p + at, 4);
        at += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t lo = u32();
        return lo | (uint64_t(u32()) << 32);
    }
    std::string str() {
        uint32_t n = u32();
        if (!ok || at + n > size) return ok = false, std::string();
        std::string s(reinterpret_cast<const char*>(p + at), n);
        at += n;
        return s;
    }
};

double msSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

constexpr uint32_t kReflectionMagic = 0x4C524346u;  // 'FCRL'
constexpr uint32_t kReflectionVersion = 1;

}  // namespace

size_t containerSize(const uint8_t* data, size_t maxSize) {
    if (!data || maxSize < 24) return 0;
    const uint32_t magic = loadBE32(data);
    if ((magic & 0xFFFFFFFEu) != 0x102A0E00u) return 0;
    const uint64_t size = uint64_t(loadBE32(data + 4)) + loadBE32(data + 8);
    if (size < 24 || size > maxSize || size > (64u << 20)) return 0;
    return size_t(size);
}

uint64_t containerHash(const uint8_t* data, size_t size) { return XXH3_64bits(data, size); }

Reflection makeReflection(const ShaderInfo& info, const ShaderBindings& b, uint64_t hash) {
    Reflection r;
    r.kind = info.kind;
    r.containerHash = hash;
    r.ucodeHash = info.ucodeHash;
    r.inputHash = translationInputHash(info);
    r.instructions = info.physicalSize / 12;
    r.vertexBindings = b.vertexBindings;
    r.samplers = b.samplers;
    r.textures = b.textures;
    r.constants = info.constants;
    const uint32_t base = info.kind == ShaderKind::Pixel ? 128 : 0;
    for (const ConstantInfo& c : info.constants) {
        if (c.registerSet != RegisterSet::Bool) continue;
        for (uint32_t i = 0; i < c.registerCount; i++) {
            const uint32_t n = base + c.registerIndex + i;
            if (n < 256) r.boolMask[n >> 5] |= 1u << (n & 31);
        }
    }
    for (const LiteralConstant& l : info.literals) r.floatLiterals.push_back(l.registerIndex);
    r.registerWrites = b.registerWrites;
    r.pixelOutputs = b.pixelOutputs;
    r.interpolators = b.interpolators;
    r.readsVPos = info.readsPixelPosition;
    r.tempRegisters = b.tempRegisters;
    r.generalControlFlow = b.generalControlFlow;
    r.dynamicRegisters = b.dynamicRegisters;
    r.warnings = b.warnings;
    return r;
}

std::vector<uint8_t> serializeReflection(const Reflection& r) {
    Writer w;
    w.u32(kReflectionMagic);
    w.u32(kReflectionVersion);
    w.u32(uint32_t(r.kind));
    w.u64(r.containerHash);
    w.u64(r.ucodeHash);
    w.u64(r.inputHash);
    w.u32(r.instructions);
    w.u32(uint32_t(r.vertexBindings.size()));
    for (const auto& v : r.vertexBindings) w.u8(v.raw), w.u8(uint32_t(v.usage)), w.u8(v.usageIndex), w.u8(v.fetchConstant);
    w.u32(uint32_t(r.samplers.size()));
    for (const auto& s : r.samplers) {
        w.u8(s.slot), w.u8(uint32_t(s.dimension)), w.u8(s.magFilter), w.u8(s.minFilter);
        w.u8(s.mipFilter), w.u8(s.anisoFilter), w.u8(s.volMagFilter), w.u8(s.volMinFilter);
    }
    w.u32(uint32_t(r.textures.size()));
    for (const auto& t : r.textures) w.u8(t.slot), w.u8(uint32_t(t.dimension));
    w.u32(uint32_t(r.constants.size()));
    for (const auto& c : r.constants) {
        w.str(c.name);
        w.u8(uint32_t(c.registerSet));
        w.u16(c.registerIndex), w.u16(c.registerCount), w.u16(c.typeClass), w.u16(c.type);
        w.u16(c.rows), w.u16(c.columns), w.u16(c.elements);
    }
    for (uint32_t m : r.boolMask) w.u32(m);
    w.u32(uint32_t(r.floatLiterals.size()));
    for (uint16_t l : r.floatLiterals) w.u16(l);
    w.u32(uint32_t(r.registerWrites.size()));
    for (const auto& rw : r.registerWrites) {
        w.u16(rw.offset);
        w.u32(uint32_t(rw.values.size()));
        for (uint32_t v : rw.values) w.u32(v);
    }
    w.u32(r.pixelOutputs);
    w.u32(r.interpolators);
    w.u32(r.tempRegisters);
    w.u32((r.readsVPos ? 1u : 0u) | (r.generalControlFlow ? 2u : 0u) | (r.dynamicRegisters ? 4u : 0u));
    w.u32(uint32_t(r.warnings.size()));
    for (const auto& s : r.warnings) w.str(s);
    return w.bytes;
}

bool deserializeReflection(const uint8_t* data, size_t size, Reflection& r) {
    Reader in{data, size};
    if (in.u32() != kReflectionMagic || in.u32() != kReflectionVersion) return false;
    r = {};
    r.kind = ShaderKind(in.u32());
    r.containerHash = in.u64();
    r.ucodeHash = in.u64();
    r.inputHash = in.u64();
    r.instructions = in.u32();
    uint32_t n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 64; i++) {
        VertexBinding v;
        v.raw = in.u8() != 0;
        v.usage = DeclUsage(in.u8());
        v.usageIndex = uint8_t(in.u8());
        v.fetchConstant = uint8_t(in.u8());
        r.vertexBindings.push_back(v);
    }
    n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 64; i++) {
        SamplerBinding s;
        s.slot = uint8_t(in.u8());
        s.dimension = TextureDimension(in.u8());
        s.magFilter = uint8_t(in.u8()), s.minFilter = uint8_t(in.u8()), s.mipFilter = uint8_t(in.u8());
        s.anisoFilter = uint8_t(in.u8()), s.volMagFilter = uint8_t(in.u8()), s.volMinFilter = uint8_t(in.u8());
        r.samplers.push_back(s);
    }
    n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 64; i++) {
        TextureUse t;
        t.slot = uint8_t(in.u8());
        t.dimension = TextureDimension(in.u8());
        r.textures.push_back(t);
    }
    n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 4096; i++) {
        ConstantInfo c;
        c.name = in.str();
        c.registerSet = RegisterSet(in.u8());
        c.registerIndex = uint16_t(in.u16()), c.registerCount = uint16_t(in.u16());
        c.typeClass = uint16_t(in.u16()), c.type = uint16_t(in.u16());
        c.rows = uint16_t(in.u16()), c.columns = uint16_t(in.u16()), c.elements = uint16_t(in.u16());
        r.constants.push_back(std::move(c));
    }
    for (uint32_t& m : r.boolMask) m = in.u32();
    n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 4096; i++) r.floatLiterals.push_back(uint16_t(in.u16()));
    n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 4096; i++) {
        RegisterWrite rw;
        rw.offset = uint16_t(in.u16());
        const uint32_t count = in.u32();
        for (uint32_t k = 0; in.ok && k < count && k < 65536; k++) rw.values.push_back(in.u32());
        r.registerWrites.push_back(std::move(rw));
    }
    r.pixelOutputs = in.u32();
    r.interpolators = in.u32();
    r.tempRegisters = in.u32();
    const uint32_t flags = in.u32();
    r.readsVPos = flags & 1, r.generalControlFlow = flags & 2, r.dynamicRegisters = flags & 4;
    n = in.u32();
    for (uint32_t i = 0; in.ok && i < n && i < 256; i++) r.warnings.push_back(in.str());
    return in.ok && in.at == size;
}

BuildResult buildShader(const uint8_t* container, size_t size, Compiler* compiler, const BuildOptions& options) {
    BuildResult out;
    ParseResult p = parseContainer(container, size);
    if (!p.ok) {
        out.stage = "parse";
        out.error = p.error;
        return out;
    }
    const size_t used = containerSize(container, size);
    const uint64_t hash = containerHash(container, used ? used : size);
    auto t0 = std::chrono::steady_clock::now();
    TranslateResult t = translate(p.info);
    out.translateMs = msSince(t0);
    if (!t.ok) {
        out.stage = "translate";
        out.error = t.error;
        return out;
    }
    out.reflection = makeReflection(p.info, t.bindings, hash);
    if ((options.dxil || options.spirv) && (!compiler || !compiler->ok())) {
        out.stage = "dxil";
        out.error = "no DXC loaded";
        return out;
    }
    auto compileOne = [&](CompileTarget target, std::vector<uint8_t>& blob, double& ms, const char* stage) {
        auto a = std::chrono::steady_clock::now();
        CompileOutput c = compiler->compile(t.hlsl, p.info.kind, target);
        ms = msSince(a);
        if (!c.ok) {
            if (out.stage == "ok") out.stage = stage, out.error = c.messages;
            return false;
        }
        blob = std::move(c.blob);
        return true;
    };
    bool ok = true;
    if (options.dxil) ok = compileOne(CompileTarget::Dxil, out.dxil, out.dxilMs, "dxil") && ok;
    if (options.spirv) ok = compileOne(CompileTarget::Spirv, out.spirv, out.spirvMs, "spirv") && ok;
    if (options.keepHlsl || !ok) out.hlsl = std::move(t.hlsl);
    out.ok = ok;
    return out;
}

uint64_t buildStamp() {
    Writer w;
    w.u64(translatorHash());
    w.str(dxcIdentity());
    return XXH3_64bits(w.bytes.data(), w.bytes.size());
}

}  // namespace fcshaders
