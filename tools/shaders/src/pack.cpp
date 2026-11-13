// fcshaders: pack file and disk cache (pack.h). Original code of this project.
#include "fcshaders/pack.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <thread>

#include <xxh3.h>

#include "fcshaders/abi.h"

namespace fs = std::filesystem;

namespace fcshaders {

namespace {

struct PackHeader {
    uint32_t magic = kPackMagic;
    uint32_t version = kPackVersion;
    uint32_t target = 0;
    uint32_t count = 0;
    uint64_t stamp = 0;
    uint64_t translator = 0;
    uint64_t indexOffset = 0;
    uint64_t indexHash = 0;
    uint32_t abiVersion = kAbiVersion;
    uint32_t flags = 0;
    uint64_t reserved = 0;
};
static_assert(sizeof(PackHeader) == 64, "pack header");

struct CacheHeader {
    uint32_t magic = 0x45534346u;  // 'FCSE'
    uint32_t version = 1;
    uint32_t target = 0;
    uint32_t kind = 0;
    uint64_t stamp = 0;
    uint64_t containerHash = 0;
    uint32_t codeSize = 0;
    uint32_t reflectionSize = 0;
};
static_assert(sizeof(CacheHeader) == 40, "cache header");

bool replaceFile(const std::string& path, const std::vector<uint8_t>& head, const std::vector<uint8_t>* parts[],
                 size_t partCount, std::string* error) {
    // Unique temporary name per thread and process: several writers may work at the same time.
    const std::string temp = path + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp";
    std::FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) {
        if (error) *error = "cannot create " + temp;
        return false;
    }
    bool ok = head.empty() || std::fwrite(head.data(), 1, head.size(), f) == head.size();
    for (size_t i = 0; ok && i < partCount; i++)
        ok = parts[i]->empty() || std::fwrite(parts[i]->data(), 1, parts[i]->size(), f) == parts[i]->size();
    ok = std::fclose(f) == 0 && ok;
    std::error_code ec;
    if (ok) {
        fs::rename(temp, path, ec);
        ok = !ec;
    }
    if (!ok) {
        fs::remove(temp, ec);
        if (error) *error = "cannot write " + path;
    }
    return ok;
}

template <class T>
std::vector<uint8_t> bytesOf(const T& v) {
    std::vector<uint8_t> b(sizeof(T));
    std::memcpy(b.data(), &v, sizeof(T));
    return b;
}

}  // namespace

const char* packTargetName(PackTarget target) { return target == PackTarget::Dxil ? "dxil" : "spv"; }

// --- PackWriter -------------------------------------------------------------------------------------------

bool PackWriter::add(uint64_t containerHash, ShaderKind kind, const std::vector<uint8_t>& code,
                     const Reflection& reflection) {
    for (const Item& i : entries_)
        if (i.containerHash == containerHash) return false;
    entries_.push_back({containerHash, reflection.ucodeHash, reflection.inputHash, uint32_t(kind), code,
                        serializeReflection(reflection)});
    return true;
}

bool PackWriter::write(const std::string& path, std::string* error) const {
    std::vector<const Item*> sorted;
    for (const Item& i : entries_) sorted.push_back(&i);
    std::sort(sorted.begin(), sorted.end(), [](const Item* a, const Item* b) { return a->containerHash < b->containerHash; });
    std::vector<uint8_t> data;
    std::vector<uint8_t> index;
    uint64_t offset = sizeof(PackHeader);
    for (const Item* i : sorted) {
        const uint64_t fields[4] = {i->containerHash, i->ucodeHash, i->inputHash, offset};
        const uint32_t sizes[4] = {uint32_t(i->code.size()), uint32_t(i->reflection.size()), i->kind, 0};
        index.insert(index.end(), reinterpret_cast<const uint8_t*>(fields), reinterpret_cast<const uint8_t*>(fields) + 32);
        index.insert(index.end(), reinterpret_cast<const uint8_t*>(sizes), reinterpret_cast<const uint8_t*>(sizes) + 16);
        data.insert(data.end(), i->code.begin(), i->code.end());
        data.insert(data.end(), i->reflection.begin(), i->reflection.end());
        offset += i->code.size() + i->reflection.size();
    }
    PackHeader h;
    h.target = uint32_t(target_);
    h.count = uint32_t(sorted.size());
    h.stamp = stamp_;
    h.translator = translatorHash();
    h.indexOffset = offset;
    h.indexHash = XXH3_64bits(index.data(), index.size());
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const std::vector<uint8_t>* parts[] = {&data, &index};
    return replaceFile(path, bytesOf(h), parts, 2, error);
}

// --- PackReader -------------------------------------------------------------------------------------------

PackReader::~PackReader() { close(); }

void PackReader::close() {
    std::lock_guard lock(mutex_);
    if (file_) std::fclose(file_);
    file_ = nullptr;
    index_.clear();
}

