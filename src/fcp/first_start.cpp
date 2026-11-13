// Reading the player's disc image on the first start (fcp/first_start.h).
//
// XDVDFS as in tools/xiso.py (ported there from FCI's tools/xiso.ps1, same author); DAT2 archives as in
// tools/xfat.py; container selection as in tools/shaders/fxo.py extract (shaders.fat, then common.fat; only
// containers 0x102A0E00/01; unique by SHA-1, the first occurrence's name, "common__" for common.fat). LZMA with the
// decoder of the LZMA SDK (Igor Pavlov, public domain, third_party/lzma-sdk, tools/get_lzma.ps1).
#include "fcp/first_start.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>

#include <rex/platform/file.h>

#include "LzmaDec.h"
#include "fcshaders/translator.h"
#include "fcp/build_process.h"
#include "fcp/env.h"
#include "fcp/hash.h"
#include "fcp/log.h"
#include "fcp/progress_window.h"
#include "fcp/user_paths.h"

namespace fcp::first_start {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSector = 2048;
constexpr uint64_t kPartitions[] = {0, 0x18300000, 0x2080000, 0xFD90000};
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string U8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

// Read-only random access to the image file (shared, so a running game or a copy does not block it).
class ImageFile {
 public:
  explicit ImageFile(const fs::path& path) {
    h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER size{};
    if (h_ != INVALID_HANDLE_VALUE && GetFileSizeEx(h_, &size)) size_ = uint64_t(size.QuadPart);
  }
  ~ImageFile() {
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
  }
  bool ok() const { return h_ != INVALID_HANDLE_VALUE; }
  uint64_t size() const { return size_; }
  bool Read(uint64_t offset, void* out, size_t n) {
    auto* p = static_cast<uint8_t*>(out);
    while (n) {
      OVERLAPPED ov{};
      ov.Offset = DWORD(offset);
      ov.OffsetHigh = DWORD(offset >> 32);
      const DWORD chunk = DWORD(std::min<size_t>(n, 1 << 24));
      DWORD got = 0;
      if (!ReadFile(h_, p, chunk, &got, &ov) || got == 0) return false;
      p += got;
      n -= got;
      offset += got;
    }
    return true;
  }

 private:
  HANDLE h_ = INVALID_HANDLE_VALUE;
  uint64_t size_ = 0;
};

struct Entry {
  std::string path;  // lower case, backslashes
  uint32_t sector = 0;
  uint32_t size = 0;
  bool dir = false;
};

bool FindPartition(ImageFile& f, uint64_t& partition, uint32_t& root_sector, uint32_t& root_size) {
  for (uint64_t p : kPartitions) {
    if (p + 0x10000 + 0x800 > f.size()) continue;
    uint8_t head[0x800];
    if (!f.Read(p + 0x10000, head, sizeof head)) continue;
    if (std::memcmp(head, kMagic, 20) == 0 && std::memcmp(head + 0x7EC, kMagic, 20) == 0) {
      partition = p;
      std::memcpy(&root_sector, head + 20, 4);
      std::memcpy(&root_size, head + 24, 4);
      return true;
    }
  }
  return false;
}

void Walk(ImageFile& f, uint64_t partition, uint32_t sector, uint32_t size, const std::string& prefix,
          std::vector<Entry>& out, int depth) {
  if (size == 0 || depth > 32) return;
  std::vector<uint8_t> table(size);
  if (!f.Read(partition + sector * kSector, table.data(), size)) return;
  std::vector<uint32_t> pending{0};
  std::unordered_set<uint32_t> seen;
  while (!pending.empty()) {
    const uint32_t off = pending.back();
    pending.pop_back();
    if (off + 14 > table.size() || !seen.insert(off).second) continue;
    uint16_t left, right;
    std::memcpy(&left, &table[off], 2);
    std::memcpy(&right, &table[off + 2], 2);
    if (left == 0xFFFF) continue;  // padding
    Entry e;
    std::memcpy(&e.sector, &table[off + 4], 4);
    std::memcpy(&e.size, &table[off + 8], 4);
    const uint8_t attr = table[off + 12], name_len = table[off + 13];
    if (off + 14 + name_len > table.size()) continue;
    e.path = prefix + Lower(std::string(reinterpret_cast<const char*>(&table[off + 14]), name_len));
    e.dir = (attr & 0x10) != 0;
    if (left) pending.push_back(uint32_t(left) * 4);
    if (right) pending.push_back(uint32_t(right) * 4);
    out.push_back(e);
    if (e.dir) Walk(f, partition, e.sector, e.size, e.path + "\\", out, depth + 1);
  }
}

std::string Normalize(std::string name) {
  std::replace(name.begin(), name.end(), '/', '\\');
  while (!name.empty() && name.front() == '\\') name.erase(0, 1);
  return Lower(name);
}

// Reads several files of one image in one pass over its directory.
class Disc {
 public:
  explicit Disc(const fs::path& image) : image_(image) {
    std::error_code ec;
    if (fs::is_directory(image, ec)) {
      folder_ = true;
      return;
    }
    file_ = std::make_unique<ImageFile>(image);
    if (!file_->ok()) {
      why_ = std::format("the disc image {} cannot be opened (error {})", U8(image), GetLastError());
      return;
    }
    uint32_t root_sector = 0, root_size = 0;
    if (!FindPartition(*file_, partition_, root_sector, root_size)) {
      why_ = std::format("{} is not an Xbox 360 disc image (no XDVDFS volume found)", U8(image));
      return;
    }
    std::vector<Entry> entries;
    Walk(*file_, partition_, root_sector, root_size, "", entries, 0);
    for (Entry& e : entries) entries_[e.path] = e;
  }
  const std::string& why() const { return why_; }
  bool ok() const { return why_.empty(); }

