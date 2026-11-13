// The recompiled game code in predator_code.dll.
//
// predator.exe carries no game code. Before the app starts, Prepare() finds the DLL, checks that it fits this
// executable (FCP_GAME_CODE_ABI, the hash of the executable's exports) and the disc image (SHA-256 of default.xex,
// config/predator/disc.toml), loads it and resolves the executable's delay-load imports from it (the originals
// __imp__<name> the hooks call). Without a fitting DLL it builds one from the player's disc image
// (src/fcp/game_code_build.cpp), behind a progress window, and then the shader pack (src/fcp/first_start.cpp).
//
// Where the DLL lives (first match that fits wins; the build writes the first):
//   FCP_GAME_CODE_DLL=<path>                 explicit file (tests)
//   <user folder>\game_code\predator_code.dll  (portable: next to the EXE; fcp/user_paths.h)
//   <EXE folder>\game_code\predator_code.dll   (the development build's DLL, when the user folder is elsewhere)
// Switches: FCP_GAME_CODE_REBUILD=1 builds even when a DLL fits; FCP_GAME_CODE_BUILD=0 never builds (fails
// instead); the build's own switches are listed in game_code_build.h.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "fcp/game_code_abi.h"

namespace rex::ppc {
struct PPCImage;
}
struct PPCFuncMapping;

namespace fcp::game_code {

/** Loads (and when needed builds) the game code. Null on failure; the player was told why. After the log started. */
const rex::ppc::PPCImage* Prepare();

/** The table of the loaded DLL; null before Prepare or in a build with the game code in the EXE. */
const FcpGameCode* Info();

/** The guest function table of the loaded game code (kPPCFuncMappings). */
const PPCFuncMapping* FuncMappings();

/** Host address range of the recompiled code (section rexguest of the module that holds it). */
void CodeRange(uintptr_t& begin, uintptr_t& end);

/** [fp_rules] counters of the game code (rex::ppc::fcp_fp_rules::Stats). */
void* FpStats();

/** SHA-256 (hex, upper case) of the export names of this executable; what the DLL was linked against. */
std::string ExportsHash();

/** Export names of this executable (sorted). */
std::vector<std::string> ExportNames();

/** Names this executable imports from predator_code.dll through its delay-load table (sorted). */
std::vector<std::string> DelayImportNames();

/** FCP_GAME_CODE_ABI of this executable. */
const char* AbiHash();

/** SHA-256 (hex, upper case) of default.xex the game code is for (config/predator/disc.toml). */
const char* ExpectedImageSha256();

/** Size in bytes of that default.xex. */
uint64_t ExpectedImageSize();

/** The game code folder in the user folder (<root>\game_code). */
std::filesystem::path Folder();

}  // namespace fcp::game_code
