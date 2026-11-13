// fcshaders: container adapter 0x102A0E00/01. Taken from King Kong Recompiled (commit e3e5d3f,
// native-renderer/shaders/src/container.cpp, MIT, tools/shaders/LICENSE-KingKongRecompiled.txt); King Kong's
// database code removed. Changes: tools/shaders/xenosrecomp/CHANGES.md.
#include "fcshaders/container.h"

#include <cstdio>
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

#include <xxh3.h>

namespace fcshaders {

namespace {

inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | uint16_t(p[1])); }

struct Reader {
    const uint8_t* data;
    size_t size;
    bool word(size_t offset, uint32_t& out) const {
        if (offset + 4 > size) return false;
        out = be32(data + offset);
        return true;
    }
    bool half(size_t offset, uint16_t& out) const {
        if (offset + 2 > size) return false;
        out = be16(data + offset);
        return true;
    }
    bool cstring(size_t offset, std::string& out) const {
        if (offset >= size) return false;
        size_t end = offset;
        while (end < size && data[end] != 0) ++end;
        if (end == size) return false;
        out.assign(reinterpret_cast<const char*>(data + offset), end - offset);
        return true;
    }
};

ParseResult fail(std::string message) {
    ParseResult r;
    r.ok = false;
    r.error = std::move(message);
    return r;
}

}  // namespace

const char* declUsageName(DeclUsage usage) {
    static const char* const names[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
                                        "TANGENT",  "BINORMAL",    "TESSFACTOR",   "POSITIONT", "COLOR", "FOG",
                                        "DEPTH",    "SAMPLE"};
    uint32_t i = uint32_t(usage);
    return i < 14 ? names[i] : "UNKNOWN";
}

uint64_t hashBytes(const void* data, size_t size) { return XXH3_64bits(data, size); }

