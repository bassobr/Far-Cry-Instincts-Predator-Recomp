// Small helpers to read guest memory from host code (big endian, same address rule as the codegen).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <windows.h>

namespace fcp::guest {

inline uint8_t* Ptr(uint8_t* base, uint32_t address) {
#if REX_PHYSICAL_OFFSET_IN_CODE
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

inline uint32_t U32(uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, Ptr(base, address), 4);
  return __builtin_bswap32(v);
}

inline uint16_t U16(uint8_t* base, uint32_t address) {
  uint16_t v;
  std::memcpy(&v, Ptr(base, address), 2);
  return __builtin_bswap16(v);
}

/** Plausible guest data address (heaps, image, stacks, physical). */
inline bool Plausible(uint32_t address) {
  return address >= 0x10000u && address < 0xFFFF0000u;
}

/**
 * True when the guest page table of the SDK maps [address, address+size) readable (one lock and a table lookup per
 * guest page, no system call). False also when the range is in no heap or crosses one. See Readable.
 */
bool GuestTableReadable(uint32_t address, size_t size);

/**
 * True when the fast check is on (default). FCP_GUEST_READABLE_VQ=1 = VirtualQuery for every call (the original),
 * =verify = both, mismatches are counted and logged (VerifyReadable).
 */
bool FastReadable();
bool VerifyReadableOn();
bool VerifyReadable(uint8_t* base, uint32_t address, size_t size);

/** True when [address, address+size) is committed, readable host memory (VirtualQuery, the original check). */
inline bool HostReadable(uint8_t* base, uint32_t address, size_t size) {
  if (!Plausible(address)) {
    return false;
  }
  const uint8_t* p = Ptr(base, address);
  const uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || info.Protect == 0) {
      return false;
    }
    p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

/**
 * True when [address, address+size) is readable. First the guest page table (the SDK commits host memory with every
 * guest commit, so a readable guest range is readable host memory); only when that says no, VirtualQuery decides as
 * before. VirtualQuery alone cost ~55 % of the main thread in a level (Player_LookInput hook, measured),
 * presumably because Windows walks the page table of the whole region for its size.
 */
inline bool Readable(uint8_t* base, uint32_t address, size_t size) {
  if (!Plausible(address)) {
    return false;
  }
  if (FastReadable()) {
    if (VerifyReadableOn()) {
      return VerifyReadable(base, address, size);
    }
    if (GuestTableReadable(address, size)) {
      return true;
    }
  }
  return HostReadable(base, address, size);
}

/** Copies up to max bytes of a NUL terminated string; stops at an access fault (structured exception). */
size_t CopyGuestString(const uint8_t* src, char* dst, size_t max);
/** True unless FCP_GUEST_STRING_UNGUARDED=1 (the original unguarded read). */
bool GuardedStrings();

/** Reads a NUL terminated guest string, at most max bytes, non printable bytes as '?'. */
inline std::string CString(uint8_t* base, uint32_t address, size_t max = 260) {
  std::string out;
  if (!Readable(base, address, max)) {
    // Shorter strings near the end of a mapping: try a smaller window.
    if (!Readable(base, address, 16)) {
      return out;
    }
    max = 16;
  }
  const uint8_t* p = Ptr(base, address);
  if (GuardedStrings()) {
    out.resize(max);
    out.resize(CopyGuestString(p, out.data(), max));
    for (char& c : out) {
      const auto u = static_cast<uint8_t>(c);
      if (u < 0x20 || u >= 0x7F) {
        c = '?';
      }
    }
    return out;
  }
  for (size_t i = 0; i < max && p[i]; ++i) {
    out.push_back(p[i] >= 0x20 && p[i] < 0x7F ? char(p[i]) : '?');
  }
  return out;
}

/** Reads a counted ANSI_STRING {u16 length, u16 max, u32 buffer}. */
inline std::string AnsiString(uint8_t* base, uint32_t address) {
  if (!Readable(base, address, 8)) {
    return {};
  }
  const uint16_t length = U16(base, address);
  const uint32_t buffer = U32(base, address + 4);
  if (!Readable(base, buffer, length)) {
    return {};
  }
  std::string out;
  const uint8_t* p = Ptr(base, buffer);
  for (uint16_t i = 0; i < length && i < 512; ++i) {
    out.push_back(p[i] >= 0x20 && p[i] < 0x7F ? char(p[i]) : '?');
  }
  return out;
}

}  // namespace fcp::guest
