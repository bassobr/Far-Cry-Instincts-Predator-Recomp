// Logging D3D hooks (renderer milestone M1): every hook logs and then runs the original.
//
// The names are symbols of the generated code. They come from config/predator/d3d_symbols.toml via the
// codegen table [function_names] (cmake/fcp_d3d_names.cmake); no address appears in C++. A name that
// is missing from the configuration fails the link.
//
// Switches (read once):
//   FCP_TRACE_D3D=<frame>[,<count>]  every hooked call of frames [frame, frame+count) with r3-r10, f1, lr
//   FCP_DUMP_DEVICE=<n>[,<n>...]     the 20,608 bytes of the device at swap n (0 = after the first Swap
//                                    entry) into <log>/device_<n>.bin
//   FCP_DUMP_SHADERS=1               every container through Create{Vertex,Pixel}Shader into
//                                    <log>/shaders/<hash>.{vs,ps} (out/ is gitignored); hash, size and
//                                    magic are always logged
//   FCP_D3D_HOOK_<NAME>=0            hook passes straight to the original (no counting, no log)
//   FCP_D3D_LOG_FIRST=<n>            per hook, the first n calls are always logged (default 3)
#pragma once

// clang-format off
#define FCP_D3D_HOOK_LIST(X)                       \
  /* seam: candidates for replacement (M2+) */     \
  X(D3DDevice_DrawIndexedVertices)                 \
  X(D3DDevice_DrawVertices)                        \
  X(D3DDevice_DrawVerticesUP)                      \
  X(D3DDevice_ClearF)                              \
  X(D3DDevice_Resolve)                             \
  X(D3DDevice_BeginTiling)                         \
  X(D3DDevice_EndTiling)                           \
  X(D3DDevice_SetPredication)                      \
  X(D3DDevice_Swap)                                \
  X(D3DDevice_SynchronizeToPresentationInterval)   \
  X(D3DQuery_Issue)                                \
  X(D3DQuery_GetData)                              \
  X(D3DDevice_BeginConditionalSurvey)              \
  X(D3DDevice_EndConditionalSurvey)                \
  X(D3DDevice_BeginConditionalRendering)           \
  X(D3DDevice_EndConditionalRendering)             \
  X(D3DDevice_InsertCallback)                      \
  X(D3DDevice_InsertFence)                         \
  X(D3DDevice_BlockUntilIdle)                      \
  X(D3DResource_BlockUntilNotBusy)                 \
  X(D3D_BlockOnFence)                              \
  X(D3D_AdvanceCommandBufferSegment)               \
  X(D3DDevice_SetClipPlane)                        \
  X(D3DDevice_SetGammaRamp)                        \
  X(D3DDevice_PersistDisplay)                      \
  X(D3DDevice_Suspend)                             \
  X(D3DDevice_Resume)                              \
  X(D3D_LockResource)                              \
  /* create and release */                         \
  X(Direct3D_CreateDevice)                         \
  X(D3D_InitializeDevice)                          \
  X(D3DDevice_CreateTexture)                       \
  X(D3DDevice_CreateRenderTarget)                  \
  X(D3DDevice_CreateVertexBuffer)                  \
  X(D3DDevice_CreateIndexBuffer)                   \
  X(D3DDevice_CreateVertexShader)                  \
  X(D3DDevice_CreatePixelShader)                   \
  X(D3DDevice_CreateVertexDeclaration)             \
  X(D3DDevice_CreateQuery)                         \
  X(D3DResource_Release)                           \
  X(D3DVertexShader_Release)                       \
  X(D3DPixelShader_Release)                        \
  X(D3DVertexDeclaration_Release)                  \
  X(D3DQuery_Release)                              \
  X(D3DDevice_Release)                             \
  /* name checks and threads */                    \
  X(D3DDevice_SetVertexShader)                     \
  X(D3DDevice_SetPixelShader)                      \
  X(D3D_CreateWorkerThread)                        \
  X(D3D_SwapWorkerThread)
// clang-format on

namespace fcp::d3d {

/** Reads the switches. Call before the title starts. */
void Configure();

/** Writes the per hook call counts and the name checks into the log. */
void WriteSummary();

}  // namespace fcp::d3d
