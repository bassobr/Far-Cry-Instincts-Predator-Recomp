// fcshaders: shader pack on disk (all disc shaders of one target) and disk cache (shaders translated later, at
// runtime). Original code of this project.
//
// Both store entries {code of one target (DXIL or SPIR-V), reflection (library.h)} under the key
// containerHash. Packs and cache entries are derived game data: they are created on the user's machine from
// their disc and are never committed or shipped.
//
// Pack file (little-endian):
//   header, 64 bytes: magic 'FCSP', version, target (0 DXIL, 1 SPIR-V), count,
//                   stamp (buildStamp), translatorHash, indexOffset, indexHash (XXH3 of the index), abiVersion,
//                   flags, 8 bytes free
//   data:           per entry code, then reflection (not aligned)
//   index:          count x 48 bytes, ascending by containerHash (binary search):
//                   containerHash, ucodeHash, inputHash, dataOffset (u64), codeSize, reflectionSize,
//                   kind, reserved
// A pack with a different stamp is stale (different translator or different DXC). PackReader opens it anyway
// and reports it via stale(); the caller decides whether it is used.
//
// Disk cache: a folder, one file per entry "<containerHash, 16 hex>.<dxil|spv>.fcse" with header
// {magic 'FCSE', version, target, kind, stamp, containerHash, codeSize, reflectionSize}, code, reflection.
// Written via a temporary file and a rename (several processes may write at the same time).
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fcshaders/library.h"

namespace fcshaders {

enum class PackTarget : uint32_t { Dxil = 0, Spirv = 1 };
const char* packTargetName(PackTarget target);  // "dxil" / "spv"

struct PackEntry {
    uint64_t containerHash = 0;
    ShaderKind kind = ShaderKind::Pixel;
    std::vector<uint8_t> code;
    Reflection reflection;
};

constexpr uint32_t kPackMagic = 0x50534346u;  // 'FCSP'
constexpr uint32_t kPackVersion = 1;

class PackWriter {
public:
    explicit PackWriter(PackTarget target, uint64_t stamp) : target_(target), stamp_(stamp) {}
    // One entry per containerHash; a second one with the same hash is ignored (false).
    bool add(uint64_t containerHash, ShaderKind kind, const std::vector<uint8_t>& code, const Reflection& reflection);
    size_t count() const { return entries_.size(); }
    // Writes the pack via "<path>.tmp" and a rename.
    bool write(const std::string& path, std::string* error = nullptr) const;

private:
    struct Item {
        uint64_t containerHash, ucodeHash, inputHash;
        uint32_t kind;
        std::vector<uint8_t> code, reflection;
    };
    PackTarget target_;
    uint64_t stamp_;
    std::vector<Item> entries_;
};

class PackReader {
public:
    PackReader() = default;
    ~PackReader();
    PackReader(const PackReader&) = delete;
    PackReader& operator=(const PackReader&) = delete;

    // Reads header and index; the entries are read from disk on find(). expectedStamp 0 = do not
    // check.
    bool open(const std::string& path, uint64_t expectedStamp, std::string* error = nullptr);
    void close();
    bool isOpen() const { return file_ != nullptr; }
    bool stale() const { return stale_; }
    PackTarget target() const { return target_; }
    uint64_t stamp() const { return stamp_; }
    size_t count() const { return index_.size(); }
    bool contains(uint64_t containerHash) const;
    // Thread-safe. False if the hash is missing or the entry cannot be read.
    bool find(uint64_t containerHash, PackEntry& out) const;
    // All hashes in index order (ascending).
    std::vector<uint64_t> hashes() const;

private:
    struct Index {
        uint64_t containerHash, ucodeHash, inputHash, offset;
        uint32_t codeSize, reflectionSize, kind, reserved;
    };
    const Index* lookup(uint64_t containerHash) const;
    std::FILE* file_ = nullptr;
    mutable std::mutex mutex_;
    std::vector<Index> index_;
    PackTarget target_ = PackTarget::Dxil;
    uint64_t stamp_ = 0;
    bool stale_ = false;
};

class DiskCache {
public:
    DiskCache() = default;
    // Creates the folder. stamp: buildStamp() of the running process.
    bool open(const std::string& dir, PackTarget target, uint64_t stamp, std::string* error = nullptr);
    bool isOpen() const { return !dir_.empty(); }
    // Thread-safe (each entry is its own file). get: false if missing, with a different stamp or damaged.
    bool get(uint64_t containerHash, PackEntry& out) const;
    bool put(const PackEntry& entry, std::string* error = nullptr) const;
    std::string pathOf(uint64_t containerHash) const;

private:
    std::string dir_;
    PackTarget target_ = PackTarget::Dxil;
    uint64_t stamp_ = 0;
};

}  // namespace fcshaders
