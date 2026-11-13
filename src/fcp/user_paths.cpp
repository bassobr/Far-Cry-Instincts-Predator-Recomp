#include "fcp/user_paths.h"

#include <windows.h>
#include <shlobj.h>

#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <system_error>
#include <vector>

#include <rex/platform/file.h>

#include "fcp/env.h"
#include "fcp/log.h"

namespace fcp::paths {
namespace {

// Neutral project name: no person or company in the path.
constexpr wchar_t kFolderName[] = L"Far-Cry-Instincts-Predator-Recomp";
constexpr char kPortableMarker[] = "portable.txt";
constexpr char kMigrationMarker[] = "migration.txt";

std::filesystem::path g_root;
Mode g_mode = Mode::kFallback;
std::string g_note;  // why, for LogSummary

std::filesystem::path FromEnv(const char* name) {
  wchar_t buffer[4096];
  const DWORD n = GetEnvironmentVariableW(std::wstring(name, name + std::strlen(name)).c_str(), buffer, 4096);
  if (n == 0 || n >= 4096) return {};
  return std::filesystem::path(std::wstring(buffer, n));
}

std::filesystem::path LocalAppData() {
  std::filesystem::path result;
  PWSTR path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &path))) {
    result.assign(path);
  }
  CoTaskMemFree(path);
  return result;
}

bool MakeFolder(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::create_directories(p, ec);
  return std::filesystem::is_directory(p, ec);
}

bool HasFiles(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return false;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
       !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) return true;
  }
  return false;
}

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
  std::error_code ec;
  if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
    return std::filesystem::equivalent(a, b, ec);
  }
  return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
}

std::string U8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}

}  // namespace

void Configure() {
  const std::filesystem::path exe = rex::platform::GetExecutableFolder();
  if (std::filesystem::path explicit_root = FromEnv("FCP_USER_ROOT"); !explicit_root.empty()) {
    if (explicit_root.is_relative()) explicit_root = exe / explicit_root;
    if (MakeFolder(explicit_root)) {
      g_root = explicit_root;
      g_mode = Mode::kExplicit;
      g_note = "FCP_USER_ROOT";
      return;
    }
    g_note = std::format("FCP_USER_ROOT={} cannot be created; ", U8(explicit_root));
  }
  std::error_code ec;
  if (std::filesystem::exists(exe / kPortableMarker, ec)) {
    g_root = exe;
    g_mode = Mode::kPortable;
    g_note += std::format("{} next to the EXE", kPortableMarker);
    return;
  }
  if (const std::filesystem::path base = LocalAppData(); !base.empty() && MakeFolder(base / kFolderName)) {
    g_root = base / kFolderName;
    g_mode = Mode::kUserFolder;
    g_note += "per-user folder (LOCALAPPDATA)";
    return;
  }
  g_root = exe;
  g_mode = Mode::kFallback;
  g_note += "LOCALAPPDATA not usable, EXE folder";
}

const std::filesystem::path& Root() { return g_root; }
std::filesystem::path Saves() { return g_root / "user"; }
std::filesystem::path Cache() { return g_root / "cache"; }
Mode GetMode() { return g_mode; }
std::string Utf8(const std::filesystem::path& p) { return U8(p); }

const char* ModeName() {
  switch (g_mode) {
    case Mode::kExplicit: return "explicit";
    case Mode::kPortable: return "portable";
    case Mode::kUserFolder: return "user folder";
    case Mode::kFallback: return "fallback";
  }
  return "?";
}

void LogSummary() {
  FCPLOG("user folder: {} ({}: {}); saves {}, cache {}, logs {}", U8(g_root), ModeName(), g_note, U8(Saves()),
         U8(Cache()), U8(g_root));
}

void Migrate() {
  const std::filesystem::path exe = rex::platform::GetExecutableFolder();
  const std::filesystem::path old_saves = exe / "user";
  if (SamePath(g_root, exe)) {
    return;  // portable or the old place itself: nothing to move
  }
  if (!fcp::env::Flag("FCP_USER_MIGRATE", true)) {
    FCPLOG("migration: off (FCP_USER_MIGRATE=0), {} not looked at", U8(old_saves));
    return;
  }
  const std::filesystem::path marker = g_root / kMigrationMarker;
  std::error_code ec;
  if (std::filesystem::exists(marker, ec)) {
    return;  // done before (or decided not to)
  }
  std::string result;
  if (!HasFiles(old_saves)) {
    result = std::format("nothing to copy: no saves in {}", U8(old_saves));
  } else if (HasFiles(Saves())) {
    result = std::format("not copied: {} already has saves (old ones stay in {})", U8(Saves()), U8(old_saves));
  } else {
    const auto t0 = std::chrono::steady_clock::now();
    size_t files = 0, failed = 0;
    uintmax_t bytes = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(old_saves, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      const std::filesystem::path rel = std::filesystem::relative(it->path(), old_saves, ec);
      const std::filesystem::path to = Saves() / rel;
      std::error_code e2;
      if (it->is_directory(e2)) {
        std::filesystem::create_directories(to, e2);
      } else if (it->is_regular_file(e2)) {
        std::filesystem::create_directories(to.parent_path(), e2);
        if (std::filesystem::copy_file(it->path(), to, std::filesystem::copy_options::skip_existing, e2) && !e2) {
          ++files;
          bytes += std::filesystem::file_size(to, e2);
        } else {
          ++failed;
          FCPLOG("migration: copy {} FAILED: {}", U8(it->path()), e2.message());
        }
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    result = std::format("copied {} files ({} bytes, {} failed) from {} to {} in {:.0f} ms; the old files stay",
                         files, bytes, failed, U8(old_saves), U8(Saves()), ms);
  }
  // Keyboard/mouse bindings of the old place, when the new place has none yet.
  const std::filesystem::path old_input = exe / "predator_input.toml", new_input = g_root / "predator_input.toml";
  if (std::filesystem::exists(old_input, ec) && !std::filesystem::exists(new_input, ec)) {
    std::error_code e2;
    std::filesystem::copy_file(old_input, new_input, e2);
    result += e2 ? std::format("; predator_input.toml FAILED: {}", e2.message()) : "; predator_input.toml copied";
  }
  FCPLOG("migration: {}", result);
  std::ofstream out(marker);
  out << "# Written once by predator.exe: saves from the old place next to the EXE (bin\\user).\n"
      << "# Delete this file to look again on the next start (nothing is overwritten).\n"
      << result << "\n";
}

}  // namespace fcp::paths
