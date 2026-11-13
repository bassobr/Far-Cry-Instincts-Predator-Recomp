// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/resources/src/endian.cpp),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
#include "kk/resources/endian.h"

namespace fcp::res {

void CopySwap(Endian endian, const void* src, void* dst, size_t bytes) {
  const uint8_t* s = static_cast<const uint8_t*>(src);
  uint8_t* d = static_cast<uint8_t*>(dst);
  if (endian == Endian::kNone) {
    if (s != d) std::memmove(d, s, bytes);
    return;
  }
  if (endian == Endian::k8in16) {
    for (size_t i = 0; i + 1 < bytes; i += 2) {
      const uint8_t a = s[i], b = s[i + 1];
      d[i] = b;
      d[i + 1] = a;
    }
    return;
  }
  for (size_t i = 0; i + 3 < bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, s + i, 4);
    v = ApplyEndian32(v, endian);
    std::memcpy(d + i, &v, 4);
  }
}

void CopySwapIndices16(const void* src, void* dst, size_t count) { CopySwap(Endian::k8in16, src, dst, count * 2); }

void CopySwapIndices32(const void* src, void* dst, size_t count) { CopySwap(Endian::k8in32, src, dst, count * 4); }

}  // namespace fcp::res
