// FCP (local change 370, see UPSTREAM.md): sound data registered by content and the speech gain.
// See rex/fcp_speech.h.

#include <rex/fcp_speech.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace rex::fcp_speech {
namespace {

constexpr size_t kMaxPositions = size_t{4} << 20;  // ~100 MB of map at most

std::atomic<f32> g_gain{1.0f};
std::shared_mutex g_mutex;
// hash -> kind (bits 0-1) | (entry offset % 2048) / kPhaseStep (bits 2-5)
std::unordered_map<u64, u8> g_positions;
u64 g_count[3] = {};
bool g_full = false;

u64 Hash(const u8* p) {
  u64 h = 0xCBF29CE484222325ull;  // FNV-1a
  for (u32 i = 0; i < kWindow; ++i) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

// An XMA packet that starts a sound: the header's first frame begins right after it (bit offset 0, bits 25..11 of
// the big endian header word). Only such places are registered, about one in 8192 positions of other data.
bool PacketStart(const u8* p) {
  return (p[0] & 3) == 0 && p[1] == 0 && (p[2] & 0xE0) == 0;
}

}  // namespace

void SetGain(f32 gain) {
  g_gain.store(std::clamp(gain, 0.0f, 1.0f), std::memory_order_relaxed);
}

f32 Gain() {
  return g_gain.load(std::memory_order_relaxed);
}

void Register(const u8* data, u32 size, u32 entry_offset, DataKind kind) {
  if (!data || size < kWindow || kind == DataKind::kUnknown) {
    return;
  }
  u32 p = (kGrain - entry_offset % kGrain) % kGrain;
  std::unique_lock lock(g_mutex);
  for (; p + kWindow <= size; p += kGrain) {
    if (!PacketStart(data + p)) {
      continue;
    }
    if (g_positions.size() >= kMaxPositions) {
      g_full = true;
      return;
    }
    const u8 phase = static_cast<u8>(((entry_offset + p) % 2048) / kPhaseStep);
    const u8 value = static_cast<u8>(static_cast<u8>(kind) | phase << 2);
    auto [it, inserted] = g_positions.try_emplace(Hash(data + p), value);
    if (inserted) {
      ++g_count[static_cast<u8>(kind)];
    } else if ((it->second & 3) != static_cast<u8>(kind)) {
      // the same bytes in speech and in other sound (silence, say): counts as sound, stays as it was
      if ((it->second & 3) == static_cast<u8>(DataKind::kSpeech)) {
        --g_count[static_cast<u8>(DataKind::kSpeech)];
        ++g_count[static_cast<u8>(DataKind::kSound)];
      }
      it->second = static_cast<u8>((it->second & ~3u) | static_cast<u8>(DataKind::kSound));
    }
  }
}

DataKind KindOf(const u8* data, u32* phase) {
  if (!data) {
    return DataKind::kUnknown;
  }
  const u64 h = Hash(data);
  std::shared_lock lock(g_mutex);
  auto it = g_positions.find(h);
  if (it == g_positions.end()) {
    return DataKind::kUnknown;
  }
  if (phase) {
    *phase = u32(it->second >> 2) * kPhaseStep;
  }
  return static_cast<DataKind>(it->second & 3);
}

void Registered(u64* sound, u64* speech) {
  std::shared_lock lock(g_mutex);
  if (sound) *sound = g_count[static_cast<u8>(DataKind::kSound)];
  if (speech) *speech = g_count[static_cast<u8>(DataKind::kSpeech)];
}

}  // namespace rex::fcp_speech
