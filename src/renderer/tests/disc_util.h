// Helpers for tests and the probe with real data: read the disc's PC DDS files and send them through the guest path.
//
// Predator's textures are stored on the disc as PC DDS (linear, little endian, found in the disc inventory). The
// game puts them into guest memory tiled at run time. GuestRoundTrip reproduces that: DDS levels -> tiled, big
// endian (kk EncodeGuestTexture) into an artificial physical memory -> King Kong's ConvertTexture (untiling,
// endian, mip tail) -> host data. For BC and 8888 formats the result must equal the DDS levels byte for byte.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kk/resources/texture_convert.h"
#include "kk/resources/xenos.h"

namespace fcp::test {

struct DdsImage {
  uint32_t width = 0, height = 0, levels = 1;
  bool cube = false;
  fcp::res::TextureFormat format = fcp::res::TextureFormat::k_8_8_8_8;  // Xenos format of the guest copy
  std::string fourcc;                         // "DXT1", "DXT3", "DXT5", "ARGB8", "R5G6B5"
  std::vector<std::vector<uint8_t>> level;    // level i: densely packed blocks (as in the DDS file)
};

bool LoadDds(const std::string& path, DdsImage& out, std::string* why);

struct RoundTrip {
  fcp::res::HostTextureData host;
  double encode_ms = 0, convert_ms = 0;
  bool bytes_equal = false;   // Host-Ebenen == DDS-Ebenen
};
// tiled: tiled as on the console; endian: 8in16 for BC, 8in32 for 8888.
bool GuestRoundTrip(const DdsImage& dds, bool tiled, RoundTrip& out, std::string* why);

std::vector<uint8_t> ReadFile(const std::string& path);
std::string RepoPath(const std::string& relative);   // FCP_REPO_DIR + relative
std::string DiscDir();                                // FCP_DISC_DIR (compile time) or environment FCP_DISC_DIR
bool Exists(const std::string& path);

}  // namespace fcp::test
