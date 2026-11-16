// Speech volume (fcp/speech_volume.h). Own code of this project; the idea (the language banks are the speech, scale
// their voices) comes from Far Cry Instincts Recomp (MIT, same author, GitHub #25).
#include "fcp/speech_volume.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include <rex/fcp_speech.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"

namespace fcp::speech {
namespace {

using rex::fcp_speech::DataKind;

// The sound archive and its project, as the disc names them (the game builds the same paths). An entry's name hash
// is CRC-32 over the lowercase path with a leading backslash (tools/xfat.py).
constexpr const char* kArchive = "dvdsound.dat";
constexpr const char* kFat = "dvdsound.fat";
constexpr const char* kProject = "\\sounds_dvd\\farcry360.sp4";
constexpr const char* kBankDir = "\\sounds_dvd\\";
constexpr const char* kLanguages[] = {"english", "french", "german", "italian", "spanish"};

bool g_enabled = true;
bool g_trace = false;
std::mutex g_mutex;

enum class FileKind : uint8_t { kArchive, kFat };
struct Handle {
  FileKind kind;
  uint64_t position = 0;
  uint8_t tail[63];  // the end of the last read (rex::fcp_speech::kWindow - 1 bytes)
  uint32_t tail_size = 0;
  uint64_t tail_end = 0;
};
std::unordered_map<uint32_t, Handle> g_handles;

struct Entry {
  uint32_t offset, size, hash;
  DataKind kind = DataKind::kSound;
};
std::vector<Entry> g_entries;  // by offset
std::vector<uint8_t> g_fat;    // the fat as the game read it
std::vector<uint8_t> g_project;
bool g_classified = false;
uint32_t g_project_offset = 0, g_project_size = 0;

// counters
uint64_t g_reads = 0, g_read_bytes = 0, g_speech_reads = 0, g_speech_bytes = 0, g_pending_reads = 0,
         g_unreadable_reads = 0, g_unclassified_reads = 0;
std::atomic<int> g_volume{100};

struct Pending {
  bool active = false;
  uint32_t handle = 0, buffer = 0, length = 0;
  uint64_t offset = 0;
  FileKind kind = FileKind::kArchive;
};
thread_local Pending t_pending;

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool EndsWith(const std::string& s, const char* tail) {
  const size_t n = std::strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

uint32_t Crc32(const std::string& s) {
  static uint32_t table[256];
  static std::once_flag once;
  std::call_once(once, [] {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  });
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char ch : s) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

uint32_t LE32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// "DAT2", u32 count, count x {u32 offset, u32 stored size, u32 unpacked size, u32 hash} (little endian)
bool ParseFat() {
  if (g_fat.size() < 8 || std::memcmp(g_fat.data(), "DAT2", 4) != 0) return false;
  const uint32_t count = LE32(g_fat.data() + 4);
  if (g_fat.size() < 8 + size_t(count) * 16) return false;
  g_entries.clear();
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = g_fat.data() + 8 + size_t(i) * 16;
    g_entries.push_back(Entry{LE32(e), LE32(e + 4), LE32(e + 12)});
  }
  std::sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) { return a.offset < b.offset; });
  const uint32_t project = Crc32(kProject);
  for (const Entry& e : g_entries) {
    if (e.hash == project) g_project_offset = e.offset, g_project_size = e.size;
  }
  FCPLOG("speech volume: {} read, {} entries, project {} at {:#x} ({} bytes)", kFat, count, kProject,
         g_project_offset, g_project_size);
  return true;
}

// Every bank the project names ("Name.SB4"): the entries "sounds_dvd\<language>\name.sb4" are speech.
void Classify() {
  std::set<std::string> names;
  std::string cur;
  for (uint8_t b : g_project) {
    const char c = char(b);
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') {
      cur.push_back(c);
      continue;
    }
    if (cur.size() > 4) {
      const std::string l = Lower(cur);
      if (EndsWith(l, ".sb4")) names.insert(l);
    }
    cur.clear();
  }
  std::unordered_map<uint32_t, int> speech;  // hash -> language
  for (const std::string& n : names) {
    for (int l = 0; l < 5; ++l) speech[Crc32(std::string(kBankDir) + kLanguages[l] + "\\" + n)] = l;
  }
  size_t per_language[5] = {};
  uint64_t speech_bytes = 0;
  for (Entry& e : g_entries) {
    auto it = speech.find(e.hash);
    if (it == speech.end()) continue;
    e.kind = DataKind::kSpeech;
    ++per_language[it->second];
    speech_bytes += e.size;
  }
  g_classified = true;
  FCPLOG("speech volume: project names {} banks; speech banks english/french/german/italian/spanish = "
         "{}/{}/{}/{}/{} ({:.1f} MiB), every other entry counts as sound",
         names.size(), per_language[0], per_language[1], per_language[2], per_language[3], per_language[4],
         speech_bytes / 1048576.0);
}

