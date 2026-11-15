// Test of the save guard's start check, backups and end wait without the game (src/fcp/save_guard.cpp).
//   fcp_save_guard_test.exe <empty work folder>
// Builds a user folder with crafted saved games (good, empty file, unfinished replacement, cut swap, broken header),
// runs Startup, checks what the guard did, then the wait at the end with and without a save in progress.
// Exit code 0 = all checks passed. The log lines go to stdout.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "fcp/log.h"
#include "fcp/save_guard.h"
#include "fcp/user_paths.h"

namespace fs = std::filesystem;

// ---- stubs of the modules the guard uses ----
namespace {
fs::path g_root;
}
namespace fcp::paths {
const fs::path& Root() { return g_root; }
fs::path Saves() { return g_root / "user"; }
std::string Utf8(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
}  // namespace fcp::paths
namespace fcp::log {
void Line(std::string_view text) { std::printf("log: %.*s\n", int(text.size()), text.data()); }
void Drain(std::chrono::milliseconds) { std::fflush(stdout); }
}  // namespace fcp::log
namespace fcp::relaunch {
int Generation() { return 0; }
}  // namespace fcp::relaunch
namespace fcp::devstart {
bool ActiveInGeneration(const char*) { return true; }
}  // namespace fcp::devstart
namespace fcp::menu {
void TestActions(const std::string&) {}
}  // namespace fcp::menu

namespace {

int g_failed = 0;
void Expect(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failed;
}

const char* kXuid = "B13EBABEBABEBABE";
const char* kTitle = "555307DC";

fs::path Type() { return g_root / "user" / kXuid / kTitle / "00000001"; }
fs::path Header(const std::string& name) { return g_root / "user" / kXuid / kTitle / "Headers" / "00000001" / (name + ".header"); }
fs::path Backups(const std::string& name) { return g_root / "save_backup" / kXuid / kTitle / "00000001" / name; }

void Write(const fs::path& p, const std::string& data) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << data;
}

std::string Read(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}

std::string HeaderBytes(const std::string& name, size_t size = 0x148) {
  std::string h(size, '\0');
  h[7] = 1;  // content type 1 (big endian at +4)
  for (size_t i = 0; i < name.size() && i < 42; ++i) h[0x108 + i] = name[i];
  return h;
}

void MakeSave(const fs::path& dir, const std::string& name, const std::string& data) {
  Write(dir / "FC2_Save", data);
  Write(dir / "__thumbnail.png", "png");
  Write(Header(name), HeaderBytes(name));
}

// a backup generation as the guard writes it
void MakeBackup(const std::string& name, const std::string& stamp, const std::string& data) {
  const fs::path g = Backups(name) / stamp;
  Write(g / "content" / "FC2_Save", data);
  Write(g / "content" / "__thumbnail.png", "png");
  Write(g / (name + ".header"), HeaderBytes(name));
}

size_t Count(const fs::path& dir) {
  std::error_code ec;
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir, ec)) n += e.is_directory(ec) ? 1 : 0;
  return n;
}

