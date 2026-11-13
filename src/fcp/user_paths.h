// User folder: where saves, profiles, caches, logs and the settings files live.
//
// Decided once at start, before the log opens:
//   1. FCP_USER_ROOT=<path>            explicit folder (tests, the old place: FCP_USER_ROOT=<folder of predator.exe>)
//   2. portable.txt next to the EXE    the EXE folder itself (the development builds write this marker)
//   3. otherwise                       %LOCALAPPDATA%\Far-Cry-Instincts-Predator-Recomp (under Proton the prefix's
//                                      AppData\Local); the EXE folder when that folder cannot be created
// Layout below the root (the same as the old layout next to the EXE):
//   predator.log, predator.gen<n>.log, logs\   log, relaunch logs, dumps and traces
//   predator.toml, predator_input.toml          settings, keyboard/mouse
//   user\                                       saves and profiles (SDK app.user_root)
//   cache\                                      shader, pipeline and HLSL caches
#pragma once

#include <filesystem>
#include <string>

namespace fcp::paths {

enum class Mode { kExplicit, kPortable, kUserFolder, kFallback };

/** Decides the root and creates it. Call first in OnConfigure (no log yet). */
void Configure();

const std::filesystem::path& Root();
std::filesystem::path Saves();  // Root()/user
std::filesystem::path Cache();  // Root()/cache
Mode GetMode();
std::string Utf8(const std::filesystem::path& p);  // for logs and SDK settings
const char* ModeName();

/** Logs the decision (after the log started). */
void LogSummary();

/** Copies saves (and predator_input.toml) from the old place next to the EXE once, never deletes.
    FCP_USER_MIGRATE=0: off. The marker Root()/migration.txt records the result. */
void Migrate();

}  // namespace fcp::paths
