// Texture packs: the game's textures as files ([graphics] texture_dump) and image files drawn in their place
// ([graphics] texture_replace). Own code; the design follows Far Cry Instincts Recomp (MIT, same author,
// src/gpu/texture_pack.cpp), which adapted it from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa,
// src/gpu/texturedumper.cpp): names by a content hash, one worker thread, DDS reading, PNG through WIC, box filtered
// mipmaps, the watch on the replacement folder.
//
// Names. A texture is called by a 64-bit XXH3 hash (16 hex digits) of its level 0 guest blocks (after the endian swap,
// untiled, every face of a cube map) and its shape (guest format, width, height, dimension; for PNG textures also the
// fetch swizzle, see below). Neither the address, the tiling, the padding nor the other mip levels go into it, so the
// name is the same in every run, level and place in memory. The dump file is "<hash>_<w>x<h>_<format>.<ext>"; a
// replacement is found by the first 16-hex-digit part of its name ("<hash>.png", "<hash>_wood.dds" ...).
//
// Two kinds of texture take part (Classify):
//  - kBlocks: the host keeps the guest's BC blocks (DXT1/2/3/4/5, DXN, DXT5A as BC1/2/3/5/4). Dumped unchanged as DDS
//    with every mip level the game has, channels in the console's storage order; a replacement gets the game's swizzle
//    on top, like the original.
//  - kImage: 8-bit unsigned colour formats the host stores as texels (8888, 565, 1555, 4444, 8, 88, DXT of a size the
//    host cannot keep as BC ...). Dumped as an uncompressed PNG of level 0 as the shader samples it (fetch swizzle
//    applied, constants 0/1 included); a replacement is taken as it is (only the swizzle's constants stay).
// Left out on purpose: render and resolve targets (they never reach the texture cache as guest textures), textures the
// game rewrites again and again (video, GuestTex::volatile_data), 3D textures, stacked 2D arrays, depth, Y'CbCr video
// formats, signed, float, integer and 16-bit-per-channel formats, views that start below level 0.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "kk/resources/guest_texture.h"
#include "kk/resources/texture_convert.h"

namespace fcp::nr::pack {

enum class Kind : uint8_t { kNone = 0, kBlocks = 1, kImage = 2 };

// Whether a converted texture can be dumped / replaced, and how; `why` = reason for kNone (static text).
Kind Classify(const res::HostTexturePlan& plan, const char** why = nullptr);

// The name of a texture (0: none). KeyFromGuest reads the guest blocks of level 0 from guest memory; KeyFromHost
// takes them from the converted data where that is the guest blocks unchanged (Conversion::kCopy, level 0 stored),
// else returns 0. Key tries the host data first. Both give the same value for the same texture.
uint64_t KeyFromGuest(const res::TextureFetch& fetch, const res::GuestMemory& memory, const res::HostTexturePlan& plan, Kind kind);
uint64_t KeyFromHost(const res::TextureFetch& fetch, const res::HostTextureData& data, Kind kind);
uint64_t Key(const res::TextureFetch& fetch, const res::GuestMemory& memory, const res::HostTextureData& data, Kind kind);

// "<hash>_<w>x<h>_<format>[_cube].png|dds" and the line for dump\index.txt.
std::string DumpName(uint64_t key, const res::TextureFetch& fetch, const res::HostTexturePlan& plan, Kind kind);
std::string DumpInfo(uint64_t key, const res::TextureFetch& fetch, const res::HostTexturePlan& plan, Kind kind);
// The first part of 16 hex digits of a file name without extension (parts split at '_', '-', '.', ' '), 0: none.
uint64_t KeyInName(std::string_view stem);
// SRV swizzle of a replacement: kBlocks the game's view swizzle, kImage identity with the game's constants.
uint16_t ReplacementSwizzle(Kind kind, uint16_t view_swizzle);

// The data a dump writes: kBlocks the converted texture unchanged (every level, every face), kImage level 0 (every
// face) as sampled, RGBA8. False if nothing can be written.
bool DumpData(const res::HostTextureData& converted, Kind kind, res::HostTextureData& out);
// File contents of a dump: PNG (2D RGBA8, one level) or DDS (everything else).
bool EncodeDump(const res::HostTextureData& data, std::vector<uint8_t>& file, bool& png);

// Replacement files -> host data (RGBA8_UNORM, BC1-5 UNORM, BC7_UNORM; 2D or cube with 6 layers; level-major
// subresources). DDS: legacy FourCC DXT1-5, ATI1/ATI2/BC4U/BC5U, 32-bit RGBA/BGRA masks, or DX10 with those formats
// (sRGB variants read as UNORM). PNG: anything WIC reads, as RGBA8.
bool ReadDds(const std::vector<uint8_t>& file, res::HostTextureData& out, std::string* why);
bool ReadPng(const std::filesystem::path& path, res::HostTextureData& out, std::string* why);
// RGBA8 with one level: the full chain, box filtered (every layer). Anything else stays as it is.
void AddMipmaps(res::HostTextureData& t);
// ReadDds / ReadPng by extension, checks (sizes, BC multiple of 4) and AddMipmaps.
bool LoadReplacement(const std::filesystem::path& path, res::HostTextureData& out, std::string* why);

struct Options {
  bool dump = false, replace = false;
  std::filesystem::path dump_dir, replace_dir;
  void (*log)(const char* line) = nullptr;  // any thread
  size_t max_queued_dump_bytes = size_t(512) << 20;
};

// Folders, worker thread and the replacement files. Dump/WantsDump/FileGen/Find/Frame from the render thread (under
// the renderer lock), the worker writes dumps and loads replacements.
class TexturePack {
 public:
  explicit TexturePack(const Options& options);
  ~TexturePack();
  TexturePack(const TexturePack&) = delete;
  TexturePack& operator=(const TexturePack&) = delete;