bool PackReader::open(const std::string& path, uint64_t expectedStamp, std::string* error) {
    close();
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        if (error) *error = "missing: " + path;
        return false;
    }
    PackHeader h;
    auto fail = [&](const std::string& why) {
        std::fclose(f);
        if (error) *error = path + ": " + why;
        return false;
    };
    if (std::fread(&h, sizeof(h), 1, f) != 1) return fail("header too short");
    if (h.magic != kPackMagic || h.version != kPackVersion) return fail("not a pack of this version");
    if (h.abiVersion != kAbiVersion) return fail("different ABI version " + std::to_string(h.abiVersion));
    std::vector<Index> index(h.count);
    if (_fseeki64(f, int64_t(h.indexOffset), SEEK_SET) != 0 ||
        (h.count && std::fread(index.data(), sizeof(Index), h.count, f) != h.count))
        return fail("index not readable");
    static_assert(sizeof(Index) == 48, "pack index entry");
    if (XXH3_64bits(index.data(), index.size() * sizeof(Index)) != h.indexHash) return fail("index corrupt");
    for (size_t i = 1; i < index.size(); i++)
        if (index[i - 1].containerHash >= index[i].containerHash) return fail("index not sorted");
    std::lock_guard lock(mutex_);
    file_ = f;
    index_ = std::move(index);
    target_ = PackTarget(h.target);
    stamp_ = h.stamp;
    stale_ = expectedStamp != 0 && h.stamp != expectedStamp;
    return true;
}

const PackReader::Index* PackReader::lookup(uint64_t hash) const {
    auto it = std::lower_bound(index_.begin(), index_.end(), hash,
                               [](const Index& e, uint64_t h) { return e.containerHash < h; });
    return it != index_.end() && it->containerHash == hash ? &*it : nullptr;
}

bool PackReader::contains(uint64_t hash) const { return lookup(hash) != nullptr; }

std::vector<uint64_t> PackReader::hashes() const {
    std::vector<uint64_t> out;
    for (const Index& e : index_) out.push_back(e.containerHash);
    return out;
}

bool PackReader::find(uint64_t hash, PackEntry& out) const {
    const Index* e = lookup(hash);
    if (!e) return false;
    std::vector<uint8_t> bytes(size_t(e->codeSize) + e->reflectionSize);
    {
        std::lock_guard lock(mutex_);
        if (!file_ || _fseeki64(file_, int64_t(e->offset), SEEK_SET) != 0 ||
            std::fread(bytes.data(), 1, bytes.size(), file_) != bytes.size())
            return false;
    }
    out.containerHash = hash;
    out.kind = ShaderKind(e->kind);
    out.code.assign(bytes.begin(), bytes.begin() + e->codeSize);
    return deserializeReflection(bytes.data() + e->codeSize, e->reflectionSize, out.reflection);
}

// --- DiskCache --------------------------------------------------------------------------------------------

bool DiskCache::open(const std::string& dir, PackTarget target, uint64_t stamp, std::string* error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        if (error) *error = "cannot create the cache folder " + dir;
        return false;
    }
    dir_ = dir;
    target_ = target;
    stamp_ = stamp;
    return true;
}

std::string DiskCache::pathOf(uint64_t hash) const {
    char name[64];
    std::snprintf(name, sizeof(name), "%016llX.%s.fcse", (unsigned long long)hash, packTargetName(target_));
    return (fs::path(dir_) / name).string();
}

bool DiskCache::get(uint64_t hash, PackEntry& out) const {
    if (dir_.empty()) return false;
    std::FILE* f = std::fopen(pathOf(hash).c_str(), "rb");
    if (!f) return false;
    CacheHeader h, want;
    bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == want.magic && h.version == want.version &&
              h.target == uint32_t(target_) && h.stamp == stamp_ && h.containerHash == hash;
    std::vector<uint8_t> bytes;
    if (ok) {
        bytes.resize(size_t(h.codeSize) + h.reflectionSize);
        ok = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size();
    }
    std::fclose(f);
    if (!ok) return false;
    out.containerHash = hash;
    out.kind = ShaderKind(h.kind);
    out.code.assign(bytes.begin(), bytes.begin() + h.codeSize);
    return deserializeReflection(bytes.data() + h.codeSize, h.reflectionSize, out.reflection);
}

bool DiskCache::put(const PackEntry& e, std::string* error) const {
    if (dir_.empty()) return false;
    CacheHeader h;
    h.target = uint32_t(target_);
    h.kind = uint32_t(e.kind);
    h.stamp = stamp_;
    h.containerHash = e.containerHash;
    const std::vector<uint8_t> reflection = serializeReflection(e.reflection);
    h.codeSize = uint32_t(e.code.size());
    h.reflectionSize = uint32_t(reflection.size());
    const std::vector<uint8_t>* parts[] = {&e.code, &reflection};
    return replaceFile(pathOf(e.containerHash), bytesOf(h), parts, 2, error);
}

}  // namespace fcshaders
