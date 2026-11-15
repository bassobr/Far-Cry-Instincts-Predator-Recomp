// Save games: backups, the start check and the wait at the end (fcp/save_guard.h).
// Model: FCI's save guard (Far Cry Instincts Recomp, src/kernel/k_save.cpp, MIT, same author): quiet backups of the
// last good state, a check at the start that moves damaged files aside and puts the backup in place, a bounded wait
// for a save at the end, test switches that end the program in the middle of a save. Written anew for XContent
// folders of the ReXGlue SDK.
#include "fcp/save_guard.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "fcp/devstart.h"
#include "fcp/env.h"
#include "fcp/hash.h"
#include "fcp/log.h"
#include "fcp/relaunch.h"
#include "fcp/settings_menu.h"
#include "fcp/user_paths.h"

namespace fcp::save_guard {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kSavedGame = 1;               // XContentType of a saved game (folder 00000001)
constexpr uint32_t kHeaderSize = 0x148;          // XCONTENT_AGGREGATE_DATA, + 4 with a license mask
constexpr uint32_t kHeaderFileNameAt = 0x108;    // file name, 42 bytes
constexpr char kThumbnail[] = "__thumbnail.png";  // the SDK's thumbnail of a package
// SDK change 330 (rexglue/src/xam/content_manager.h)
constexpr char kStaging[] = ".saving";
constexpr char kReady[] = ".ready";
constexpr char kPrevious[] = ".old";

bool g_off = false;          // FCP_SAVE_GUARD=0
int g_keep = 3;              // FCP_SAVE_BACKUPS
int64_t g_exit_wait_ms = 5000;

std::string U8(const fs::path& p) { return fcp::paths::Utf8(p); }
fs::path FromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
  const size_t n = std::strlen(suffix);
  return s.size() > n && Lower(s.substr(s.size() - n)) == suffix;
}

bool IsHex(const std::string& s, size_t n) {
  return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

std::string Stamp() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  return std::format("{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                     t.wMilliseconds);
}

std::string Size(uintmax_t bytes) {
  return bytes >= 1024 * 1024 ? std::format("{:.2f} MB", double(bytes) / (1024.0 * 1024.0))
                              : std::format("{} bytes", bytes);
}

fs::path BackupRoot() { return fcp::paths::Root() / "save_backup"; }
fs::path DamagedRoot() { return fcp::paths::Root() / "save_damaged"; }

// ---- a saved game on disk ----

struct Save {
  std::string xuid, title, name;  // folder names
  fs::path dir;                   // user\<xuid>\<title>\00000001\<name>
  fs::path header;                // user\<xuid>\<title>\Headers\00000001\<name>.header
  std::string Id() const { return xuid + "\\" + title + "\\" + name; }
  fs::path BackupDir() const { return BackupRoot() / xuid / title / std::format("{:08X}", kSavedGame) / name; }
  fs::path ProfileDir() const { return fcp::paths::Saves() / title / "profile"; }
};

std::string TypeFolder() { return std::format("{:08X}", kSavedGame); }

// every <xuid>\<title>\00000001 folder below the saves (xuid 0 = installed content: not a player's save)
std::vector<fs::path> TypeFolders() {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& x : fs::directory_iterator(fcp::paths::Saves(), ec)) {
    const std::string xn = U8(x.path().filename());
    if (!x.is_directory(ec) || !IsHex(xn, 16) || xn == "0000000000000000") continue;
    for (const auto& t : fs::directory_iterator(x.path(), ec)) {
      if (!t.is_directory(ec) || !IsHex(U8(t.path().filename()), 8)) continue;
      const fs::path type = t.path() / TypeFolder();
      if (fs::is_directory(type, ec)) out.push_back(type);
    }
  }
  return out;
}

Save MakeSave(const fs::path& type_folder, const std::string& name) {
  Save s;
  s.name = name;
  s.title = U8(type_folder.parent_path().filename());
  s.xuid = U8(type_folder.parent_path().parent_path().filename());
  s.dir = type_folder / FromU8(name);
  s.header = type_folder.parent_path() / "Headers" / TypeFolder() / FromU8(name + ".header");
  return s;
}

std::vector<Save> AllSaves() {
  std::vector<Save> out;
  std::error_code ec;
  for (const fs::path& type : TypeFolders()) {
    for (const auto& e : fs::directory_iterator(type, ec)) {
      const std::string n = U8(e.path().filename());
      if (!e.is_directory(ec) || EndsWith(n, kStaging) || EndsWith(n, kReady) || EndsWith(n, kPrevious)) continue;
      out.push_back(MakeSave(type, n));
    }
  }
  return out;
}

