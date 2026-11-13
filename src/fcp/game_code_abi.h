// Interface between predator.exe and predator_code.dll, the recompiled game code.
//
// Plain C types only: the DLL is compiled freestanding (rex/ppc/guest_prelude.h, rexglue/include/rex/guest_std),
// the executable against the MSVC STL. The DLL exports one function, fcp_game_code(), that returns this table.
// Everything else the executable takes from the DLL goes through the delay-load import library of
// predator_code.dll (the originals __imp__<name> of hooked functions); everything the DLL takes from the
// executable through predator.lib (hooks, the kernel import wrappers).
//
// Checks before the executable runs anything of the DLL (src/fcp/game_code.cpp):
//   abi          FCP_GAME_CODE_ABI of the executable: hash of everything that decides the generated code and its
//                compile (codegen sources, SDK headers, title configuration, flags, this header);
//   exports      hash of the export names of the executable the DLL was linked against (the hooks);
//   image_sha256 SHA-256 of the default.xex the code was generated from (config/predator/disc.toml).
#pragma once

#include <stdint.h>

#define FCP_GAME_CODE_INTERFACE 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FcpGameCode {
  uint32_t interface_version;  // FCP_GAME_CODE_INTERFACE
  uint32_t size;               // sizeof(FcpGameCode)
  char abi[68];                // FCP_GAME_CODE_ABI (hex, NUL terminated)
  char exports[68];            // hash of the executable's export names (hex)
  char image_sha256[68];       // SHA-256 of default.xex (hex, upper case)
  char origin[16];             // "build" (CMake) or "first-start"
  const void* image;           // const rex::ppc::PPCImage* (kPPCImageConfig of the generated code)
  const void* func_mappings;   // const PPCFuncMapping* (kPPCFuncMappings, ended by address 0)
  void* fp_stats;              // rex::ppc::fcp_fp_rules::Stats* of the DLL ([fp_rules] slow path counters)
  uint32_t function_count;     // entries in func_mappings
  uint32_t reserved;
} FcpGameCode;

typedef const FcpGameCode* (*FcpGameCodeFunc)(void);

#ifdef __cplusplus
}
#endif
