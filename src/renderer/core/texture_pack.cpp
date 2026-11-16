// Texture packs, see texture_pack.h. Own code; the design follows Far Cry Instincts Recomp (MIT, same author),
// src/gpu/texture_pack.cpp (adapted there from SCDA-XBERecomp, MIT, Copyright (c) 2026 TGP482, Joshhhuaaa): name
// parts in the file name, the worker with loads before dumps, .part files, index.txt, DDS header offsets, PNG through
// WIC, the 2x2 box filter and the change notification with a settle time.
#include "core/texture_pack.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>

#include <xxh3.h>

#include "kk/resources/image_io.h"

namespace fcp::nr::pack {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using H = res::HostFormat;

constexpr uint32_t kMaxSide = 16384;
constexpr auto kSettle = std::chrono::milliseconds(500);  // files still being copied in
constexpr DWORD kChanges = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                           FILE_NOTIFY_CHANGE_SIZE;

uint32_t DivUp(uint32_t v, uint32_t d) { return (v + d - 1) / d; }

std::string Utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

bool IsBc(H f) {
  return f == H::BC1_UNORM || f == H::BC2_UNORM || f == H::BC3_UNORM || f == H::BC4_UNORM || f == H::BC5_UNORM ||
         f == H::BC7_UNORM;
}

// Size of one subresource of host format f.
void Shape(H f, uint32_t w, uint32_t h, uint32_t& row_pitch, uint32_t& rows) {
  const res::HostFormatInfo& info = res::GetHostFormatInfo(f);
  row_pitch = DivUp(w, info.block_size) * info.bytes_per_block;
  rows = DivUp(h, info.block_size);
}

uint32_t FullChain(uint32_t w, uint32_t h) {
  uint32_t n = 1;
  while ((std::max(w, h) >> n) > 0) ++n;
  return n;
}

// Appends one subresource (level-major order is the caller's job).
res::HostSubresource& AddSub(res::HostTextureData& t, uint32_t level, uint32_t layer, uint32_t w, uint32_t h) {
  res::HostSubresource s;
  s.level = level;
  s.layer = layer;
  s.width = w;
  s.height = h;
  s.depth = 1;
  uint32_t rows = 0;
  Shape(t.plan.format, w, h, s.row_pitch, rows);
  s.depth_pitch = s.row_pitch * rows;
  s.size = s.depth_pitch;
  s.offset = t.bytes.size();
  t.bytes.resize(s.offset + s.size, 0);
  t.subresources.push_back(s);
  return t.subresources.back();
}

// Hash chain over the shape, then the blocks of each face.
uint64_t ShapeHash(const res::TextureFetch& f, const res::HostTexturePlan& p, Kind kind) {
  const uint32_t shape[7] = {0x54504346u /* "FCPT" */, 1u /* version */, uint32_t(f.Format()), p.width, p.height,
                             uint32_t(p.dimension), kind == Kind::kImage ? uint32_t(p.view_swizzle) : 0u};
  return XXH3_64bits(shape, sizeof shape);
}

const char* FormatToken(res::TextureFormat f) {
  switch (f) {
    case res::TextureFormat::k_8: return "8";
    case res::TextureFormat::k_1_5_5_5: return "1555";
    case res::TextureFormat::k_5_6_5: return "565";
    case res::TextureFormat::k_6_5_5: return "655";
    case res::TextureFormat::k_8_8_8_8: return "8888";
    case res::TextureFormat::k_8_8_8_8_A: return "8888a";
    case res::TextureFormat::k_8_A: return "8a";
    case res::TextureFormat::k_8_B: return "8b";
    case res::TextureFormat::k_8_8: return "88";
    case res::TextureFormat::k_4_4_4_4: return "4444";
    case res::TextureFormat::k_DXT1: return "dxt1";
    case res::TextureFormat::k_DXT2_3: return "dxt3";
    case res::TextureFormat::k_DXT4_5: return "dxt5";
    case res::TextureFormat::k_DXN: return "dxn";
    case res::TextureFormat::k_DXT3A: return "dxt3a";
    case res::TextureFormat::k_DXT5A: return "dxt5a";
    case res::TextureFormat::k_CTX1: return "ctx1";
    case res::TextureFormat::k_DXT1_AS_16_16_16_16: return "dxt1x";
    case res::TextureFormat::k_DXT2_3_AS_16_16_16_16: return "dxt3x";
    case res::TextureFormat::k_DXT4_5_AS_16_16_16_16: return "dxt5x";
    case res::TextureFormat::k_8_8_8_8_AS_16_16_16_16: return "8888x";
    case res::TextureFormat::k_DXT3A_AS_1_1_1_1: return "dxt3a1111";
    case res::TextureFormat::k_1: return "1";
    case res::TextureFormat::k_1_REVERSE: return "1r";
    default: return "other";
  }
}

std::string SwizzleText(uint16_t s) {
  std::string out;
  for (int i = 0; i < 4; ++i) out += "xyzw01??"[res::SwizzleComponent(s, i)];
  return out;
}

}  // namespace

