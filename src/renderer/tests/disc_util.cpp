// disc_util.h. Eigener Code dieses Projekts.
#include "disc_util.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "kk/resources/guest_texture.h"

namespace fcp::test {

using namespace fcp::res;

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool Exists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

std::string RepoPath(const std::string& relative) { return std::string(FCP_REPO_DIR) + "/" + relative; }

std::string DiscDir() {
  if (const char* e = std::getenv("FCP_DISC_DIR"); e && *e) return e;
  return FCP_DISC_DIR;
}

namespace {
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}  // namespace

bool LoadDds(const std::string& path, DdsImage& out, std::string* why) {
  const std::vector<uint8_t> f = ReadFile(path);
  if (f.size() < 128 || std::memcmp(f.data(), "DDS ", 4) != 0) return why && (*why = "not a DDS", false);
  const uint8_t* h = f.data() + 4;
  out = {};
  out.height = U32(h + 8);
  out.width = U32(h + 12);
  out.levels = std::max(1u, U32(h + 24));
  const uint32_t pf_flags = U32(h + 76), fourcc = U32(h + 80), bits = U32(h + 84);
  out.cube = (U32(h + 108) & 0x200) != 0;
  uint32_t block = 4, bytes = 0;
  if (pf_flags & 4) {
    out.fourcc.assign(reinterpret_cast<const char*>(h + 80), 4);
    if (fourcc == 0x31545844) out.format = TextureFormat::k_DXT1, bytes = 8;
    else if (fourcc == 0x33545844) out.format = TextureFormat::k_DXT2_3, bytes = 16;
    else if (fourcc == 0x35545844) out.format = TextureFormat::k_DXT4_5, bytes = 16;
    else return why && (*why = "FourCC " + out.fourcc + " not supported", false);
  } else if (bits == 32) {
    out.fourcc = "ARGB8", out.format = TextureFormat::k_8_8_8_8, block = 1, bytes = 4;
  } else if (bits == 16 && !(pf_flags & 1)) {
    out.fourcc = "R5G6B5", out.format = TextureFormat::k_5_6_5, block = 1, bytes = 2;
  } else {
    return why && (*why = "pixel format not supported (" + std::to_string(bits) + " bit)", false);
  }
  if (out.cube) return why && (*why = "cube map (not part of this test)", false);
  size_t at = 128;
  for (uint32_t l = 0; l < out.levels; ++l) {
    const uint32_t w = std::max(1u, out.width >> l), hh = std::max(1u, out.height >> l);
    const size_t size = size_t((w + block - 1) / block) * ((hh + block - 1) / block) * bytes;
    if (at + size > f.size()) return why && (*why = "file too short", false);
    out.level.emplace_back(f.begin() + at, f.begin() + at + size);
    at += size;
  }
  return true;
}

bool GuestRoundTrip(const DdsImage& dds, bool tiled, RoundTrip& out, std::string* why) {
  TextureFetchDesc d;
  d.format = dds.format;
  const bool bc = dds.format == TextureFormat::k_DXT1 || dds.format == TextureFormat::k_DXT2_3 || dds.format == TextureFormat::k_DXT4_5;
  d.endian = bc || dds.format == TextureFormat::k_5_6_5 ? Endian::k8in16 : Endian::k8in32;
  d.tiled = tiled;
  d.packed_mips = tiled;
  d.width = dds.width;
  d.height = dds.height;
  d.max_level = dds.levels - 1;
  d.base_address = 0x00100000;
  d.mip_address = dds.levels > 1 ? 0x01000000 : 0;
  const TextureFetch fetch = MakeTextureFetch(d);

  auto t0 = std::chrono::steady_clock::now();
  GuestTextureImage image;
  if (!EncodeGuestTexture(fetch, [&](uint32_t level, uint32_t layer) -> const uint8_t* {
        return layer == 0 && level < dds.level.size() ? dds.level[level].data() : nullptr;
      }, image))
    return why && (*why = "EncodeGuestTexture failed", false);
  // Artificial physical memory: base at 1 MB, mips at 16 MB.
  std::vector<uint8_t> memory(0x01000000 + image.mips.size() + 4096, 0);
  if (d.base_address + image.base.size() > d.mip_address && d.mip_address) return why && (*why = "base too large", false);
  std::memcpy(memory.data() + d.base_address, image.base.data(), image.base.size());
  if (!image.mips.empty()) std::memcpy(memory.data() + d.mip_address, image.mips.data(), image.mips.size());
  auto t1 = std::chrono::steady_clock::now();
  GuestMemory guest;
  guest.base = memory.data();
  guest.size = memory.size();
  if (!ConvertTexture(fetch, guest, out.host, why)) return false;
  auto t2 = std::chrono::steady_clock::now();
  out.encode_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  out.convert_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
  // Comparison: every host level against the DDS level (densely packed, rows of equal length).
  out.bytes_equal = out.host.plan.conversion == Conversion::kCopy;
  for (uint32_t l = 0; out.bytes_equal && l < dds.levels; ++l) {
    const HostSubresource* s = out.host.Find(l, 0);
    if (!s || s->size < dds.level[l].size()) {
      out.bytes_equal = false;
      break;
    }
    const uint32_t rows = bc ? (s->height + 3) / 4 : s->height;
    const size_t row_bytes = dds.level[l].size() / rows;
    for (uint32_t r = 0; r < rows && out.bytes_equal; ++r)
      out.bytes_equal = std::memcmp(out.host.bytes.data() + s->offset + size_t(r) * s->row_pitch,
                                    dds.level[l].data() + r * row_bytes, row_bytes) == 0;
  }
  return true;
}

}  // namespace fcp::test