ParseResult parseContainer(const uint8_t* data, size_t maxSize) {
    if (!data) return fail("null container");
    // The header is six words; the sizes bound everything else.
    if (maxSize < 24) return fail("container shorter than its header");
    uint32_t flags = be32(data);
    if ((flags & 0xFFFFFF00u) != 0x102A0E00u) return fail("not a 2005 XDK shader container (bad magic)");
    uint32_t virtualSize = be32(data + 4);
    uint32_t physicalSize = be32(data + 8);
    if (virtualSize < 24 || (virtualSize & 3) || (physicalSize & 3))
        return fail("bad virtual or physical size");
    if (physicalSize == 0 || physicalSize % 12 != 0) return fail("microcode size is not a multiple of 12 bytes");
    size_t total = size_t(virtualSize) + physicalSize;
    if (total > maxSize) return fail("container runs past the end of its data");

    Reader r{data, total};
    ParseResult result;
    ShaderInfo& info = result.info;
    info.kind = (flags & 1) ? ShaderKind::Vertex : ShaderKind::Pixel;
    info.virtualSize = virtualSize;
    info.physicalSize = physicalSize;

    // Microcode.
    info.ucode.resize(physicalSize / 4);
    for (size_t i = 0; i < info.ucode.size(); i++) info.ucode[i] = be32(data + virtualSize + i * 4);
    info.ucodeHash = hashBytes(data + virtualSize, physicalSize);

    uint32_t blockOffset = be32(data + 12);
    uint32_t ctabOffset = be32(data + 16);
    uint32_t tableOffset = be32(data + 20);
    // Two vertex shaders of the database have no constant table (offset 0).
    if (ctabOffset != 0 && ctabOffset + 4 + 28 > virtualSize) return fail("constant table offset out of range");
    if (tableOffset == 0 || tableOffset + 4 > virtualSize) return fail("binding table offset out of range");

    // Register block: literal constants. {u64 dirty mask, u64 ?, u32 list length, u32 ?, list}.
    if (blockOffset) {
        if (blockOffset + 24 > virtualSize) return fail("register block out of range");
        uint32_t listBytes = be32(data + blockOffset + 16);
        size_t at = blockOffset + 24, end = blockOffset + 24 + listBytes;
        if (end > virtualSize) return fail("register block list out of range");
        // Plain writes.
        while (at + 4 <= end) {
            uint16_t offset = be16(data + at), count = be16(data + at + 2);
            at += 4;
            if (count == 0) break;
            if (at + size_t(count) * 4 > end) return fail("register write list runs past the block");
            // offset is into the device's register image at +1152: float constants from 768
            // (+1920; vertex 0-255 then pixel 256-511, 16 bytes each), bool constants at 8960
            // (+10112, 8 dwords), loop constants at 8992 (+10144, 32 dwords).
            RegisterWrite write;
            write.offset = offset;
            for (uint32_t k = 0; k < count; k++) write.values.push_back(be32(data + at + k * 4));
            info.registerWrites.push_back(write);
            if (offset >= 768 && offset < 8960) {
                uint32_t byteOffset = offset - 768u;
                if ((byteOffset & 15) || (count & 3)) return fail("register write not aligned to float constants");
                uint32_t absReg = byteOffset / 16;
                if (absReg + count / 4 > 512) return fail("register write past the float constants");
                for (uint32_t k = 0; k < count / 4; k++) {
                    uint32_t reg = absReg + k;
                    uint32_t rel = info.kind == ShaderKind::Pixel ? (reg >= 256 ? reg - 256 : 0xFFFF) : reg;
                    if (rel > 255) return fail("literal constant register outside this stage's range");
                    LiteralConstant lit;
                    lit.registerIndex = uint16_t(rel);
                    for (int c = 0; c < 4; c++) lit.value[c] = be32(data + at + (k * 4 + c) * 4);
                    info.literals.push_back(lit);
                }
            } else if (offset >= 8960 && offset < 8992) {
                if ((offset & 3) || offset + count * 4 > 8992) return fail("bool constant write out of range");
                for (uint32_t k = 0; k < count; k++)
                    info.boolLiterals.push_back({(offset - 8960u) / 4 + k, be32(data + at + k * 4)});
            } else if (offset >= 8992 && offset < 9120) {
                if ((offset & 3) || offset + count * 4 > 9120) return fail("loop constant write out of range");
                for (uint32_t k = 0; k < count; k++)
                    info.loopLiterals.push_back({(offset - 8992u) / 4 + k, be32(data + at + k * 4)});
            } else {
                return fail("register write outside the shader constants (offset " + std::to_string(offset) + ")");
            }
            at += size_t(count) * 4;
        }
        // Masked writes {offset, count, (mask, value) pairs}: none in this game; refuse so
        // nothing is silently dropped.
        while (at + 4 <= end) {
            uint16_t count = be16(data + at + 2);
            at += 4;
            if (count == 0) break;
            return fail("masked register writes are not supported");
        }
    }

    // Constant table: u32 size, then D3DXSHADER_CONSTANTTABLE with offsets from its start.
    if (ctabOffset) {
        size_t ctab = ctabOffset + 4;
        uint32_t ctabSize = be32(data + ctabOffset);
        if (ctabOffset + 4 + ctabSize > virtualSize) return fail("constant table size out of range");
        uint32_t creator = be32(data + ctab + 4);
        uint32_t constants = be32(data + ctab + 12);
        uint32_t constantInfo = be32(data + ctab + 16);
        uint32_t target = be32(data + ctab + 24);
        (void)creator;
        // Stripped tables (creator 0, 2,137 vertex shaders of the database) leave stale bytes in
        // the target field (for example "_FUR"): no target then.
        if (target && (target >= ctabSize || !r.cstring(ctab + target, info.target))) info.target.clear();
        info.constantTable.assign(data + ctab, data + ctab + ctabSize);
        for (uint32_t i = 0; i < constants; i++) {
            size_t ci = ctab + constantInfo + size_t(i) * 20;
            if (ci + 20 > virtualSize) return fail("constant info out of range");
            ConstantInfo c;
            uint32_t nameOffset = be32(data + ci);
            if (!r.cstring(ctab + nameOffset, c.name)) return fail("constant name out of range");
            c.registerSet = RegisterSet(be16(data + ci + 4));
            c.registerIndex = be16(data + ci + 6);
            c.registerCount = be16(data + ci + 8);
            uint32_t typeInfo = be32(data + ci + 12);
            size_t ti = ctab + typeInfo;
            if (ti + 16 > virtualSize) return fail("constant type info out of range");
            c.typeClass = be16(data + ti);
            c.type = be16(data + ti + 2);
            c.rows = be16(data + ti + 4);
            c.columns = be16(data + ti + 6);
            c.elements = be16(data + ti + 8);
            info.constants.push_back(std::move(c));
        }
    }

    // Binding table.
    {
        size_t n = (virtualSize - tableOffset) / 4;
        info.bindingTable.resize(n);
        for (size_t i = 0; i < n; i++) info.bindingTable[i] = be32(data + tableOffset + i * 4);
        const auto& t = info.bindingTable;
        if (t.empty()) return fail("empty binding table");
        info.programControl = t[0];
        if (info.kind == ShaderKind::Vertex) {
            if (t.size() < 11) return fail("vertex binding table too short");
            size_t fetchStart = 10 + t[6];
            uint32_t nFetch = t[7], nWords = t[8];
            if (fetchStart + nFetch + nWords > t.size()) return fail("vertex binding table lists out of range");
            // Word 8 counts the words after the vfetch list. The interpolators are the first
            // SQ_PROGRAM_CNTL.VS_EXPORT_COUNT + 1 of them (bits 20-23 of word 0); in 20 shaders of the
            // database more words follow (instruction addresses, not semantics). Checked on all 4,240
            // vertex shaders: the semantics then match the export registers the microcode writes.
            uint32_t nInterp = std::min<uint32_t>(nWords, ((t[0] >> 20) & 0xF) + 1);
            uint32_t instructionCount = physicalSize / 12;
            for (uint32_t i = 0; i < nFetch; i++) {
                uint32_t e = t[fetchStart + i];
                FetchBinding f;
                f.address = uint16_t(e & 0xFFF);
                f.usage = DeclUsage((e >> 12) & 0xF);
                f.usageIndex = uint8_t((e >> 16) & 0xF);
                f.classHint = uint8_t((e >> 20) & 0xF);
                if (f.address >= instructionCount) return fail("vfetch binding address outside the microcode");
                if (uint32_t(f.usage) > 13) return fail("vfetch binding with an unknown usage");
                info.fetches.push_back(f);
            }
            for (uint32_t i = 0; i < nInterp; i++) {
                uint32_t e = t[fetchStart + nFetch + i];
                Interpolator o;
                o.usageIndex = uint8_t(e & 0xF);
                o.usage = DeclUsage((e >> 4) & 0xF);
                o.reg = uint8_t((e >> 8) & 0xF);
                o.mask = uint8_t((e >> 12) & 0xF);
                if (uint32_t(o.usage) > 13) return fail("interpolator with an unknown usage");
                info.interpolators.push_back(o);
            }
        } else {
            if (t.size() < 8) return fail("pixel binding table too short");
            // The interpolator count: word 6 is a mask of the interpolators (one bit each, from
            // bit 0), word 1 bits 8-15 a count that is 0 in about 700 of the database's 2,343 pixel
            // shaders although their descriptors follow word 8 as usual (found by the native
            // renderer: those shaders read zeros for every input). The larger of the two.
            uint32_t nInterp = std::max<uint32_t>((t[1] >> 8) & 0xFF, uint32_t(std::popcount(t[6] & 0xFFFFu)));
            if (8 + nInterp > t.size()) return fail("pixel interpolator list out of range");
            for (uint32_t i = 0; i < nInterp; i++) {
                uint32_t e = t[8 + i];
                Interpolator o;
                o.usageIndex = uint8_t(e & 0xF);
                o.usage = DeclUsage((e >> 4) & 0xF);
                o.reg = uint8_t((e >> 8) & 0xF);
                o.mask = uint8_t((e >> 12) & 0xF);
                if (uint32_t(o.usage) > 13) return fail("interpolator with an unknown usage");
                info.interpolators.push_back(o);
            }
            info.readsPixelPosition = (t[0] >> 18) & 1;  // SQ_PROGRAM_CNTL.param_gen
            info.pixelPositionRegister = uint8_t(nInterp);
        }
    }

    result.ok = true;
    return result;
}

ParseResult parseMicrocode(ShaderKind kind, const uint8_t* data, size_t size) {
    if (!data || size == 0 || size % 12 != 0) return fail("microcode size is not a multiple of 12 bytes");
    ParseResult result;
    ShaderInfo& info = result.info;
    info.kind = kind;
    info.rawMicrocode = true;
    info.physicalSize = uint32_t(size);
    info.ucode.resize(size / 4);
    for (size_t i = 0; i < info.ucode.size(); i++) info.ucode[i] = be32(data + i * 4);
    info.ucodeHash = hashBytes(data, size);
    result.ok = true;
    return result;
}

}  // namespace fcshaders