// ---- classification and names --------------------------------------------------------------------------------

Kind Classify(const res::HostTexturePlan& p, const char** why) {
  auto no = [&](const char* w) {
    if (why) *why = w;
    return Kind::kNone;
  };
  if (p.dimension == res::Dimension::k3D) return no("volume texture");
  if (p.dimension == res::Dimension::k1D) return no("1D texture");
  if (p.dimension == res::Dimension::k2D && p.layers != 1) return no("2D array");
  if (p.dimension == res::Dimension::kCube && p.layers != 6) return no("cube map without 6 faces");
  if (p.min_level != 0) return no("level 0 not stored");
  if (!p.width || !p.height || p.width > kMaxSide || p.height > kMaxSide) return no("size");
  if (p.shader_flags & (res::kShaderMixedSigns | res::kShaderInteger | res::kShaderNorm32)) return no("signed or integer data");
  switch (p.conversion) {
    case res::Conversion::kCrY1CbY0ToRGBA8:
    case res::Conversion::kY1CrY0CbToRGBA8: return no("video format");
    case res::Conversion::kDepth24ToFloat:
    case res::Conversion::kDepth20e4ToFloat: return no("depth format");
    case res::Conversion::kFixed16ToHalf:
    case res::Conversion::k7e3ToRGBA16F:
    case res::Conversion::kSplitBlockToTexels: return no("render target format");
    default: break;
  }
  if (IsBc(p.format)) return p.decompressed ? no("decoded BC") : Kind::kBlocks;
  switch (p.format) {
    case H::RGBA8_UNORM:
    case H::BGRA4_UNORM:
    case H::B5G6R5_UNORM:
    case H::B5G5R5A1_UNORM:
    case H::R8_UNORM:
    case H::RG8_UNORM: return Kind::kImage;
    default: return no("not 8-bit unsigned");
  }
}

uint64_t KeyFromGuest(const res::TextureFetch& fetch, const res::GuestMemory& memory, const res::HostTexturePlan& plan,
                      Kind kind) {
  if (kind == Kind::kNone) return 0;
  const res::TextureRanges ranges = res::GetTextureRanges(fetch);
  if (!ranges.base_bytes) return 0;
  std::vector<uint8_t> base;
  if (!res::LoadSwappedRegion(memory, ranges.base, ranges.base_bytes, fetch.EndianMode(), base)) return 0;
  const res::GuestLayout layout = res::ComputeGuestLayout(fetch);
  res::GuestRegions regions;
  regions.base = base.data();
  regions.base_size = base.size();
  uint64_t h = ShapeHash(fetch, plan, kind);
  std::vector<uint8_t> blocks;
  for (uint32_t layer = 0; layer < plan.layers; ++layer) {
    res::BlockExtent extent;
    if (!res::ReadGuestBlocks(layout, regions, 0, layer, blocks, extent)) return 0;
    h = XXH3_64bits_withSeed(blocks.data(), blocks.size(), h);
  }
  return h ? h : 1;
}