  bool Read(const std::string& name, std::vector<uint8_t>& out, std::string& why) {
    const std::string key = Normalize(name);
    if (folder_) {
      std::ifstream in(image_ / fs::path(key), std::ios::binary);
      if (!in) return why = std::format("{} is missing in the folder {}", name, U8(image_)), false;
      out.assign(std::istreambuf_iterator<char>(in), {});
      return true;
    }
    if (!ok()) return why = why_, false;
    const auto it = entries_.find(key);
    if (it == entries_.end() || it->second.dir) {
      return why = std::format("{} is missing in the disc image {}", name, U8(image_)), false;
    }
    out.resize(it->second.size);
    if (!file_->Read(partition_ + uint64_t(it->second.sector) * kSector, out.data(), out.size())) {
      return why = std::format("reading {} from the disc image failed (error {})", name, GetLastError()), false;
    }
    return true;
  }

 private:
  fs::path image_;
  bool folder_ = false;
  std::unique_ptr<ImageFile> file_;
  uint64_t partition_ = 0;
  std::map<std::string, Entry> entries_;
  std::string why_;
};

// ---- DAT2 (.fat/.dat), tools/xfat.py ----

struct FatEntry {
  uint32_t offset, stored, size, hash;
};

bool ReadFat(const std::vector<uint8_t>& fat, std::vector<FatEntry>& out, std::string& why) {
  if (fat.size() < 8 || std::memcmp(fat.data(), "DAT2", 4) != 0) return why = "no DAT2 archive", false;
  uint32_t n;
  std::memcpy(&n, fat.data() + 4, 4);
  if (fat.size() != 8 + 16ull * n) return why = "DAT2 table size does not match its count", false;
  out.resize(n);
  std::memcpy(out.data(), fat.data() + 8, 16ull * n);
  return true;
}

void* LzmaAlloc(ISzAllocPtr, size_t size) { return HeapAlloc(GetProcessHeap(), 0, size); }
void LzmaFree(ISzAllocPtr, void* p) {
  if (p) HeapFree(GetProcessHeap(), 0, p);
}
const ISzAlloc kLzmaAlloc = {LzmaAlloc, LzmaFree};

// Stored != unpacked: 1 tag byte, then an LZMA "alone" stream (13 byte header: props, dictionary, u64 size).
bool Unpack(const uint8_t* raw, const FatEntry& e, std::vector<uint8_t>& out) {
  if (e.stored == e.size) {
    out.assign(raw, raw + e.size);
    return true;
  }
  if (e.stored < 1 + 13) return false;
  const uint8_t* stream = raw + 1;
  out.resize(e.size);
  SizeT dest_len = e.size;
  SizeT src_len = e.stored - 1 - 13;
  ELzmaStatus status;
  const SRes r = LzmaDecode(out.data(), &dest_len, stream + 13, &src_len, stream, LZMA_PROPS_SIZE, LZMA_FINISH_ANY,
                            &status, &kLzmaAlloc);
  return r == SZ_OK && dest_len == e.size;
}

uint32_t BigU32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

std::string ReadTextValue(const fs::path& file, const std::string& key) {
  std::ifstream in(file);
  std::string line;
  while (std::getline(in, line)) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    auto trim = [](std::string& s) {
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(0, 1);
    };
    trim(k);
    trim(v);
    if (k != key) continue;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    return v;
  }
  return {};
}

}  // namespace

