// Texture packs (core/texture_pack.h): names, classification, dump data, DDS/PNG round trips, the worker and the
// folder. Synthetic textures only (no game data); the folders live under %TEMP% and are removed again.
#include <windows.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "core/texture_pack.h"
#include "kk/backend/tests/check.h"
#include "kk/resources/guest_texture.h"
#include "kk/resources/texture_convert.h"
#include "kk/resources/tiling.h"

using namespace fcp;
using namespace fcp::nr;
namespace fs = std::filesystem;
using res::Dimension;
using res::TextureFormat;

namespace {

// Guest memory with a texture placed at its fetch constant's addresses.
struct Guest {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(32u << 20, 0xCD);  // padding and the rest: not zero
  res::GuestMemory Memory() const {
    res::GuestMemory m;
    m.base = bytes.data();
    m.size = bytes.size();
    return m;
  }
};

uint32_t Rand(uint32_t& s) { return s = s * 1664525u + 1013904223u; }

// Puts a texture with pseudo-random blocks (seed) into g; returns its fetch constant.
res::TextureFetch Place(Guest& g, res::TextureFetchDesc d, uint32_t seed) {
  const res::TextureFetch f = res::MakeTextureFetch(d);
  const res::GuestLayout layout = res::ComputeGuestLayout(f);
  const uint32_t bpb = res::GetFormatInfo(d.format).BytesPerBlock();
  std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> data;
  for (uint32_t level = 0; level <= layout.max_level; ++level)
    for (uint32_t layer = 0; layer < layout.layers; ++layer) {
      std::vector<uint8_t>& v = data[{level, layer}];
      v.resize(res::GetLevelBlockExtent(layout, level).Count() * bpb);
      for (uint8_t& b : v) b = uint8_t(Rand(seed) >> 24);
    }
  res::GuestTextureImage image;
  CHECK(res::EncodeGuestTexture(f, [&](uint32_t level, uint32_t layer) -> const uint8_t* {
    auto it = data.find({level, layer});
    return it == data.end() ? nullptr : it->second.data();
  }, image));
  if (d.base_address) std::memcpy(g.bytes.data() + d.base_address, image.base.data(), image.base.size());
  if (d.mip_address) std::memcpy(g.bytes.data() + d.mip_address, image.mips.data(), image.mips.size());
  return f;
}

res::TextureFetchDesc Desc(TextureFormat format, uint32_t w, uint32_t h, bool tiled, uint32_t base, uint32_t mips = 0,
                           uint32_t max_level = 0) {
  res::TextureFetchDesc d;
  d.format = format;
  d.endian = format == TextureFormat::k_8_8_8_8 ? res::Endian::k8in32 : res::Endian::k8in16;
  d.width = w, d.height = h, d.tiled = tiled;
  d.base_address = base, d.mip_address = mips, d.max_level = max_level;
  d.packed_mips = true;
  return d;
}

res::HostTextureData Convert(const Guest& g, const res::TextureFetch& f) {
  res::HostTextureData data;
  std::string why;
  CHECK(res::ConvertTexture(f, g.Memory(), data, &why));
  return data;
}

fs::path TempDir(const char* name) {
  fs::path p = fs::temp_directory_path() / (std::string("fcp_texpack_") + name + "_" + std::to_string(GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

void WriteBytes(const fs::path& p, const std::vector<uint8_t>& b) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

res::HostTextureData Rgba(uint32_t w, uint32_t h, uint32_t seed) {
  res::HostTextureData t;
  t.plan.format = res::HostFormat::RGBA8_UNORM;
  t.plan.width = w, t.plan.height = h;
  res::HostSubresource s;
  s.width = w, s.height = h, s.row_pitch = w * 4, s.depth_pitch = s.size = size_t(w) * h * 4;
  t.subresources.push_back(s);
  t.bytes.resize(s.size);
  for (uint8_t& b : t.bytes) b = uint8_t(Rand(seed) >> 24);
  return t;
}

}  // namespace

TEST(TexturePack_KeyStableAcrossAddressTilingAndPadding) {
  Guest a, b;
  // the same texels: tiled at one address, linear at another; the bytes around differ
  std::fill(b.bytes.begin(), b.bytes.end(), uint8_t(0x11));
  const res::TextureFetch fa = Place(a, Desc(TextureFormat::k_DXT1, 256, 128, true, 0x00100000, 0x00400000, 8), 7);
  const res::TextureFetch fb = Place(b, Desc(TextureFormat::k_DXT1, 256, 128, false, 0x00900000, 0x00C00000, 8), 7);
  const res::HostTextureData da = Convert(a, fa), db = Convert(b, fb);
  CHECK(pack::Classify(da.plan) == pack::Kind::kBlocks);
  const uint64_t ka = pack::KeyFromGuest(fa, a.Memory(), da.plan, pack::Kind::kBlocks);
  const uint64_t kb = pack::KeyFromGuest(fb, b.Memory(), db.plan, pack::Kind::kBlocks);
  CHECK(ka != 0);
  CHECK_EQ(ka, kb);
  // other texels, other mips only, other size: other names
  Guest c;
  const res::TextureFetch fc = Place(c, Desc(TextureFormat::k_DXT1, 256, 128, true, 0x00100000, 0x00400000, 8), 8);
  CHECK(pack::KeyFromGuest(fc, c.Memory(), Convert(c, fc).plan, pack::Kind::kBlocks) != ka);
  // only level 0 counts: overwrite the mips of a
  std::fill(a.bytes.begin() + 0x00400000, a.bytes.begin() + 0x00500000, uint8_t(0x77));
  CHECK_EQ(pack::Key(fa, a.Memory(), Convert(a, fa), pack::Kind::kBlocks), ka);
}

TEST(TexturePack_HostKeyEqualsGuestKey) {
  struct Case {
    TextureFormat format;
    uint32_t w, h;
    bool tiled, cube;
  };
  const Case cases[] = {{TextureFormat::k_DXT1, 128, 64, true, false},   {TextureFormat::k_DXT4_5, 64, 64, false, false},
                        {TextureFormat::k_8_8_8_8, 96, 40, true, false}, {TextureFormat::k_8, 64, 32, true, false},
                        {TextureFormat::k_DXT1, 64, 64, true, true},     {TextureFormat::k_8_8_8_8, 32, 32, true, true}};
  uint32_t seed = 100;
  for (const Case& k : cases) {
    Guest g;
    res::TextureFetchDesc d = Desc(k.format, k.w, k.h, k.tiled, 0x00100000, 0x00800000, 3);
    if (k.cube) d.dimension = Dimension::kCube, d.depth_or_layers = 6;
    const res::TextureFetch f = Place(g, d, ++seed);
    const res::HostTextureData data = Convert(g, f);
    const pack::Kind kind = pack::Classify(data.plan);
    CHECK(kind != pack::Kind::kNone);
    const uint64_t host = pack::KeyFromHost(f, data, kind), guest = pack::KeyFromGuest(f, g.Memory(), data.plan, kind);
    CHECK(host != 0);
    CHECK_EQ(host, guest);
  }
  // a converted (not copied) format has no host key, the guest key still works
  Guest g;
  const res::TextureFetch f = Place(g, Desc(TextureFormat::k_5_6_5, 64, 64, true, 0x00100000), 5);
  const res::HostTextureData data = Convert(g, f);
  const pack::Kind kind = pack::Classify(data.plan);
  CHECK(kind == pack::Kind::kImage);
  if (data.plan.conversion != res::Conversion::kCopy) CHECK_EQ(pack::KeyFromHost(f, data, kind), uint64_t(0));
  CHECK(pack::Key(f, g.Memory(), data, kind) != 0);
}

TEST(TexturePack_Classify) {
  Guest g;
  auto kind = [&](res::TextureFetchDesc d) {
    const res::TextureFetch f = Place(g, d, 3);
    res::HostTextureData data;
    if (!res::ConvertTexture(f, g.Memory(), data)) return pack::Kind::kNone;
    return pack::Classify(data.plan);
  };
  CHECK(kind(Desc(TextureFormat::k_DXT1, 64, 64, true, 0x00100000)) == pack::Kind::kBlocks);
  CHECK(kind(Desc(TextureFormat::k_DXN, 64, 64, true, 0x00100000)) == pack::Kind::kBlocks);
  CHECK(kind(Desc(TextureFormat::k_8_8_8_8, 64, 64, true, 0x00100000)) == pack::Kind::kImage);
  CHECK(kind(Desc(TextureFormat::k_DXT1, 30, 30, true, 0x00100000)) == pack::Kind::kImage);  // decoded: unaligned
  CHECK(kind(Desc(TextureFormat::k_16_16_16_16, 64, 64, true, 0x00100000)) == pack::Kind::kNone);
  CHECK(kind(Desc(TextureFormat::k_16_16_FLOAT, 64, 64, true, 0x00100000)) == pack::Kind::kNone);
  CHECK(kind(Desc(TextureFormat::k_Cr_Y1_Cb_Y0_REP, 64, 64, false, 0x00100000)) == pack::Kind::kNone);
  res::TextureFetchDesc vol = Desc(TextureFormat::k_DXT1, 32, 32, true, 0x00100000);
  vol.dimension = Dimension::k3D, vol.depth_or_layers = 4;
  CHECK(kind(vol) == pack::Kind::kNone);
  res::TextureFetchDesc arr = Desc(TextureFormat::k_DXT1, 32, 32, true, 0x00100000);
  arr.stacked = true, arr.depth_or_layers = 3;
  CHECK(kind(arr) == pack::Kind::kNone);
  res::TextureFetchDesc sgn = Desc(TextureFormat::k_8_8_8_8, 32, 32, true, 0x00100000);
  for (auto& s : sgn.signs) s = res::TextureSign::kSigned;
  CHECK(kind(sgn) == pack::Kind::kNone);
}

TEST(TexturePack_NamesAndSwizzle) {
  CHECK_EQ(pack::KeyInName("0123456789abcdef_256x256_dxt1"), uint64_t(0x0123456789abcdefull));
  CHECK_EQ(pack::KeyInName("wood-0123456789ABCDEF"), uint64_t(0x0123456789abcdefull));
  CHECK_EQ(pack::KeyInName("0123456789abcdef"), uint64_t(0x0123456789abcdefull));
  CHECK_EQ(pack::KeyInName("0123456789abcde"), uint64_t(0));
  CHECK_EQ(pack::KeyInName("x0123456789abcdef"), uint64_t(0));
  Guest g;
  const res::TextureFetch f = Place(g, Desc(TextureFormat::k_DXT1, 256, 128, true, 0x00100000), 1);
  const res::HostTextureData data = Convert(g, f);
  CHECK_EQ(pack::DumpName(0xabcull, f, data.plan, pack::Kind::kBlocks), std::string("0000000000000abc_256x128_dxt1.dds"));
  const uint16_t zyx1 = res::MakeSwizzle(res::kSwzZ, res::kSwzY, res::kSwzX, res::kSwz1);
  CHECK_EQ(pack::ReplacementSwizzle(pack::Kind::kBlocks, zyx1), zyx1);
  CHECK_EQ(pack::ReplacementSwizzle(pack::Kind::kImage, zyx1), res::MakeSwizzle(res::kSwzX, res::kSwzY, res::kSwzZ, res::kSwz1));
}

TEST(TexturePack_ImageDumpIsAsSampled) {
  // A8R8G8B8-like: X = B in memory order, the swizzle shows Z as red; the PNG has the shown order.
  Guest g;
  res::TextureFetchDesc d = Desc(TextureFormat::k_8_8_8_8, 16, 8, false, 0x00100000);
  d.swizzle = res::MakeSwizzle(res::kSwzZ, res::kSwzY, res::kSwzX, res::kSwzW);
  const res::TextureFetch f = Place(g, d, 9);
  const res::HostTextureData data = Convert(g, f);
  CHECK(pack::Classify(data.plan) == pack::Kind::kImage);
  res::HostTextureData file;
  CHECK(pack::DumpData(data, pack::Kind::kImage, file));
  CHECK(file.plan.format == res::HostFormat::RGBA8_UNORM);
  const res::HostSubresource* s = data.Find(0, 0);
  const uint8_t* in = data.bytes.data() + s->offset;
  const uint8_t* out = file.bytes.data() + file.subresources[0].offset;
  CHECK_EQ(out[0], in[2]);
  CHECK_EQ(out[1], in[1]);
  CHECK_EQ(out[2], in[0]);
  CHECK_EQ(out[3], in[3]);
  std::vector<uint8_t> bytes;
  bool png = false;
  CHECK(pack::EncodeDump(file, bytes, png));
  CHECK(png);
}

TEST(TexturePack_DdsRoundTrip) {
  // BC1 with its mips, and a cube map: the dump's DDS reads back with the same blocks
  for (const bool cube : {false, true}) {
    Guest g;
    res::TextureFetchDesc d = Desc(TextureFormat::k_DXT1, 64, 64, true, 0x00100000, 0x00800000, 6);
    if (cube) d.dimension = Dimension::kCube, d.depth_or_layers = 6;
    const res::TextureFetch f = Place(g, d, 21);
    const res::HostTextureData data = Convert(g, f);
    res::HostTextureData file, back;
    CHECK(pack::DumpData(data, pack::Kind::kBlocks, file));
    std::vector<uint8_t> bytes;
    bool png = true;
    CHECK(pack::EncodeDump(file, bytes, png));
    CHECK(!png);
    std::string why;
    CHECK(pack::ReadDds(bytes, back, &why));
    CHECK(back.plan.format == res::HostFormat::BC1_UNORM);
    CHECK_EQ(back.plan.levels, data.plan.levels);
    CHECK_EQ(back.plan.layers, cube ? 6u : 1u);
    CHECK(back.plan.dimension == (cube ? Dimension::kCube : Dimension::k2D));
    for (const res::HostSubresource& s : data.subresources) {
      const res::HostSubresource* b = back.Find(s.level, s.layer);
      CHECK(b && b->size == s.size);
      if (b && b->size == s.size) CHECK(std::memcmp(back.bytes.data() + b->offset, data.bytes.data() + s.offset, s.size) == 0);
    }
  }
}

TEST(TexturePack_DdsBc7AndBgra) {
  // BC7 (DX10 header, 8x8 with 2 levels) reads back as BC7 with its levels
  res::HostTextureData bc7;
  bc7.plan.format = res::HostFormat::BC7_UNORM;
  bc7.plan.width = 8, bc7.plan.height = 8, bc7.plan.levels = 2;
  uint32_t seed = 77;
  for (uint32_t level = 0; level < 2; ++level) {
    res::HostSubresource s;
    s.level = level;
    s.width = s.height = 8 >> level;
    s.row_pitch = (level ? 1 : 2) * 16;
    s.depth_pitch = s.size = s.row_pitch * (level ? 1 : 2);
    s.offset = bc7.bytes.size();
    bc7.bytes.resize(s.offset + s.size);
    for (size_t i = s.offset; i < bc7.bytes.size(); ++i) bc7.bytes[i] = uint8_t(Rand(seed) >> 24);
    bc7.subresources.push_back(s);
  }
  std::vector<uint8_t> bytes;
  bool png = true;
  CHECK(pack::EncodeDump(bc7, bytes, png));
  res::HostTextureData back;
  std::string why;
  CHECK(pack::ReadDds(bytes, back, &why));
  CHECK(back.plan.format == res::HostFormat::BC7_UNORM);
  CHECK_EQ(back.plan.levels, 2u);
  CHECK(back.bytes == bc7.bytes);
  // legacy BGRA8 without alpha: channels swapped to RGBA, alpha 255
  std::vector<uint8_t> f(128 + 4, 0);
  auto put = [&](size_t at, uint32_t v) { std::memcpy(&f[at], &v, 4); };
  put(0, 0x20534444), put(4, 124), put(8, 0x1007), put(12, 1), put(16, 1), put(76, 32), put(80, 0x40), put(88, 32);
  put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF);
  f[128] = 10, f[129] = 20, f[130] = 30, f[131] = 0;  // B G R X
  CHECK(pack::ReadDds(f, back, &why));
  CHECK(back.plan.format == res::HostFormat::RGBA8_UNORM);
  CHECK(back.bytes.size() == 4 && back.bytes[0] == 30 && back.bytes[1] == 20 && back.bytes[2] == 10 && back.bytes[3] == 255);
}

TEST(TexturePack_PngRoundTripAndMipmaps) {
  const fs::path dir = TempDir("png");
  const res::HostTextureData img = Rgba(20, 12, 4);
  std::vector<uint8_t> bytes;
  bool png = false;
  CHECK(pack::EncodeDump(img, bytes, png));
  CHECK(png);
  WriteBytes(dir / "0123456789abcdef.png", bytes);
  res::HostTextureData back;
  std::string why;
  CHECK(pack::ReadPng(dir / "0123456789abcdef.png", back, &why));
  CHECK_EQ(back.plan.width, 20u);
  CHECK_EQ(back.plan.height, 12u);
  CHECK(back.bytes.size() == img.bytes.size() && std::memcmp(back.bytes.data(), img.bytes.data(), img.bytes.size()) == 0);
  // the chain: 20x12 -> 10x6 -> 5x3 -> 2x1 -> 1x1, box filtered
  CHECK(pack::LoadReplacement(dir / "0123456789abcdef.png", back, &why));
  CHECK_EQ(back.plan.levels, 5u);
  CHECK_EQ(back.subresources.size(), size_t(5));
  const res::HostSubresource* l1 = back.Find(1, 0);
  CHECK(l1 && l1->width == 10 && l1->height == 6);
  if (l1) {
    const uint8_t* s = img.bytes.data();
    const uint32_t want = (s[0] + s[4] + s[80] + s[84] + 2) / 4;  // texels (0,0), (1,0), (0,1), (1,1), red
    CHECK_EQ(uint32_t(back.bytes[l1->offset]), want);
  }
  // BC whose size is no multiple of 4 is refused
  res::HostTextureData bc;
  bc.plan.format = res::HostFormat::BC1_UNORM;
  bc.plan.width = 6, bc.plan.height = 6;
  res::HostSubresource s;
  s.width = 6, s.height = 6, s.row_pitch = 16, s.depth_pitch = s.size = 32;
  bc.subresources.push_back(s);
  bc.bytes.resize(32);
  CHECK(pack::EncodeDump(bc, bytes, png));
  WriteBytes(dir / "1111111111111111.dds", bytes);
  CHECK(!pack::LoadReplacement(dir / "1111111111111111.dds", back, &why));
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(TexturePack_FolderWorkerAndLiveChange) {
  const fs::path root = TempDir("pack");
  const uint64_t key = 0x00112233445566ffull, other = 0x0011223344556677ull;
  fs::create_directories(root / "replace" / "sub");
  {
    std::vector<uint8_t> bytes;
    bool png = false;
    CHECK(pack::EncodeDump(Rgba(8, 8, 1), bytes, png));
    WriteBytes(root / "replace" / "sub" / "00112233445566ff_crate.png", bytes);
  }
  pack::Options o;
  o.dump = o.replace = true;
  o.dump_dir = root / "dump";
  o.replace_dir = root / "replace";
  {
    pack::TexturePack p(o);
    const uint32_t gen = p.FileGen(key);
    CHECK(gen != 0);
    CHECK_EQ(p.FileGen(other), 0u);
    CHECK(!p.WantsDump(key));  // has a replacement
    CHECK(p.WantsDump(other));
    std::shared_ptr<const res::HostTextureData> img;
    CHECK(p.Find(key, gen, img) == pack::TexturePack::Lookup::kLoading);
    auto lookup = pack::TexturePack::Lookup::kLoading;
    for (int i = 0; i < 400 && lookup == pack::TexturePack::Lookup::kLoading; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      lookup = p.Find(key, gen, img);
    }
    CHECK(lookup == pack::TexturePack::Lookup::kReady);
    CHECK(img && img->plan.width == 8 && img->plan.levels == 4);
    // a dump
    CHECK(p.Dump(other, "0011223344556677_8x8_8888.png", "info\n", Rgba(8, 8, 2)));
    CHECK(!p.WantsDump(other));
    CHECK(!p.Dump(other, "0011223344556677_8x8_8888.png", "info\n", Rgba(8, 8, 2)));
    // live: the replacement changes on disk
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (int i = 0; i < 3; ++i) p.Frame();  // the watch is made on the first call
    const uint32_t rev = p.Revision();
    {
      std::vector<uint8_t> bytes;
      bool png = false;
      CHECK(pack::EncodeDump(Rgba(16, 16, 3), bytes, png));
      WriteBytes(root / "replace" / "sub" / "00112233445566ff_crate.png", bytes);
    }
    for (int i = 0; i < 300 && p.Revision() == rev; ++i) {
      p.Frame();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(p.Revision() != rev);
    const uint32_t gen2 = p.FileGen(key);
    CHECK(gen2 != 0 && gen2 != gen);
    CHECK(p.Find(key, gen, img) == pack::TexturePack::Lookup::kNone);  // the old generation is gone
  }  // the destructor writes the dump still queued
  CHECK(fs::exists(root / "dump" / "0011223344556677_8x8_8888.png"));
  CHECK(fs::exists(root / "dump" / "index.txt"));
  {
    pack::Options o2 = o;
    o2.replace = false;
    pack::TexturePack p(o2);
    CHECK(!p.WantsDump(other));  // dumped in an earlier run
  }
  std::error_code ec;
  fs::remove_all(root, ec);
}