uint64_t KeyFromHost(const res::TextureFetch& fetch, const res::HostTextureData& data, Kind kind) {
  const res::HostTexturePlan& p = data.plan;
  if (kind == Kind::kNone || p.conversion != res::Conversion::kCopy || p.decompressed || p.min_level != 0) return 0;
  // kCopy: the same block size and bytes per block as the guest's
  const res::FormatInfo& g = res::GetFormatInfo(fetch.Format());
  const res::HostFormatInfo& hi = res::GetHostFormatInfo(p.format);
  if (g.block_width != hi.block_size || g.block_height != hi.block_size || g.BytesPerBlock() != hi.bytes_per_block) return 0;
  uint64_t h = ShapeHash(fetch, p, kind);
  for (uint32_t layer = 0; layer < p.layers; ++layer) {
    const res::HostSubresource* s = data.Find(0, layer);
    if (!s || s->depth != 1) return 0;
    h = XXH3_64bits_withSeed(data.bytes.data() + s->offset, s->size, h);
  }
  return h ? h : 1;
}

uint64_t Key(const res::TextureFetch& fetch, const res::GuestMemory& memory, const res::HostTextureData& data, Kind kind) {
  if (const uint64_t k = KeyFromHost(fetch, data, kind)) return k;
  return KeyFromGuest(fetch, memory, data.plan, kind);
}

std::string DumpName(uint64_t key, const res::TextureFetch& fetch, const res::HostTexturePlan& plan, Kind kind) {
  const bool cube = plan.dimension == res::Dimension::kCube;
  const bool png = kind == Kind::kImage && !cube;
  char name[96];
  std::snprintf(name, sizeof name, "%016llx_%ux%u_%s%s.%s", (unsigned long long)key, plan.width, plan.height,
                FormatToken(fetch.Format()), cube ? "_cube" : "", png ? "png" : "dds");
  return name;
}

std::string DumpInfo(uint64_t key, const res::TextureFetch& fetch, const res::HostTexturePlan& plan, Kind kind) {
  char info[256];
  std::snprintf(info, sizeof info, "%016llx  %ux%u  %s  guest %s  host %s  %u level%s  swizzle %s  %s\n",
                (unsigned long long)key, plan.width, plan.height, plan.dimension == res::Dimension::kCube ? "cube" : "2D",
                res::GetFormatInfo(fetch.Format()).name, res::GetHostFormatInfo(plan.format).name, plan.levels,
                plan.levels == 1 ? "" : "s", SwizzleText(plan.view_swizzle).c_str(),
                kind == Kind::kBlocks ? "blocks unchanged (storage order)" : "as sampled (swizzle applied)");
  return info;
}

uint64_t KeyInName(std::string_view stem) {
  size_t start = 0;
  while (start <= stem.size()) {
    const size_t end = std::min(stem.find_first_of("_-. ", start), stem.size());
    const std::string_view part = stem.substr(start, end - start);
    if (part.size() == 16 && part.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos)
      return std::strtoull(std::string(part).c_str(), nullptr, 16);
    start = end + 1;
  }
  return 0;
}

uint16_t ReplacementSwizzle(Kind kind, uint16_t view_swizzle) {
  if (kind != Kind::kImage) return view_swizzle;
  uint8_t c[4];
  for (int i = 0; i < 4; ++i) {
    const uint8_t s = res::SwizzleComponent(view_swizzle, i);
    c[i] = s >= 4 ? s : uint8_t(i);
  }
  return res::MakeSwizzle(c[0], c[1], c[2], c[3]);
}

// ---- dump data -------------------------------------------------------------------------------------------------

bool DumpData(const res::HostTextureData& converted, Kind kind, res::HostTextureData& out) {
  out = {};
  const res::HostTexturePlan& p = converted.plan;
  if (kind == Kind::kBlocks) {
    out = converted;
    return !out.subresources.empty();
  }
  if (kind != Kind::kImage) return false;
  out.plan = p;
  out.plan.format = H::RGBA8_UNORM;
  out.plan.conversion = res::Conversion::kCopy;
  out.plan.levels = 1;
  out.plan.view_swizzle = res::kSwizzleXYZW;
  out.plan.decompressed = false;
  for (uint32_t layer = 0; layer < p.layers; ++layer) {
    const res::HostSubresource* s = converted.Find(0, layer);
    if (!s) return false;
    const std::vector<uint8_t> rgba = fcp_res_tools::HostToRgba8(converted, *s, 0);
    res::HostSubresource& d = AddSub(out, 0, layer, s->width, s->height);
    std::memcpy(out.bytes.data() + d.offset, rgba.data(), std::min(rgba.size(), d.size));
  }
  return true;
}

