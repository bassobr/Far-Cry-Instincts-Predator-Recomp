// What the hook layer (D3D hooks in predator.exe) passes to the native renderer per call.
//
// All structures are plain C layouts (no pointers to host objects except those explicitly named) so the same
// structures pass through the versioned C table NrApi (nr_api.h) and through the C++ interface DrawSink
// (draw_sink.h).
//
// Principles:
// - The renderer reads a draw's state from the device structure (GuestDevice, offsets from
//   config/predator/renderer.toml). The hooks pass only the call's arguments and the device pointer.
// - Guest addresses are 32-bit guest addresses (virtual, as the game holds them). Host pointers exist only
//   for data valid only during the call (DrawVerticesUP) or that the hook already has at hand.
// - Every record carries the raw registers of the call (GuestCall): the argument layout of
//   BeginTiling/EndTiling is not verified for XDK 2.0.2417; the decoded fields are provisional and the
//   renderer can fall back to the raw values until clarified.
// - Copy boundaries for a render thread: whatever a record names by guest address, the renderer reads at
//   execution; before that, the ThreadedSink copies the device blocks and object headers named by the config,
//   and the UP data. Guest data of vertex, index and texture buffers is never copied (valid until the fence).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
namespace fcp::nr {
#endif

// The raw registers of a hook call (for trace, diagnostics and not yet verified arguments).
struct GuestCall {
  uint32_t r[8];      // r3..r10
  double f[4];        // f1..f4
  uint32_t lr;        // return address in the guest
  uint32_t host_thread;
};

// D3DPRIMITIVETYPE of the 360 (Xenos numbering).
enum PrimitiveType : uint32_t {
  kPrimPointList = 1,
  kPrimLineList = 2,
  kPrimLineStrip = 3,
  kPrimTriangleList = 4,
  kPrimTriangleFan = 5,
  kPrimTriangleStrip = 6,
  kPrimRectList = 8,
  kPrimQuadList = 13,
};

enum DrawKind : uint32_t {
  kDrawIndexed = 0,     // D3DDevice_DrawIndexedVertices
  kDrawVertices = 1,    // D3DDevice_DrawVertices
  kDrawVerticesUP = 2,  // D3DDevice_DrawVerticesUP: data in up_data
};

struct DrawRecord {
  uint32_t device;       // guest address of the device structure
  uint32_t frame;        // frame counter of the hook layer (swap to swap)
  uint32_t kind;         // DrawKind
  uint32_t primitive;    // PrimitiveType
  int32_t base_vertex;   // Indexed
  uint32_t start;        // first index (Indexed) or first vertex
  uint32_t count;        // indices or vertices
  // kDrawVerticesUP only: host pointer to the vertex data in guest memory, valid only during the call
  // (the renderer copies it immediately into a per-frame ring), plus guest address, bytes and stride.
  const uint8_t* up_data;
  uint32_t up_guest;
  uint32_t up_bytes;
  uint32_t up_stride;
  struct GuestCall call;
};

// D3DCLEAR flags of the 360: bits 0-3 targets, 0x10 Z, 0x20 stencil (King Kong; verify for Predator by trace).
struct ClearRecord {
  uint32_t device;
  uint32_t frame;
  uint32_t flags;
  uint32_t has_rect;
  int32_t rect[4];       // x1 y1 x2 y2
  float color[4];        // from D3DVECTOR4 (ClearF), RGBA
  float z;
  uint32_t stencil;
  struct GuestCall call;
};

struct ResolveRecord {
  uint32_t device;
  uint32_t frame;
  uint32_t flags;
  uint32_t has_rect;
  int32_t rect[4];
  uint32_t dest_texture;  // guest address of the destination texture (0: none)
  uint32_t has_point;
  int32_t point[2];
  uint32_t dest_level;
  uint32_t dest_slice;
  uint32_t has_clear_color;
  float clear_color[4];
  float clear_z;
  uint32_t clear_stencil;
  uint32_t parameters;    // guest address of D3DRESOLVE_PARAMETERS (0: none)
  struct GuestCall call;
};

// BeginTiling / EndTiling. Layout PROVISIONAL (recalled from later XDK versions, still an open question):
//   BeginTiling(Flags, Count, pTileRects, pClearColor, ClearZ, ClearStencil)
//   EndTiling(ResolveFlags, pResolveRects, pDestTexture, pClearColor, ClearZ, ClearStencil, pParameters)
// decoded = 0: the hooks only filled call.
struct TilingRecord {
  uint32_t device;
  uint32_t frame;
  uint32_t begin;         // 1 BeginTiling, 0 EndTiling
  uint32_t decoded;
  uint32_t flags;         // Begin: Flags, End: ResolveFlags
  uint32_t rect_count;
  uint32_t rects;         // guest address of a D3DRECT array
  uint32_t dest_texture;  // End only
  uint32_t has_clear_color;
  float clear_color[4];
  float clear_z;
  uint32_t clear_stencil;
  uint32_t parameters;    // End only
  struct GuestCall call;
};

struct PredicationRecord {
  uint32_t device;
  uint32_t frame;
  uint32_t mask;
  struct GuestCall call;
};

// Occlusion queries (CreateQuery type 9) and conditional survey / rendering.
enum QueryOp : uint32_t {
  kQueryIssueBegin = 1,  // D3DISSUE_BEGIN (Flags 2)
  kQueryIssueEnd = 2,    // D3DISSUE_END (Flags 1)
  kQueryGetData = 3,
  kQueryRelease = 4,
  kSurveyBegin = 5,
  kSurveyEnd = 6,
  kConditionalBegin = 7,
  kConditionalEnd = 8,
};
enum QueryStatus : uint32_t {
  kQueryNotReady = 0,     // GetData reports S_FALSE, the game asks again later
  kQueryReady = 1,        // samples valid
  kQueryUnsupported = 2,  // the renderer does not know the query: the hooks answer as before
};
struct QueryRecord {
  uint32_t device;
  uint32_t frame;
  uint32_t op;            // QueryOp
  uint32_t query;         // guest address of the query object (survey/conditional: 0)
  uint32_t flags;         // issue/GetData flags or survey index
  struct GuestCall call;
};

enum ShaderKind : uint32_t { kVertexShader = 0, kPixelShader = 1 };
// After the original CreateVertexShader/CreatePixelShader (hook runs afterwards).
struct ShaderCreatedRecord {
  uint32_t kind;             // ShaderKind
  uint32_t object;           // guest address of the shader object returned by the original
  uint32_t container;        // guest address of the container the game passed
  const uint8_t* container_data;  // host pointer to it (size is in the header; max_size bounds the read)
  uint32_t max_size;
  struct GuestCall call;
};

// D3D::LockResource (before the original): the CPU wants to read or write a resource's memory. Which
// register carries the resource is not verified; the renderer checks r3..r6 (call.r) for a texture object
// whose base is a resolve target and then writes the host copy back to guest memory.
struct LockRecord {
  uint32_t frame;
  struct GuestCall call;
};

struct SwapRecord {
  uint32_t device;
  uint32_t frame;
  uint32_t front_buffer;   // guest address of the texture being shown (0: read from the device)
  uint32_t flags;
  struct GuestCall call;
};

#ifdef __cplusplus
}  // namespace fcp::nr
#endif