// Registers bytes of the archive (data, from archive offset `offset`) by the entries they belong to. Returns whether
// speech was among them.
bool RegisterBytes(const uint8_t* data, uint64_t length, uint64_t offset) {
  const uint64_t end = offset + length;
  auto it = std::upper_bound(g_entries.begin(), g_entries.end(), offset,
                             [](uint64_t o, const Entry& e) { return o < e.offset; });
  if (it != g_entries.begin()) --it;
  bool speech = false;
  for (; it != g_entries.end() && it->offset < end; ++it) {
    const uint64_t s = std::max<uint64_t>(it->offset, offset), e = std::min<uint64_t>(uint64_t(it->offset) + it->size, end);
    if (e <= s) continue;
    rex::fcp_speech::Register(data + (s - offset), uint32_t(e - s), uint32_t(s - it->offset), it->kind);
    speech = speech || it->kind == DataKind::kSpeech;
  }
  return speech;
}

// Registers the bytes just read (buffer, read from archive offset `offset`). The game copies its reads out of a
// staging buffer (measured: eight 64 KiB slots, every bank goes through them), so the content, not the address, is
// what the XMA decoder can find again. A sound that starts in the last bytes of one read is found with the seam
// (the end of the previous read of this handle and the start of this one).
void RegisterRead(uint8_t* base, Handle& h, uint32_t buffer, uint32_t length, uint64_t offset) {
  if (!g_classified) {
    ++g_unclassified_reads;
    return;
  }
  if (!fcp::guest::Readable(base, buffer, length)) {
    ++g_unreadable_reads;
    h.tail_size = 0;
    return;
  }
  const uint8_t* data = fcp::guest::Ptr(base, buffer);
  constexpr uint32_t kTail = rex::fcp_speech::kWindow - 1;
  if (h.tail_size == kTail && h.tail_end == offset && length > 0) {
    uint8_t seam[2 * kTail];
    const uint32_t head = std::min<uint32_t>(kTail, length);
    std::memcpy(seam, h.tail, kTail);
    std::memcpy(seam + kTail, data, head);
    RegisterBytes(seam, kTail + head, offset - kTail);
  }
  const bool speech = RegisterBytes(data, length, offset);
  if (length >= kTail) {
    std::memcpy(h.tail, data + length - kTail, kTail);
    h.tail_size = kTail;
    h.tail_end = offset + length;
  } else {
    h.tail_size = 0;
  }
  if (speech) {
    ++g_speech_reads;
    g_speech_bytes += length;
  }
  if (g_trace) {
    FCPLOG("speech volume: read off={:#x} len={:#x} -> {:08X}{}", offset, length, buffer, speech ? " speech" : "");
  }
}
}  // namespace

void Configure() {
  g_enabled = fcp::env::Get("FCP_SPEECH_TRACK") != "0";
  g_trace = fcp::env::Flag("FCP_TRACE_SPEECH");
  const std::string v = fcp::env::Get("FCP_SPEECH_VOLUME");
  SetVolume(v.empty() ? 100 : std::atoi(v.c_str()));
  if (!g_enabled) {
    FCPLOG("speech volume: FCP_SPEECH_TRACK=0: no tracking, the speech stays at the console's level");
  }
}

bool Enabled() { return g_enabled; }

void SetVolume(int percent) {
  percent = std::clamp(percent, 0, 100);
  g_volume = percent;
  rex::fcp_speech::SetGain(g_enabled ? float(percent) / 100.0f : 1.0f);
  FCPLOG("speech volume: {} %{}", percent, percent == 100 ? " (the console's mix)" : "");
}

void OnOpen(const std::string& name, uint32_t handle) {
  if (!g_enabled) return;
  const std::string l = Lower(name);
  const bool archive = EndsWith(l, kArchive), fat = EndsWith(l, kFat);
  if (!archive && !fat) return;
  std::lock_guard lock(g_mutex);
  g_handles[handle] = Handle{archive ? FileKind::kArchive : FileKind::kFat};
}

