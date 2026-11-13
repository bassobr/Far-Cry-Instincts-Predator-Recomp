#include "fcp/sound_trace.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"

namespace fcp::sound_trace {
namespace {

bool g_enabled = false;
std::mutex g_mutex;
struct Handle {
  std::string name;
  uint64_t position = 0;
  uint64_t reads = 0;
  uint64_t bytes = 0;
};
std::map<uint32_t, Handle> g_handles;
uint64_t g_reads = 0, g_bytes = 0;
std::chrono::steady_clock::time_point g_t0;

bool IsSoundArchive(std::string name) {
  std::transform(name.begin(), name.end(), name.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return name.find("dvdsound.dat") != std::string::npos;
}

}  // namespace

void Configure() {
  g_enabled = fcp::env::Flag("FCP_TRACE_SOUND_READS");
  g_t0 = std::chrono::steady_clock::now();
  if (g_enabled) {
    FCPLOG("FCP_TRACE_SOUND_READS: every read of dvdsound.dat is logged");
  }
}

bool Enabled() {
  return g_enabled;
}

void OnOpen(const std::string& name, uint32_t handle) {
  if (!g_enabled || !IsSoundArchive(name)) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_handles[handle] = Handle{name};
  FCPLOG("sound open handle={:08X} name=\"{}\"", handle, name);
}

void OnRead(uint32_t handle, uint32_t length, uint8_t* base, uint32_t offset_address, uint32_t lr) {
  if (!g_enabled) {
    return;
  }
  std::lock_guard lock(g_mutex);
  auto it = g_handles.find(handle);
  if (it == g_handles.end()) {
    return;
  }
  Handle& h = it->second;
  uint64_t offset = h.position;
  if (offset_address && fcp::guest::Plausible(offset_address)) {
    const uint64_t given = (uint64_t(fcp::guest::U32(base, offset_address)) << 32) |
                           fcp::guest::U32(base, offset_address + 4);
    if ((given >> 32) != 0xFFFFFFFFull) {  // -1/-2: use the file pointer
      offset = given;
    }
  }
  h.position = offset + length;
  ++h.reads;
  h.bytes += length;
  ++g_reads;
  g_bytes += length;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  FCPLOG("sound read t={:.3f} handle={:08X} off={:X} len={:X} lr={:08X}", t, handle, offset, length, lr);
}

void OnSetInformation(uint32_t handle, uint32_t info_class, uint8_t* base, uint32_t info) {
  constexpr uint32_t kFilePositionInformation = 14;
  if (!g_enabled || info_class != kFilePositionInformation || !fcp::guest::Plausible(info)) {
    return;
  }
  std::lock_guard lock(g_mutex);
  auto it = g_handles.find(handle);
  if (it != g_handles.end()) {
    it->second.position =
        (uint64_t(fcp::guest::U32(base, info)) << 32) | fcp::guest::U32(base, info + 4);
  }
}

void OnClose(uint32_t handle) {
  if (!g_enabled) {
    return;
  }
  std::lock_guard lock(g_mutex);
  g_handles.erase(handle);
}

void WriteSummary() {
  if (!g_enabled) {
    return;
  }
  std::lock_guard lock(g_mutex);
  FCPLOG("sound read summary: {} reads, {} bytes, {} open handles", g_reads, g_bytes, g_handles.size());
}

}  // namespace fcp::sound_trace