struct Check {
  bool ok = false;
  bool header_ok = false;
  std::string why;   // what is wrong
  std::string note;  // what is unusual but fine
  int files = 0;
  uintmax_t bytes = 0;
};

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// the XContent header: complete, of a saved game, naming this package
std::string HeaderProblem(const fs::path& header, const std::string& name) {
  std::ifstream in(header, std::ios::binary);
  if (!in) return "header missing";
  std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
  if (b.size() != kHeaderSize && b.size() != kHeaderSize + 4) return std::format("header has {} bytes", b.size());
  if (Be32(b.data() + 4) != kSavedGame) return std::format("header type {:08X}", Be32(b.data() + 4));
  std::string file(reinterpret_cast<const char*>(b.data() + kHeaderFileNameAt), 42);
  file = file.substr(0, file.find('\0'));
  if (Lower(file) != Lower(name)) return std::format("header names '{}'", file);
  return {};
}

// the game's own file check (title configuration, not an address): nothing yet beyond size; see CheckContent
Check CheckContent(const fs::path& dir, const fs::path& header, const std::string& name) {
  Check c;
  std::error_code ec;
  std::string problems;
  auto add = [&](const std::string& s) { problems += (problems.empty() ? "" : "; ") + s; };
  int game_files = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const uintmax_t size = e.file_size(ec);
    ++c.files;
    c.bytes += size;
    const std::string n = U8(e.path().filename());
    if (Lower(n) != kThumbnail) ++game_files;
    if (size == 0) add(std::format("{} is empty", n));
  }
  if (ec) add("folder not readable: " + ec.message());
  if (game_files == 0) add("no file of the game");
  const std::string h = HeaderProblem(header, name);
  c.header_ok = h.empty();
  if (h == "header missing") {
    c.note = "no header (the game sees the folder name)";
  } else if (!h.empty()) {
    add(h);
  }
  c.why = problems;
  c.ok = problems.empty();
  return c;
}

std::string Describe(const Check& c) { return std::format("{} files, {}", c.files, Size(c.bytes)); }

// names, sizes and write times of the files (a copy with CopyFileW keeps the times)
std::string Fingerprint(const fs::path& dir) {
  std::vector<std::string> parts;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    parts.push_back(std::format("{}|{}|{}", Lower(U8(e.path().filename())), e.file_size(ec),
                                e.last_write_time(ec).time_since_epoch().count()));
  }
  std::sort(parts.begin(), parts.end());
  std::string s;
  for (const std::string& p : parts) s += p + "\n";
  return s;
}

bool CopyFiles(const fs::path& from, const fs::path& to, bool recursive) {
  std::error_code ec;
  fs::create_directories(to, ec);
  for (const auto& e : fs::directory_iterator(from, ec)) {
    std::error_code e2;
    if (e.is_directory(e2)) {
      if (recursive && !CopyFiles(e.path(), to / e.path().filename(), true)) return false;
      continue;
    }
    if (!e.is_regular_file(e2)) continue;
    if (!CopyFileW(e.path().c_str(), (to / e.path().filename()).c_str(), FALSE)) return false;
  }
  return !ec;
}

std::vector<fs::path> Generations(const Save& s) {
  std::vector<fs::path> out;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(s.BackupDir(), ec)) {
    if (e.is_directory(ec) && !EndsWith(U8(e.path().filename()), ".tmp")) out.push_back(e.path());
  }
  std::sort(out.begin(), out.end());  // the names are times: oldest first
  return out;
}

// ---- backups ----

std::mutex g_lock;
std::condition_variable g_wake;
std::deque<std::string> g_jobs;  // save names to back up ("" = every save)
bool g_backup_running = false;
std::map<std::string, std::chrono::steady_clock::time_point> g_open;  // root -> save opened for writing
std::map<std::string, std::string> g_open_file;                       // root -> save name
int g_saves_started = 0;

bool SaveOpen() {
  const auto now = std::chrono::steady_clock::now();
  for (const auto& [root, at] : g_open) {
    if (now - at < std::chrono::seconds(60)) return true;  // older: a create that failed without a close
  }
  return false;
}