bool EncodeDump(const res::HostTextureData& data, std::vector<uint8_t>& file, bool& png) {
  const res::HostTexturePlan& p = data.plan;
  png = p.format == H::RGBA8_UNORM && p.dimension == res::Dimension::k2D && p.layers == 1 && p.levels == 1;
  if (png) {
    const res::HostSubresource* s = data.Find(0, 0);
    if (!s) return false;
    std::vector<uint8_t> rgba(size_t(s->width) * s->height * 4);
    for (uint32_t y = 0; y < s->height; ++y)
      std::memcpy(rgba.data() + size_t(y) * s->width * 4, data.bytes.data() + s->offset + size_t(y) * s->row_pitch,
                  size_t(s->width) * 4);
    return fcp_res_tools::EncodePng(s->width, s->height, rgba, file);
  }
  return fcp_res_tools::EncodeDds(data, file);
}

// ---- reading replacement files -----------------------------------------------------------------------------------

namespace {

constexpr uint32_t FourCC(const char (&s)[5]) {
  return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24;
}
// "DDS " then DDS_HEADER (byte offsets from the start of the file); DDS_HEADER_DXT10 follows for FourCC DX10
constexpr size_t kDdsFlags = 8, kDdsHeight = 12, kDdsWidth = 16, kDdsDepth = 24, kDdsMipCount = 28, kDdsPixelFlags = 80,
                 kDdsFourCC = 84, kDdsBitCount = 88, kDdsRedMask = 92, kDdsGreenMask = 96, kDdsBlueMask = 100,
                 kDdsAlphaMask = 104, kDdsCaps2 = 112, kDdsHeaderBytes = 128, kDx10Format = 128, kDx10Dimension = 132,
                 kDx10Misc = 136, kDx10ArraySize = 140, kDx10HeaderBytes = 148;
constexpr uint32_t kDdpfAlphaPixels = 0x1, kDdpfFourCC = 0x4, kDdpfRgb = 0x40;
constexpr uint32_t kCaps2Cube = 0x200, kCaps2AllFaces = 0xFC00, kCaps2Volume = 0x200000;

uint32_t Read32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v;
  std::memcpy(&v, &b[at], 4);
  return v;
}

H FromDxgi(uint32_t f, bool& bgra) {
  bgra = false;
  switch (f) {
    case 28: case 29: return H::RGBA8_UNORM;                // R8G8B8A8_UNORM(_SRGB)
    case 87: case 91: bgra = true; return H::RGBA8_UNORM;   // B8G8R8A8_UNORM(_SRGB)
    case 71: case 72: return H::BC1_UNORM;
    case 74: case 75: return H::BC2_UNORM;
    case 77: case 78: return H::BC3_UNORM;
    case 80: return H::BC4_UNORM;
    case 83: return H::BC5_UNORM;
    case 98: case 99: return H::BC7_UNORM;
    default: return H::UNKNOWN;
  }
}

}  // namespace

