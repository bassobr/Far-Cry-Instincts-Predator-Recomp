// NrApi: the versioned C table between the D3D hooks in predator.exe and the renderer. The hooks hold only
// this table; whether the renderer itself, a render thread or a trace decorator sits behind it is invisible to
// them. The records are defined in nr_types.h.
//
// Versioning rule: only append new entries at the end and bump kNrApiVersion; the hooks check `size` and
// `version` and call nothing the table does not have (entry null = not present).
//
// Hooking: the hook layer obtains the table with fcp::nr::GetNrApi() (set by the renderer or GPU plugin with
// SetNrApi; until then null = the originals run), calls the entry for each replaced function and then does NOT run the original (draw, clear, resolve,
// tiling, predication, query issue/GetData), or calls the entry after the original
// (CreateVertexShader/CreatePixelShader, Release). Every hook has a fallback switch FCP_D3D_HOOK_<NAME>=0.
#pragma once

#include <stdint.h>

#include "renderer/nr_types.h"

#ifdef __cplusplus
extern "C" {
namespace fcp::nr {
#endif

#define FCP_NR_API_VERSION 2u

struct NrApi {
  uint32_t size;       // sizeof(NrApi) of the creator
  uint32_t version;    // FCP_NR_API_VERSION of the creator
  void* context;       // first argument of every entry
  void (*device_created)(void* context, uint32_t device);
  void (*draw)(void* context, const DrawRecord* record);
  void (*clear)(void* context, const ClearRecord* record);
  void (*resolve)(void* context, const ResolveRecord* record);
  void (*tiling)(void* context, const TilingRecord* record);
  void (*predication)(void* context, const PredicationRecord* record);
  uint32_t (*query)(void* context, const QueryRecord* record, uint32_t* samples);  // QueryStatus
  void (*shader_created)(void* context, const ShaderCreatedRecord* record);
  void (*shader_released)(void* context, uint32_t kind, uint32_t object);
  void (*swap)(void* context, const SwapRecord* record);
  // Version 2: D3D::LockResource (reading back resolve targets for CPU copies).
  void (*lock)(void* context, const LockRecord* record);
};

#ifdef __cplusplus
}  // namespace fcp::nr
}  // extern "C"

namespace fcp::nr {
class DrawSink;
// A table that forwards every entry to `sink` (sink must outlive the table).
NrApi MakeNrApi(DrawSink& sink);
// The process-wide table the hooks use; SetNrApi sets it (null: all entries empty).
const NrApi* GetNrApi();
void SetNrApi(const NrApi* api);
}  // namespace fcp::nr
#endif