// copies the save's state as a new backup generation, unless the newest one holds it already
void Backup(const Save& s, const char* why) {
  std::error_code ec;
  const Check c = CheckContent(s.dir, s.header, s.name);
  if (!c.ok) {
    FCPLOG("save: '{}' not backed up ({}): {}", s.Id(), why, c.why);
    return;
  }
  const std::string print = Fingerprint(s.dir);
  const std::vector<fs::path> gens = Generations(s);
  if (!gens.empty() && Fingerprint(gens.back() / "content") == print) return;  // this state is backed up
  const auto t0 = std::chrono::steady_clock::now();
  const fs::path dst = s.BackupDir() / Stamp();
  const fs::path tmp = fs::path(dst.wstring() + L".tmp");
  fs::remove_all(tmp, ec);
  bool ok = CopyFiles(s.dir, tmp / "content", false);
  if (ok && fs::exists(s.header, ec)) {
    ok = CopyFileW(s.header.c_str(), (tmp / s.header.filename()).c_str(), FALSE) != 0;
  }
  if (ok && fs::is_directory(s.ProfileDir(), ec)) ok = CopyFiles(s.ProfileDir(), tmp / "profile", true);
  // the copy must be the state that was checked: a save that started meanwhile makes it another one
  ok = ok && Fingerprint(tmp / "content") == print && Fingerprint(s.dir) == print;
  if (!ok) {
    fs::remove_all(tmp, ec);
    FCPLOG("save: backup of '{}' failed or the save changed meanwhile; the previous backups stay", s.Id());
    return;
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove_all(tmp, ec);
    FCPLOG("save: backup of '{}' could not be put in place: {}", s.Id(), ec.message());
    return;
  }
  std::vector<fs::path> now = Generations(s);
  int removed = 0;
  while (int(now.size()) > g_keep) {
    fs::remove_all(now.front(), ec);
    now.erase(now.begin());
    ++removed;
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  FCPLOG("save: '{}' backed up ({}, {}) in {:.0f} ms: {} ({} of at most {} kept{})", s.Id(), why, Describe(c), ms,
         U8(dst), now.size(), g_keep, removed ? std::format(", {} oldest removed", removed) : "");
}

void Worker() {
  SetThreadDescription(GetCurrentThread(), L"fcp save backup");
  for (;;) {
    std::string job;
    {
      std::unique_lock lock(g_lock);
      g_wake.wait(lock, [] { return !g_jobs.empty(); });
      // not while a save is written: the next close queues it again
      while (SaveOpen()) g_wake.wait_for(lock, std::chrono::milliseconds(200));
      job = g_jobs.front();
      g_jobs.pop_front();
      g_backup_running = true;
    }
    for (const Save& s : AllSaves()) {
      if (job.empty() || Lower(s.name) == Lower(job)) Backup(s, job.empty() ? "state at the start" : "after the save");
    }
    {
      std::lock_guard lock(g_lock);
      g_backup_running = false;
    }
    g_wake.notify_all();
  }
}

void Queue(const std::string& name) {
  if (g_off || g_keep <= 0) return;
  {
    std::lock_guard lock(g_lock);
    g_jobs.push_back(name);
  }
  g_wake.notify_all();
}

// ---- the start check ----

fs::path DamagedPlace(const std::string& stamp, const Save& s, const std::string& what) {
  return DamagedRoot() / stamp / s.xuid / s.title / TypeFolder() / FromU8(s.name + what);
}

bool MoveAside(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  fs::create_directories(to.parent_path(), ec);
  fs::rename(from, to, ec);
  if (ec) FCPLOG("save: could not move {} to {}: {}", U8(from), U8(to), ec.message());
  return !ec;
}

// what a cut replacement left (SDK change 330)
void FinishReplacements(const std::string& stamp, bool repair) {
  std::error_code ec;
  for (const fs::path& type : TypeFolders()) {
    std::vector<fs::path> entries;
    for (const auto& e : fs::directory_iterator(type, ec)) entries.push_back(e.path());
    // a complete .ready first: then an .old beside it is the previous state, not the one to put back
    std::stable_partition(entries.begin(), entries.end(), [](const fs::path& p) { return EndsWith(U8(p.filename()), kReady); });
    for (const fs::path& p : entries) {
      const std::string n = U8(p.filename());
      const auto base = [&](const char* suffix) { return n.substr(0, n.size() - std::strlen(suffix)); };
      if (EndsWith(n, kReady)) {
        const Save s = MakeSave(type, base(kReady));
        FCPLOG("save: '{}': a save was cut after it was written, before it took the old one's place{}", s.Id(),
               repair ? "; it takes it now" : "; left as it is");
        if (!repair) continue;
        if (fs::exists(s.dir, ec) && !MoveAside(s.dir, DamagedPlace(stamp, s, " (replaced by the cut save)"))) continue;
        fs::rename(p, s.dir, ec);
        FCPLOG("save: '{}': {}", s.Id(), ec ? "FAILED: " + ec.message() : std::string("the cut save is in place"));
      } else if (EndsWith(n, kPrevious)) {
        const Save s = MakeSave(type, base(kPrevious));
        if (!repair) {
          FCPLOG("save: '{}': the previous state of a cut save is left as it is", s.Id());
          continue;
        }
        if (!fs::exists(s.dir, ec)) {
          fs::rename(p, s.dir, ec);
          FCPLOG("save: '{}': a save was cut while it took the old one's place: the previous state is back{}", s.Id(),
                 ec ? " FAILED: " + ec.message() : std::string());
        } else {
          MoveAside(p, DamagedPlace(stamp, s, " (previous state)"));
          FCPLOG("save: '{}': the previous state left by a finished save moved to {}", s.Id(), U8(DamagedRoot() / stamp));
        }
      } else if (EndsWith(n, kStaging)) {
        const Save s = MakeSave(type, base(kStaging));
        FCPLOG("save: '{}': a save was cut while it was written; the previous save stays{}", s.Id(),
               repair ? ", the unfinished one goes to " + U8(DamagedRoot() / stamp) : std::string(" (left as it is)"));
        if (repair) MoveAside(p, DamagedPlace(stamp, s, " (unfinished save)"));
      }
    }
  }
}

// the newest backup generation that passes the check
std::optional<fs::path> GoodBackup(const Save& s) {
  const std::vector<fs::path> gens = Generations(s);
  for (auto it = gens.rbegin(); it != gens.rend(); ++it) {
    if (CheckContent(*it / "content", *it / s.header.filename(), s.name).ok) return *it;
  }
  return std::nullopt;
}

void Restore(const Save& s, const Check& bad, const std::string& stamp) {
  const std::optional<fs::path> gen = GoodBackup(s);
  if (!gen) {
    FCPLOG("save: '{}' is DAMAGED ({}), no good backup: left as it is", s.Id(), bad.why);
    return;
  }
  std::error_code ec;
  const fs::path keep = DamagedPlace(stamp, s, "");
  if (!MoveAside(s.dir, keep)) return;
  if (!bad.header_ok && fs::exists(s.header, ec)) MoveAside(s.header, keep.wstring() + L".header");
  const fs::path tmp = fs::path(s.dir.wstring() + L".restoring");
  fs::remove_all(tmp, ec);
  bool ok = CopyFiles(*gen / "content", tmp, false);
  if (ok) {
    fs::rename(tmp, s.dir, ec);
    ok = !ec;
  }
  if (ok && !fs::exists(s.header, ec) && fs::exists(*gen / s.header.filename(), ec)) {
    fs::create_directories(s.header.parent_path(), ec);
    ok = CopyFileW((*gen / s.header.filename()).c_str(), s.header.c_str(), FALSE) != 0;
  }
  const Check now = CheckContent(s.dir, s.header, s.name);
  FCPLOG("save: '{}' was DAMAGED ({}): {} from the backup {} ({}); the damaged files are in {}", s.Id(), bad.why,
         ok && now.ok ? "RESTORED" : "could NOT be restored", U8(gen->filename()), Describe(now), U8(keep));
}

// the title settings of the profile (written whole by the SDK): an empty file gets the newest backup's
void CheckProfiles(const std::string& stamp, bool repair) {
  std::error_code ec;
  for (const auto& t : fs::directory_iterator(fcp::paths::Saves(), ec)) {
    if (!t.is_directory(ec) || !IsHex(U8(t.path().filename()), 8)) continue;
    const fs::path profile = t.path() / "profile";
    for (auto it = fs::recursive_directory_iterator(profile, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      std::error_code e2;
      if (!it->is_regular_file(e2) || it->file_size(e2) > 0) continue;
      const fs::path rel = fs::relative(it->path(), profile, e2);
      FCPLOG("save: profile setting {} is EMPTY", U8(it->path()));
      if (!repair) continue;
      // the newest generation of any save of this title that has the file
      fs::path best;
      for (const Save& s : AllSaves()) {
        if (s.title != U8(t.path().filename())) continue;
        for (const fs::path& g : Generations(s)) {
          const fs::path cand = g / "profile" / rel;
          if (fs::file_size(cand, e2) > 0 && !e2 && (best.empty() || g.filename() > best.parent_path().filename()))
            best = cand;
        }
      }
      if (best.empty()) {
        FCPLOG("save: no backup of {}: left as it is", U8(rel));
        continue;
      }
      const fs::path keep = DamagedRoot() / stamp / U8(t.path().filename()) / "profile" / rel;
      fs::create_directories(keep.parent_path(), e2);
      fs::rename(it->path(), keep, e2);
      const bool ok = !e2 && CopyFileW(best.c_str(), it->path().c_str(), FALSE);
      FCPLOG("save: profile setting {} {} from {}", U8(rel), ok ? "RESTORED" : "could NOT be restored", U8(best));
    }
  }
}

// another process that uses the same saves (a second copy, or the parent of a relaunch until it ends)
HANDLE g_instance = nullptr;
bool OtherInstance() {
  const std::string key = fcp::hash::Sha256(Lower(U8(fs::absolute(fcp::paths::Saves()))));
  const std::wstring name = L"Local\\fcp_saves_" + std::wstring(key.begin(), key.begin() + 24);
  g_instance = CreateMutexW(nullptr, FALSE, name.c_str());
  return g_instance && GetLastError() == ERROR_ALREADY_EXISTS;
}

// ---- tests: FCP_TEST_SAVE_END ----

struct TestEnd {
  int n = 0;
  std::string how;
  int ms = 0;
} g_test;

void EndForTest(int save) {
  if (g_test.n == 0 || save != g_test.n) return;
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(g_test.ms));
    FCPLOG("save: FCP_TEST_SAVE_END: {} {} ms after the start of save {}", g_test.how, g_test.ms, g_test.n);
    if (g_test.how == "kill") {
      fcp::log::Drain(std::chrono::milliseconds(500));
      TerminateProcess(GetCurrentProcess(), 9);
    } else if (g_test.how == "menu") {
      fcp::menu::TestActions("open");  // the game goes into its pause menu, then Exit Game (without the question)
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      fcp::menu::TestActions("quit");
    } else if (g_test.how == "restart") {
      fcp::menu::TestActions("restart");  // as Apply (restarts the game) in the F10 menu, without the question
    } else {
      EnumWindows(
          [](HWND w, LPARAM) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId() || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
            PostMessageW(w, WM_CLOSE, 0, 0);
            return FALSE;
          },
          0);
    }
  }).detach();
}