bool ReadDds(const std::vector<uint8_t>& f, res::HostTextureData& out, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  out = {};
  if (f.size() < kDdsHeaderBytes || Read32(f, 0) != FourCC("DDS ")) return fail("not a DDS file");
  size_t at = kDdsHeaderBytes;
  const uint32_t pf = Read32(f, kDdsPixelFlags), cc = Read32(f, kDdsFourCC), caps2 = Read32(f, kDdsCaps2);
  if (caps2 & kCaps2Volume) return fail("volume DDS");
  uint32_t faces = (caps2 & kCaps2Cube) ? 6 : 1;
  if (faces == 6 && (caps2 & kCaps2AllFaces) != kCaps2AllFaces) return fail("cube DDS without all 6 faces");
  H format = H::UNKNOWN;
  bool bgra = false, no_alpha = false;
  if (pf & kDdpfFourCC) {
    if (cc == FourCC("DXT1")) format = H::BC1_UNORM;
    else if (cc == FourCC("DXT2") || cc == FourCC("DXT3")) format = H::BC2_UNORM;
    else if (cc == FourCC("DXT4") || cc == FourCC("DXT5")) format = H::BC3_UNORM;
    else if (cc == FourCC("ATI1") || cc == FourCC("BC4U")) format = H::BC4_UNORM;
    else if (cc == FourCC("ATI2") || cc == FourCC("BC5U")) format = H::BC5_UNORM;
    else if (cc == FourCC("DX10")) {
      if (f.size() < kDx10HeaderBytes) return fail("DX10 header cut off");
      at = kDx10HeaderBytes;
      format = FromDxgi(Read32(f, kDx10Format), bgra);
      if (Read32(f, kDx10Dimension) != 3) return fail("DX10 DDS that is not 2D");
      const uint32_t array = std::max(1u, Read32(f, kDx10ArraySize));
      faces = (Read32(f, kDx10Misc) & 0x4) ? 6 * array : array;  // TEXTURECUBE
      if (faces != 1 && faces != 6) return fail("DDS array");
    }
  } else if ((pf & kDdpfRgb) && Read32(f, kDdsBitCount) == 32) {
    const uint32_t r = Read32(f, kDdsRedMask), g = Read32(f, kDdsGreenMask), b = Read32(f, kDdsBlueMask);
    if (r == 0x000000FF && g == 0x0000FF00 && b == 0x00FF0000) format = H::RGBA8_UNORM;
    else if (r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF) format = H::RGBA8_UNORM, bgra = true;
    no_alpha = !(pf & kDdpfAlphaPixels) || Read32(f, kDdsAlphaMask) == 0;
  }
  if (format == H::UNKNOWN) return fail("DDS format not supported (RGBA8/BGRA8, BC1-BC5, BC7)");
  if (Read32(f, kDdsDepth) > 1 && !(pf & kDdpfFourCC && cc == FourCC("DX10"))) return fail("volume DDS");
  const uint32_t w = Read32(f, kDdsWidth), h = Read32(f, kDdsHeight);
  if (!w || !h || w > kMaxSide || h > kMaxSide) return fail("size");
  if (faces == 6 && w != h) return fail("cube faces not square");
  const uint32_t mips = (Read32(f, kDdsFlags) & 0x20000) ? Read32(f, kDdsMipCount) : 1;
  out.plan.format = format;
  out.plan.dimension = faces == 6 ? res::Dimension::kCube : res::Dimension::k2D;
  out.plan.width = w;
  out.plan.height = h;
  out.plan.layers = faces;
  out.plan.levels = std::clamp(mips, 1u, FullChain(w, h));
  // file: each face's chain; here level-major
  std::vector<size_t> src(size_t(faces) * out.plan.levels);
  for (uint32_t face = 0; face < faces; ++face)
    for (uint32_t l = 0; l < out.plan.levels; ++l) {
      uint32_t pitch, rows;
      Shape(format, std::max(w >> l, 1u), std::max(h >> l, 1u), pitch, rows);
      src[size_t(face) * out.plan.levels + l] = at;
      at += size_t(pitch) * rows;
      if (at > f.size()) return fail("DDS file cut off");
    }
  for (uint32_t l = 0; l < out.plan.levels; ++l)
    for (uint32_t face = 0; face < faces; ++face) {
      res::HostSubresource& s = AddSub(out, l, face, std::max(w >> l, 1u), std::max(h >> l, 1u));
      std::memcpy(out.bytes.data() + s.offset, f.data() + src[size_t(face) * out.plan.levels + l], s.size);
    }
  if (format == H::RGBA8_UNORM && (bgra || no_alpha))
    for (size_t i = 0; i + 3 < out.bytes.size(); i += 4) {
      if (bgra) std::swap(out.bytes[i], out.bytes[i + 2]);
      if (no_alpha) out.bytes[i + 3] = 0xFF;
    }
  return true;
}

bool ReadPng(const fs::path& path, res::HostTextureData& out, std::string* why) {
  using Microsoft::WRL::ComPtr;
  out = {};
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> rgba;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr))
      hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &rgba);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = rgba->GetSize(&w, &h);
    if (FAILED(hr)) {
      if (why) *why = "WIC could not read the image";
    } else if (!w || !h || w > kMaxSide || h > kMaxSide) {
      if (why) *why = "size";
    } else {
      out.plan.format = H::RGBA8_UNORM;
      out.plan.dimension = res::Dimension::k2D;
      out.plan.width = w;
      out.plan.height = h;
      res::HostSubresource& s = AddSub(out, 0, 0, w, h);
      ok = SUCCEEDED(rgba->CopyPixels(nullptr, s.row_pitch, UINT(s.size), out.bytes.data() + s.offset));
      if (!ok && why) *why = "WIC could not decode the pixels";
    }
  }
  if (SUCCEEDED(co)) CoUninitialize();
  return ok;
}

