// predator_code.dll: the table the executable reads (src/fcp/game_code_abi.h). Compiled with the generated code,
// freestanding (no C or C++ runtime; runtime.cpp has the two symbols it would give), by the first-start build on
// the player's PC and by the development build (src/fcp/game_code_build.cpp). The DLL has no entry point: nothing
// here needs initialising at run time (checked by the build: no .CRT and no .tls section).
//
// game_code_config.h, written by the build into its work folder, defines
//   FCP_GAME_CODE_ABI      the executable's ABI hash          FCP_GAME_CODE_EXPORTS  hash of its export names
//   FCP_GAME_CODE_IMAGE    SHA-256 of default.xex             FCP_GAME_CODE_ORIGIN   "build" or "first-start"
#include "predator_pch.h"

#include <rex/ppc/fcp_fp_rules.h>

#include "fcp/game_code_abi.h"
#include "game_code_config.h"

namespace {

FcpGameCode g_info = {
    FCP_GAME_CODE_INTERFACE,
    sizeof(FcpGameCode),
    FCP_GAME_CODE_ABI,
    FCP_GAME_CODE_EXPORTS,
    FCP_GAME_CODE_IMAGE,
    FCP_GAME_CODE_ORIGIN,
    &kPPCImageConfig,
    kPPCFuncMappings,
    &rex::ppc::fcp_fp_rules::g_stats,
    0,
    0,
};

}  // namespace

extern "C" __declspec(dllexport) const FcpGameCode* fcp_game_code() {
  if (g_info.function_count == 0) {
    uint32_t n = 0;
    while (kPPCFuncMappings[n].address != 0) {
      ++n;
    }
    g_info.function_count = n;
  }
  return &g_info;
}