void BeforeRead(uint8_t* base, uint32_t handle, uint32_t buffer, uint32_t length, uint32_t offset_address) {
  t_pending.active = false;
  if (!g_enabled) return;
  std::lock_guard lock(g_mutex);
  auto it = g_handles.find(handle);
  if (it == g_handles.end()) return;
  Handle& h = it->second;
  uint64_t offset = h.position;
  if (offset_address && fcp::guest::Plausible(offset_address)) {
    const uint64_t given =
        (uint64_t(fcp::guest::U32(base, offset_address)) << 32) | fcp::guest::U32(base, offset_address + 4);
    if ((given >> 32) != 0xFFFFFFFFull) offset = given;  // -1/-2: the file pointer
  }
  h.position = offset + length;
  t_pending = Pending{true, handle, buffer, length, offset, h.kind};
}

void AfterRead(uint8_t* base, uint32_t status) {
  if (!t_pending.active) return;
  Pending p = t_pending;
  t_pending.active = false;
  constexpr uint32_t kPending = 0x103;
  if (status != 0 && status != kPending) return;
  std::lock_guard lock(g_mutex);
  if (p.kind == FileKind::kFat) {
    // the game reads its fat (maybe in pieces); keep the bytes at their offset
    if (status == 0 && fcp::guest::Readable(base, p.buffer, p.length) && p.offset < (64u << 20)) {
      if (g_fat.size() < p.offset + p.length) g_fat.resize(size_t(p.offset + p.length));
      std::memcpy(g_fat.data() + p.offset, fcp::guest::Ptr(base, p.buffer), p.length);
      ParseFat();
    }
    return;
  }
  ++g_reads;
  g_read_bytes += p.length;
  // the project: keep its bytes, then the banks it names are known
  if (!g_classified && g_project_size && p.offset < uint64_t(g_project_offset) + g_project_size &&
      p.offset + p.length > g_project_offset && status == 0) {
    if (g_project.size() < g_project_size) g_project.resize(g_project_size);
    const uint64_t s = std::max<uint64_t>(p.offset, g_project_offset);
    const uint64_t e = std::min<uint64_t>(p.offset + p.length, uint64_t(g_project_offset) + g_project_size);
    if (fcp::guest::Readable(base, p.buffer + uint32_t(s - p.offset), size_t(e - s))) {
      std::memcpy(g_project.data() + (s - g_project_offset), fcp::guest::Ptr(base, p.buffer + uint32_t(s - p.offset)),
                  size_t(e - s));
      if (e == uint64_t(g_project_offset) + g_project_size) Classify();
    }
  }
  if (status != 0) {
    ++g_pending_reads;  // the data is not there yet (the SDK completes reads at once; never seen)
    return;
  }
  auto it = g_handles.find(p.handle);
  if (it != g_handles.end()) RegisterRead(base, it->second, p.buffer, p.length, p.offset);
}

void OnSetInformation(uint8_t* base, uint32_t handle, uint32_t info_class, uint32_t info) {
  constexpr uint32_t kFilePositionInformation = 14;
  if (!g_enabled || info_class != kFilePositionInformation || !fcp::guest::Plausible(info)) return;
  std::lock_guard lock(g_mutex);
  auto it = g_handles.find(handle);
  if (it != g_handles.end()) {
    it->second.position = (uint64_t(fcp::guest::U32(base, info)) << 32) | fcp::guest::U32(base, info + 4);
  }
}

void OnClose(uint32_t handle) {
  if (!g_enabled) return;
  std::lock_guard lock(g_mutex);
  g_handles.erase(handle);
}

void WriteSummary() {
  if (!g_enabled) return;
  std::lock_guard lock(g_mutex);
  uint64_t sound = 0, speech = 0;
  rex::fcp_speech::Registered(&sound, &speech);
  FCPLOG("speech volume summary: {} %, fat {}, banks classified {}, archive reads {} ({:.1f} MiB), of them with "
         "speech {} ({:.1f} MiB), pending {}, unreadable {}, before the classification {}; registered positions "
         "sound {}, speech {}",
         g_volume.load(), g_entries.empty() ? "missing" : "read", g_classified ? "yes" : "no", g_reads,
         g_read_bytes / 1048576.0, g_speech_reads, g_speech_bytes / 1048576.0, g_pending_reads, g_unreadable_reads,
         g_unclassified_reads, sound, speech);
}

}  // namespace fcp::speech