void AddMipmaps(res::HostTextureData& t) {
  res::HostTexturePlan& p = t.plan;
  if (p.format != H::RGBA8_UNORM || p.levels != 1 || t.subresources.size() != p.layers) return;
  const uint32_t levels = FullChain(p.width, p.height);
  if (levels == 1) return;
  for (uint32_t l = 1; l < levels; ++l)
    for (uint32_t layer = 0; layer < p.layers; ++layer) {
      const uint32_t pw = std::max(p.width >> (l - 1), 1u), ph = std::max(p.height >> (l - 1), 1u);
      const uint32_t w = std::max(p.width >> l, 1u), h = std::max(p.height >> l, 1u);
      AddSub(t, l, layer, w, h);  // may move bytes: take the pointers afterwards
      const res::HostSubresource& src = *t.Find(l - 1, layer);
      const res::HostSubresource& dst = t.subresources.back();
      const uint8_t* s = t.bytes.data() + src.offset;
      uint8_t* d = t.bytes.data() + dst.offset;
      for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* r0 = s + size_t(std::min(y * 2, ph - 1)) * src.row_pitch;
        const uint8_t* r1 = s + size_t(std::min(y * 2 + 1, ph - 1)) * src.row_pitch;
        for (uint32_t x = 0; x < w; ++x) {
          const size_t x0 = size_t(std::min(x * 2, pw - 1)) * 4, x1 = size_t(std::min(x * 2 + 1, pw - 1)) * 4;
          for (uint32_t c = 0; c < 4; ++c)
            d[size_t(y) * dst.row_pitch + x * 4 + c] = uint8_t((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] + r1[x1 + c] + 2) / 4);
        }
      }
    }
  p.levels = levels;
  // level-major order (AddSub appended level by level already)
}

bool LoadReplacement(const fs::path& path, res::HostTextureData& out, std::string* why) {
  std::wstring ext = path.extension().wstring();
  for (wchar_t& c : ext) c = wchar_t(std::towlower(c));
  bool ok = false;
  if (ext == L".dds") {
    std::ifstream in(path, std::ios::binary);
    const std::vector<uint8_t> f{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    ok = ReadDds(f, out, why);
  } else if (ext == L".png") {
    ok = ReadPng(path, out, why);
  } else if (why) {
    *why = "not .png or .dds";
  }
  if (!ok) return false;
  if (IsBc(out.plan.format) && ((out.plan.width | out.plan.height) & 3)) {
    if (why) *why = "BC image whose size is not a multiple of 4";
    return false;
  }
  AddMipmaps(out);
  return true;
}

// ---- the pack ----------------------------------------------------------------------------------------------------

TexturePack::TexturePack(const Options& options) : opt_(options) {
  std::error_code ec;
  if (opt_.dump) {
    fs::create_directories(opt_.dump_dir, ec);
    for (fs::directory_iterator it(opt_.dump_dir, ec), end; !ec && it != end; it.increment(ec)) {
      const fs::path& p = it->path();
      if (p.extension() == L".png" || p.extension() == L".dds")
        if (const uint64_t k = KeyInName(Utf8(p.stem()))) dumped_.insert(k);
    }
    Log("texture pack: dumping into %s (%zu textures there already)", Utf8(opt_.dump_dir).c_str(), dumped_.size());
  }
  if (opt_.replace) {
    fs::create_directories(opt_.replace_dir, ec);
    files_ = ListReplacements();
    for (auto& [key, file] : files_) file.gen = next_gen_++;
    Log("texture pack: %zu replacement images in %s", files_.size(), Utf8(opt_.replace_dir).c_str());
  }
  worker_ = std::thread([this] { Work(); });
}

TexturePack::~TexturePack() {
  {
    std::lock_guard l(lock_);
    stop_ = true;
    loads_.clear();  // dumps still queued are written
  }
  wake_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (watch_) FindCloseChangeNotification(watch_);
}

void TexturePack::Log(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof buffer, fmt, args);
  va_end(args);
  if (opt_.log) opt_.log(buffer);
  else std::fprintf(stderr, "%s\n", buffer);
}