  bool Dumping() const { return opt_.dump; }
  bool Replacing() const { return opt_.replace; }

  // Dumping is on, `key` was not dumped (in this or an earlier run: the dump folder) and has no replacement.
  bool WantsDump(uint64_t key);
  // Hands a dump to the worker (written as <dump_dir>/<name>, a line in index.txt). False: the worker's queue is full
  // (try again later) or the key is no longer wanted.
  bool Dump(uint64_t key, const std::string& name, const std::string& info, res::HostTextureData&& data);

  // Advances whenever the replacement folder was listed again and something changed: look the keys up again.
  uint32_t Revision() const { return revision_.load(std::memory_order_acquire); }
  // Generation of key's replacement file (changes when the file changes), 0: no file.
  uint32_t FileGen(uint64_t key);
  enum class Lookup { kNone, kLoading, kReady };
  // The loaded replacement of key at gen. kLoading: the first ask queues the load, ask again later. kReady: `out` is
  // the image; the pack forgets it (ask again for a new load). kNone: no file, or it did not load (logged).
  Lookup Find(uint64_t key, uint32_t gen, std::shared_ptr<const res::HostTextureData>& out);

  // Once per frame (render thread): notices changes in the replacement folder (~0.5 s after the last change).
  void Frame();
  std::string Summary();

 private:
  struct File {
    std::filesystem::path path;
    uint64_t size = 0;
    int64_t time = 0;
    uint32_t gen = 0;
  };
  struct Ready {
    uint32_t gen = 0;
    std::shared_ptr<const res::HostTextureData> data;  // null: did not load
  };
  void Log(const char* fmt, ...);
  void Post(std::function<void()> job, bool load);  // lock held
  void Work();
  std::unordered_map<uint64_t, File> ListReplacements();
  void Relist();  // worker
  void Load(uint64_t key, uint32_t gen, std::filesystem::path path);  // worker

  Options opt_;
  std::mutex lock_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> loads_, dumps_;
  bool stop_ = false;
  std::thread worker_;
  size_t queued_dump_bytes_ = 0;
  std::unordered_set<uint64_t> dumped_;
  std::unordered_map<uint64_t, File> files_;
  uint32_t next_gen_ = 1;
  std::unordered_map<uint64_t, uint32_t> asked_;  // key -> gen being loaded
  std::unordered_map<uint64_t, Ready> ready_;
  std::atomic<uint32_t> revision_{1};
  // render thread: the folder watch
  void* watch_ = nullptr;  // HANDLE, null: none
  uint32_t watch_tries_ = 0;
  bool changed_ = false;
  std::chrono::steady_clock::time_point changed_at_{};
  // statistics (lock_)
  uint64_t dumps_written_ = 0, dumps_failed_ = 0, loads_ok_ = 0, loads_failed_ = 0, relists_ = 0;
  double load_ms_ = 0, load_ms_max_ = 0, dump_ms_ = 0;
};

}  // namespace fcp::nr::pack