fs::path ImagePath() {
  wchar_t buffer[4096];
  const DWORD n = GetEnvironmentVariableW(L"FCP_GAME_ISO", buffer, 4096);
  if (n == 0 || n >= 4096) return {};
  fs::path p(std::wstring(buffer, n));
  if (p.is_relative()) p = fcp::paths::Root() / p;
  return p;
}

bool ReadDiscFile(const fs::path& image, const std::string& name, std::vector<uint8_t>& out, std::string& why) {
  Disc disc(image);
  if (!disc.ok()) return why = disc.why(), false;
  return disc.Read(name, out, why);
}

std::string XexSha256(const fs::path& image, const fs::path& cache, std::string& why) {
  std::error_code ec;
  const auto size = fs::is_directory(image, ec) ? 0 : fs::file_size(image, ec);
  const auto time = fs::last_write_time(image, ec).time_since_epoch().count();
  const std::string key = std::format("{}|{}|{}", U8(fs::absolute(image, ec)), size, time);
  if (std::ifstream in(cache); in) {
    std::string k, h;
    std::getline(in, k);
    std::getline(in, h);
    if (k == key && h.size() == 64) return h;
  }
  std::vector<uint8_t> xex;
  if (!ReadDiscFile(image, "default.xex", xex, why)) return {};
  const std::string h = fcp::hash::Sha256(xex.data(), xex.size());
  fs::create_directories(cache.parent_path(), ec);
  std::ofstream(cache) << key << "\n" << h << "\n";
  return h;
}

std::string PackTarget() {
  std::string backend = Lower(fcp::env::Get("FCP_RENDER_BACKEND"));
  if (backend.empty()) backend = Lower(ReadTextValue(rex::platform::GetExecutableFolder() / "renderer.toml", "backend"));
  return backend == "d3d12" ? "dxil" : "spirv";
}

fs::path PackPath(const std::string& target) {
  return fcp::paths::Root() / "shaders" / ("predator." + target + ".fcpack");
}

// The translator a pack was built with (pack header, tools/shaders/include/fcshaders/pack.h: translatorHash at byte
// 24) against this build's: another translator (kTranslatorVersion, e.g. 7 for the render scale, grafik-extras.md)
// makes the renderer reject the pack and translate every shader at run time. Without a readable header: as before.
bool PackFromThisTranslator(const fs::path& pack) {
  std::ifstream in(pack, std::ios::binary);
  uint8_t head[32] = {};
  if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return true;
  uint32_t magic = 0;
  uint64_t translator = 0;
  std::memcpy(&magic, head, 4);
  std::memcpy(&translator, head + 24, 8);
  if (magic != 0x50534346u) return true;  // 'FCSP'
  return translator == fcshaders::translatorHash();
}

bool UseExistingPack(const std::string& target) {
  if (!fcp::env::Get("FCP_SHADER_PACK").empty()) return true;
  std::error_code ec;
  const fs::path next_to_exe = rex::platform::GetExecutableFolder() / "shaders" / ("predator." + target + ".fcpack");
  if (fs::exists(next_to_exe, ec)) {
    if (PackFromThisTranslator(next_to_exe)) return true;
    FCPLOG("first start: shader pack {} is from another translator version", U8(next_to_exe));
  }
  const fs::path own = PackPath(target);
  if (!fs::exists(own, ec)) return false;
  if (!PackFromThisTranslator(own)) {
    FCPLOG("first start: shader pack {} is from another translator version, built again", U8(own));
    return false;
  }
  SetEnvironmentVariableW(L"FCP_SHADER_PACK", own.wstring().c_str());
  FCPLOG("first start: shader pack {} (FCP_SHADER_PACK for the renderer)", U8(own));
  return true;
}