void TexturePack::Post(std::function<void()> job, bool load) {
  (load ? loads_ : dumps_).push_back(std::move(job));
  wake_.notify_one();
}

void TexturePack::Work() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  SetThreadDescription(GetCurrentThread(), L"texture pack");
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock l(lock_);
      wake_.wait(l, [&] { return stop_ || !loads_.empty() || !dumps_.empty(); });
      if (loads_.empty() && dumps_.empty()) break;  // stop_
      auto& q = loads_.empty() ? dumps_ : loads_;
      job = std::move(q.front());
      q.pop_front();
    }
    job();
  }
  if (SUCCEEDED(co)) CoUninitialize();
}

std::unordered_map<uint64_t, TexturePack::File> TexturePack::ListReplacements() {
  std::unordered_map<uint64_t, File> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(opt_.replace_dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    const fs::path& p = it->path();
    std::wstring ext = p.extension().wstring();
    for (wchar_t& c : ext) c = wchar_t(std::towlower(c));
    if (ext != L".png" && ext != L".dds") continue;
    const uint64_t key = KeyInName(Utf8(p.stem()));
    if (!key) continue;
    File f;
    f.path = p;
    f.size = it->file_size(e2);
    f.time = int64_t(it->last_write_time(e2).time_since_epoch().count());
    // two files for one texture: the first in path order wins (the same in every run)
    auto [old, inserted] = files.try_emplace(key, f);
    if (!inserted && p < old->second.path) old->second = f;
  }
  return files;
}

bool TexturePack::WantsDump(uint64_t key) {
  if (!key || !opt_.dump) return false;
  std::lock_guard l(lock_);
  return !dumped_.count(key) && !files_.count(key);
}