bool Contains(const fs::path& root, const std::string& part) {
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    if (fcp::paths::Utf8(it->path()).find(part) != std::string::npos) return true;
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return std::printf("usage: fcp_save_guard_test <empty folder>\n"), 2;
  g_root = fs::absolute(argv[1]);
  fs::remove_all(g_root);
  fs::create_directories(g_root);
  SetEnvironmentVariableA("FCP_SAVE_EXIT_WAIT_MS", "300");

  MakeSave(Type() / "IMGOOD", "IMGOOD", "good state");
  // an empty game file, with an older good backup
  MakeSave(Type() / "IMEMPTY", "IMEMPTY", "");
  MakeBackup("IMEMPTY", "20261101-100000-000", "older good state");
  // a save cut while it was written: the old one stays, the unfinished one goes aside
  MakeSave(Type() / "IMCUT", "IMCUT", "previous save");
  Write(Type() / "IMCUT.saving" / "FC2_Save", "half");
  // a save cut after it was written, before the swap (old already moved aside)
  Write(Type() / "IMREADY.ready" / "FC2_Save", "new complete save");
  Write(Type() / "IMREADY.old" / "FC2_Save", "older save");
  Write(Header("IMREADY"), HeaderBytes("IMREADY"));
  // a save cut while the old one was moved aside: the previous state comes back
  Write(Type() / "IMOLD.old" / "FC2_Save", "previous state");
  Write(Header("IMOLD"), HeaderBytes("IMOLD"));
  // a truncated header, with a good backup
  MakeSave(Type() / "IMHEAD", "IMHEAD", "head state");
  Write(Header("IMHEAD"), HeaderBytes("IMHEAD", 100));
  MakeBackup("IMHEAD", "20261101-100000-000", "head state");
  // an empty profile setting with a backup of it
  Write(g_root / "user" / kTitle / "profile" / "User" / "63E83FFF", "");
  Write(Backups("IMHEAD") / "20261101-100000-000" / "profile" / "User" / "63E83FFF", "profile bytes");

  fcp::save_guard::Startup();
  fcp::save_guard::WaitForSave("test: backups after the start");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  fcp::save_guard::WaitForSave("test: backups after the start");

  Expect(Read(Type() / "IMGOOD" / "FC2_Save") == "good state", "good save untouched");
  Expect(Count(Backups("IMGOOD")) == 1, "good save backed up once");
  Expect(Read(Type() / "IMEMPTY" / "FC2_Save") == "older good state", "empty file: backup restored");
  Expect(Contains(g_root / "save_damaged", "IMEMPTY"), "empty file: damaged save kept in save_damaged");
  Expect(Read(Type() / "IMCUT" / "FC2_Save") == "previous save", "cut while written: previous save stays");
  Expect(!fs::exists(Type() / "IMCUT.saving") && Contains(g_root / "save_damaged", "IMCUT (unfinished save)"),
         "cut while written: unfinished save moved aside");
  Expect(Read(Type() / "IMREADY" / "FC2_Save") == "new complete save", "cut before the swap: new save in place");
  Expect(!fs::exists(Type() / "IMREADY.ready") && !fs::exists(Type() / "IMREADY.old"), "cut before the swap: no leftovers");
  Expect(Read(Type() / "IMOLD" / "FC2_Save") == "previous state", "cut in the swap: previous state back");
  Expect(fs::file_size(Header("IMHEAD")) == 0x148, "broken header: header restored");
  Expect(Read(g_root / "user" / kTitle / "profile" / "User" / "63E83FFF") == "profile bytes", "empty profile setting restored");

  // a second backup only when the save changed; at most 3 generations
  for (int i = 0; i < 4; ++i) {
    fcp::save_guard::OnCreate("save", 1, "IMGOOD", 2);
    fcp::save_guard::OnCreateDone(0);
    Write(Type() / "IMGOOD" / "FC2_Save", "state " + std::to_string(i));
    fcp::save_guard::OnClose("save", 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    fcp::save_guard::WaitForSave("test: after a save");
  }
  Expect(Count(Backups("IMGOOD")) == 3, "at most 3 backup generations");

  // the end waits for a save in progress, at most FCP_SAVE_EXIT_WAIT_MS
  fcp::save_guard::OnCreate("save", 1, "IMGOOD", 2);
  auto t0 = std::chrono::steady_clock::now();
  fcp::save_guard::WaitForSave("test: save never closes");
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  Expect(waited >= 290 && waited < 1500, "end waits at most the limit (" + std::to_string(waited) + " ms)");
  std::thread closer([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fcp::save_guard::OnClose("save", 0);
  });
  t0 = std::chrono::steady_clock::now();
  SetEnvironmentVariableA("FCP_SAVE_EXIT_WAIT_MS", nullptr);
  fcp::save_guard::OnCreate("save", 1, "IMGOOD", 2);
  fcp::save_guard::WaitForSave("test: save closes after 100 ms");
  closer.join();
  const auto waited2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  Expect(waited2 >= 90 && waited2 < 290, "end waits until the save is done (" + std::to_string(waited2) + " ms)");
  // reading a save (OPEN_EXISTING) is no save in progress
  fcp::save_guard::OnCreate("load", 1, "IMGOOD", 3);
  t0 = std::chrono::steady_clock::now();
  fcp::save_guard::WaitForSave("test: only reading");
  Expect(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50), "reading a save does not hold the end");

  std::printf("%s: %d failed\n", g_failed ? "FAILED" : "passed", g_failed);
  return g_failed ? 1 : 0;
}