bool BuildShaderPack(const fs::path& image, const fs::path& work, const std::string& target, const fs::path& log,
                     std::string& why) {
  const auto t0 = std::chrono::steady_clock::now();
  const fs::path exe_dir = rex::platform::GetExecutableFolder();
  const fs::path tool = exe_dir / "fcshaders.exe";
  std::error_code ec;
  if (!fs::exists(tool, ec)) return why = "fcshaders.exe is missing next to predator.exe", false;
  fcp::progress::SetStage(fcp::progress::Stage::kShaders, L"Reading the shaders from the disc image");
  const fs::path fxo = work / "fxo";
  fs::remove_all(work, ec);
  fs::create_directories(fxo, ec);
  Disc disc(image);
  if (!disc.ok()) return why = disc.why(), false;
  std::set<std::string> seen;
  size_t containers = 0, unique = 0;
  for (const char* arc : {"shaders", "common"}) {
    std::vector<uint8_t> fat_bytes, dat;
    std::vector<FatEntry> fat;
    if (!disc.Read(std::string(arc) + ".fat", fat_bytes, why) || !ReadFat(fat_bytes, fat, why) ||
        !disc.Read(std::string(arc) + ".dat", dat, why)) {
      return why = std::format("{}.fat/.dat: {}", arc, why), false;
    }
    std::vector<uint8_t> data;
    for (size_t i = 0; i < fat.size(); ++i) {
      const FatEntry& e = fat[i];
      if (uint64_t(e.offset) + e.stored > dat.size()) return why = std::format("{}.dat is too short", arc), false;
      if (!Unpack(dat.data() + e.offset, e, data)) {
        return why = std::format("unpacking entry {} of {}.dat failed", i, arc), false;
      }
      if (data.size() < 4) continue;
      const uint32_t magic = BigU32(data.data());
      if (magic != 0x102A0E00 && magic != 0x102A0E01) continue;
      ++containers;
      const std::string sha = fcp::hash::Sha1(data.data(), data.size());
      if (!seen.insert(sha).second) continue;
      ++unique;
      const std::string name = std::format("{}{:08X}.fxo", std::string(arc) == "common" ? "common__" : "", e.hash);
      std::ofstream(fxo / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                       std::streamsize(data.size()));
      if (fcp::progress::Cancelled()) return why = "stopped", false;
    }
    fcp::progress::SetStageFraction(std::string(arc) == "shaders" ? 0.1f : 0.2f);
  }
  const double read_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  FCPLOG("first start: {} shader containers ({} unique) from the disc image in {:.1f} s", containers, unique, read_s);
  fcp::progress::SetStage(fcp::progress::Stage::kShaders, L"Translating the shaders");
  fcp::progress::SetStageFraction(0.2f);
  const fs::path pack = PackPath(target);
  fs::create_directories(pack.parent_path(), ec);
  const fs::path tmp = pack.wstring() + L".tmp";
  fcp::build::Processes run(log, int(fcp::env::Int("FCP_GAME_CODE_CPUS", 0)));
  run.Note(std::format("---- shader pack {} ----", target));
  const std::wstring command = fcp::build::Quote(tool) + L" pack " + fcp::build::Quote(fxo) + L" --dxc " +
                               fcp::build::Quote(exe_dir) + L" --out " + fcp::build::Quote(tmp) + L" --target " +
                               fcp::build::Widen(target);
  void* process = run.Start(command, work);
  if (!process) return why = "fcshaders.exe could not be started", false;
  // fcshaders reports no progress; the bar moves on time (measured: about 16 s on 16 cores).
  const auto pack_start = std::chrono::steady_clock::now();
  while (WaitForSingleObject(process, 250) == WAIT_TIMEOUT) {
    if (fcp::progress::Cancelled()) {
      run.Cancel();
      return why = "stopped", false;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - pack_start).count();
    fcp::progress::SetStageFraction(0.2f + 0.75f * float(std::min(1.0, s / 30.0)));
  }
  if (const unsigned long code = fcp::build::Processes::ExitCode(process)) {
    return why = std::format("translating the shaders failed (fcshaders.exe exit code {})", code), false;
  }
  fs::rename(tmp, pack, ec);
  if (ec) return why = std::format("saving {} failed: {}", U8(pack), ec.message()), false;
  if (!fcp::env::Flag("FCP_GAME_CODE_KEEP")) fs::remove_all(work, ec);
  const double total_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  FCPLOG("first start: shader pack {} built in {:.1f} s", U8(pack), total_s);
  SetEnvironmentVariableW(L"FCP_SHADER_PACK", pack.wstring().c_str());
  return true;
}

}  // namespace fcp::first_start