bool TexturePack::Dump(uint64_t key, const std::string& name, const std::string& info, res::HostTextureData&& data) {
  if (!key || !opt_.dump) return false;
  std::lock_guard l(lock_);
  if (dumped_.count(key) || files_.count(key)) return false;
  const size_t bytes = data.bytes.size();
  if (queued_dump_bytes_ + bytes > opt_.max_queued_dump_bytes && queued_dump_bytes_) return false;
  dumped_.insert(key);
  queued_dump_bytes_ += bytes;
  Post(
      [this, bytes, path = opt_.dump_dir / name, info, d = std::move(data)] {
        const auto t0 = Clock::now();
        std::vector<uint8_t> file;
        bool png = false;
        bool ok = EncodeDump(d, file, png);
        // under a temporary name: a dump cut off by quitting is not taken for a whole one
        fs::path part = path;
        part += L".part";
        if (ok) {
          std::ofstream out(part, std::ios::binary | std::ios::trunc);
          out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
          ok = bool(out);
        }
        std::error_code ec;
        if (ok) fs::rename(part, path, ec);
        if (!ok || ec) {
          fs::remove(part, ec);
          Log("texture pack: %s not written", Utf8(path).c_str());
        } else {
          std::ofstream index(opt_.dump_dir / "index.txt", std::ios::app | std::ios::binary);
          index << Utf8(path.filename()) << "  " << info;
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::lock_guard l2(lock_);
        queued_dump_bytes_ -= bytes;
        ++(ok && !ec ? dumps_written_ : dumps_failed_);
        dump_ms_ += ms;
      },
      false);
  return true;
}

uint32_t TexturePack::FileGen(uint64_t key) {
  if (!opt_.replace) return 0;
  std::lock_guard l(lock_);
  auto it = files_.find(key);
  return it == files_.end() ? 0 : it->second.gen;
}

TexturePack::Lookup TexturePack::Find(uint64_t key, uint32_t gen, std::shared_ptr<const res::HostTextureData>& out) {
  if (!opt_.replace || !gen) return Lookup::kNone;
  std::lock_guard l(lock_);
  auto file = files_.find(key);
  if (file == files_.end() || file->second.gen != gen) return Lookup::kNone;
  if (auto r = ready_.find(key); r != ready_.end() && r->second.gen == gen) {
    if (!r->second.data) return Lookup::kNone;  // did not load; stays so until the file changes
    out = std::move(r->second.data);
    ready_.erase(r);
    asked_.erase(key);
    return Lookup::kReady;
  }
  if (auto a = asked_.find(key); a != asked_.end() && a->second == gen) return Lookup::kLoading;
  asked_[key] = gen;
  Post([this, key, gen, path = file->second.path] { Load(key, gen, path); }, true);
  return Lookup::kLoading;
}

void TexturePack::Load(uint64_t key, uint32_t gen, fs::path path) {
  const auto t0 = Clock::now();
  auto data = std::make_shared<res::HostTextureData>();
  std::string why;
  const bool ok = LoadReplacement(path, *data, &why);
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  if (ok)
    Log("texture pack: %016llx replaced by %s (%ux%u%s, %s, %u levels, %.1f ms)", (unsigned long long)key,
        Utf8(path.filename()).c_str(), data->plan.width, data->plan.height,
        data->plan.dimension == res::Dimension::kCube ? " cube" : "", res::GetHostFormatInfo(data->plan.format).name,
        data->plan.levels, ms);
  else
    Log("texture pack: %s did not load (%s)", Utf8(path).c_str(), why.c_str());
  std::lock_guard l(lock_);
  auto a = asked_.find(key);
  if (a == asked_.end() || a->second != gen) return;  // the file changed meanwhile
  ready_[key] = Ready{gen, ok ? std::shared_ptr<const res::HostTextureData>(std::move(data)) : nullptr};
  ++(ok ? loads_ok_ : loads_failed_);
  load_ms_ += ms;
  load_ms_max_ = std::max(load_ms_max_, ms);
}

void TexturePack::Relist() {
  std::unordered_map<uint64_t, File> files = ListReplacements();
  std::lock_guard l(lock_);
  size_t added = 0, changed = 0, removed = 0;
  for (auto& [key, f] : files) {
    auto old = files_.find(key);
    if (old != files_.end() && old->second.path == f.path && old->second.size == f.size && old->second.time == f.time) {
      f.gen = old->second.gen;
      continue;
    }
    f.gen = next_gen_++;
    ++(old == files_.end() ? added : changed);
    ready_.erase(key);
    asked_.erase(key);
  }
  for (const auto& [key, f] : files_)
    if (!files.count(key)) ++removed, ready_.erase(key), asked_.erase(key);
  files_ = std::move(files);
  ++relists_;
  if (added || changed || removed) {
    revision_.fetch_add(1, std::memory_order_acq_rel);
    Log("texture pack: replacement folder changed: %zu new, %zu changed, %zu removed, %zu images", added, changed, removed,
        files_.size());
  }
}

void TexturePack::Frame() {
  if (!opt_.replace) return;
  if (!watch_) {
    // made again now and then while it fails (folder missing)
    if (watch_tries_++ % 120) return;
    HANDLE h = FindFirstChangeNotificationW(opt_.replace_dir.c_str(), TRUE, kChanges);
    if (h != INVALID_HANDLE_VALUE) watch_ = h;
    return;
  }
  const auto now = Clock::now();
  if (WaitForSingleObject(watch_, 0) == WAIT_OBJECT_0) {
    changed_ = true;
    changed_at_ = now;
    if (!FindNextChangeNotification(watch_)) {
      FindCloseChangeNotification(watch_);
      watch_ = nullptr;
    }
  }
  if (!changed_ || now - changed_at_ < kSettle) return;
  changed_ = false;
  std::lock_guard l(lock_);
  Post([this] { Relist(); }, true);
}

std::string TexturePack::Summary() {
  std::lock_guard l(lock_);
  char s[320];
  std::snprintf(s, sizeof s,
                "%llu dumps written (%llu failed, %.0f ms on the worker), %llu replacements loaded (%llu failed, %.0f ms, "
                "longest %.1f ms), %zu replacement images, %llu folder listings",
                (unsigned long long)dumps_written_, (unsigned long long)dumps_failed_, dump_ms_,
                (unsigned long long)loads_ok_, (unsigned long long)loads_failed_, load_ms_, load_ms_max_, files_.size(),
                (unsigned long long)relists_);
  return s;
}

}  // namespace fcp::nr::pack