thread_local std::string t_root, t_file;
thread_local uint32_t t_flags = 0, t_type = 0;

}  // namespace

void Startup() {
  g_off = !fcp::env::Flag("FCP_SAVE_GUARD", true);
  g_keep = int(std::clamp<int64_t>(fcp::env::Int("FCP_SAVE_BACKUPS", 3), 0, 100));
  g_exit_wait_ms = std::clamp<int64_t>(fcp::env::Int("FCP_SAVE_EXIT_WAIT_MS", 5000), 0, 60000);
  if (const std::string t = fcp::env::Get("FCP_TEST_SAVE_END");
      !t.empty() && fcp::devstart::ActiveInGeneration("FCP_TEST_SAVE_END_GENS")) {
    g_test.n = std::atoi(t.c_str());
    const size_t a = t.find(':'), b = a == std::string::npos ? a : t.find(':', a + 1);
    g_test.how = a == std::string::npos ? "kill" : Lower(t.substr(a + 1, b == std::string::npos ? b : b - a - 1));
    g_test.ms = b == std::string::npos ? 0 : std::atoi(t.c_str() + b + 1);
    FCPLOG("save: FCP_TEST_SAVE_END: {} {} ms after the start of save {} of this process", g_test.how, g_test.ms,
           g_test.n);
  }
  const auto t0 = std::chrono::steady_clock::now();
  const bool other = OtherInstance();
  const int gen = fcp::relaunch::Generation();
  // a relaunch child: its parent checked at the first start and saved since; the parent may still run
  const bool repair = !g_off && !other && gen == 0;
  const std::string stamp = Stamp();
  if (gen > 0) {
    FCPLOG("save: relaunch generation {}: the saves were checked at the first start", gen);
  } else {
    if (other) FCPLOG("save: another predator.exe uses these saves: checked only, nothing repaired");
    FinishReplacements(stamp, repair);
    const std::vector<Save> saves = AllSaves();
    for (const Save& s : saves) {
      const Check c = CheckContent(s.dir, s.header, s.name);
      if (c.ok) {
        FCPLOG("save: '{}' good ({}{}), {} backups", s.Id(), Describe(c), c.note.empty() ? "" : "; " + c.note,
               Generations(s).size());
      } else if (repair) {
        Restore(s, c, stamp);
      } else {
        FCPLOG("save: '{}' is DAMAGED ({}){}", s.Id(), c.why, g_off ? " (FCP_SAVE_GUARD=0: left as it is)" : "");
      }
    }
    CheckProfiles(stamp, repair);
    // leftovers of backups that a process end cut
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(BackupRoot(), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (it->is_directory(ec) && EndsWith(U8(it->path().filename()), ".tmp")) {
        FCPLOG("save: unfinished backup {} removed", U8(it->path()));
        fs::remove_all(it->path(), ec);
        break;  // the iterator is invalid now; the next start removes another one
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    FCPLOG("save: {} saved game(s) checked in {:.0f} ms in {}; backups {}", saves.size(), ms, U8(fcp::paths::Saves()),
           g_off ? "off (FCP_SAVE_GUARD=0)" : std::format("in {} ({} generations)", U8(BackupRoot()), g_keep));
  }
  if (g_off) {
    FCPLOG("save: FCP_SAVE_GUARD=0: no backups, no repair, no wait for a save at the end, the SDK deletes before it "
           "writes");
    return;
  }
  std::thread(Worker).detach();
  Queue("");  // the state at the start, when no backup has it yet
}

void OnCreate(const std::string& root, uint32_t type, const std::string& file, uint32_t flags) {
  t_root = root, t_file = file, t_flags = flags, t_type = type;
  const uint32_t how = flags & 0xF;
  if (type != kSavedGame || (how != 1 && how != 2 && how != 4 && how != 5)) return;  // reading a save
  int n;
  {
    std::lock_guard lock(g_lock);
    g_open[Lower(root)] = std::chrono::steady_clock::now();
    g_open_file[Lower(root)] = file;
    n = ++g_saves_started;
  }
  FCPLOG("save: '{}' saving (root {}, flags {:08X}, save {} of this process)", file, root, flags, n);
  EndForTest(n);
}

void OnCreateDone(uint32_t result) {
  constexpr uint32_t kIoPending = 0x3E5;
  if (result == 0 || result == kIoPending || t_root.empty()) return;
  std::lock_guard lock(g_lock);
  if (g_open.erase(Lower(t_root))) FCPLOG("save: '{}' could not be opened (error {:08X})", t_file, result);
}

void OnClose(const std::string& root, uint32_t result) {
  std::string file;
  std::chrono::steady_clock::time_point at;
  {
    std::lock_guard lock(g_lock);
    const auto it = g_open.find(Lower(root));
    if (it == g_open.end()) return;
    at = it->second;
    file = g_open_file[Lower(root)];
    g_open.erase(it);
  }
  g_wake.notify_all();
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count();
  if (result != 0) {
    FCPLOG("save: '{}' NOT saved: closing failed (error {:08X}); the previous save stays", file, result);
    return;
  }
  uintmax_t bytes = 0;
  int files = 0;
  for (const Save& s : AllSaves()) {
    if (Lower(s.name) != Lower(file)) continue;
    const Check c = CheckContent(s.dir, s.header, s.name);
    bytes = c.bytes, files = c.files;
  }
  FCPLOG("save: '{}' saved in {:.0f} ms ({} files, {})", file, ms, files, Size(bytes));
  Queue(file);
}

void WaitForSave(const char* why) {
  if (g_off) return;
  const auto t0 = std::chrono::steady_clock::now();
  std::unique_lock lock(g_lock);
  bool waited = false;
  std::string file;
  for (const auto& [root, name] : g_open_file) {
    if (g_open.contains(root)) file = name;
  }
  while (SaveOpen() || g_backup_running || !g_jobs.empty()) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (ms >= g_exit_wait_ms) {
      FCPLOG("save: {}: {} still runs after {} ms, ending anyway{}", why, SaveOpen() ? "a save" : "a backup", ms,
             SaveOpen() ? " (the previous save stays: the new one replaces it only when complete)" : "");
      return;
    }
    waited = true;
    g_wake.wait_for(lock, std::chrono::milliseconds(10));
  }
  if (waited) {
    FCPLOG("save: {}: waited {} ms for {}", why,
           std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(),
           file.empty() ? "the backup" : "the save of '" + file + "' and its backup");
  }
}

}  // namespace fcp::save_guard
