#include "fcp/d3d_hooks.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/hash.h>
#include <rex/ppc/hook.h>

#include "fcp/env.h"
#include "fcp/guest.h"
#include "fcp/log.h"
#include "fcp/render_bridge.h"
#include "fcp/watchdog.h"
#include "fcp_d3d_config.h"
#include "renderer/nr_api.h"

namespace fcp::d3d {
namespace {

enum Id : int {
#define FCP_X(name) k_##name,
  FCP_D3D_HOOK_LIST(FCP_X)
#undef FCP_X
      kHookCount
};

constexpr std::array<const char*, kHookCount> kNames = {
#define FCP_X(name) #name,
    FCP_D3D_HOOK_LIST(FCP_X)
#undef FCP_X
};

struct HookState {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> frame_calls{0};
  bool enabled = true;
};
std::array<HookState, kHookCount> g_hooks;

uint64_t g_trace_first = UINT64_MAX;  // FCP_TRACE_D3D frame
uint64_t g_trace_count = 1;
std::vector<uint64_t> g_dump_frames;  // FCP_DUMP_DEVICE
bool g_dump_shaders = false;
uint64_t g_log_first = 3;
std::atomic<uint32_t> g_device{0};

// Shader objects by kind, for the name check of Set*/Release (vertex vs pixel).
std::mutex g_shader_mutex;
std::unordered_map<uint32_t, char> g_shader_kind;  // object -> 'V' / 'P'
struct Check {
  std::atomic<uint64_t> match{0}, mismatch{0}, unknown{0};
};
Check g_check_set_vs, g_check_set_ps, g_check_rel_vs, g_check_rel_ps;
std::atomic<uint64_t> g_shaders_created[2];  // [0] vertex, [1] pixel
std::atomic<uint64_t> g_shaders_bad_magic{0};

bool InTrace() {
  const uint64_t f = fcp::watch::SwapCount();
  return f >= g_trace_first && f < g_trace_first + g_trace_count;
}

// Argument registers at entry (a full PPCContext copy per call would cost 1.5 KB).
struct Args {
  uint32_t r3, r4, r5, r6, r7, r8, r9, r10, lr;
  double f1;
  static Args From(const PPCContext& c) {
    return {c.r3.u32, c.r4.u32, c.r5.u32, c.r6.u32, c.r7.u32, c.r8.u32,
            c.r9.u32, c.r10.u32, static_cast<uint32_t>(c.lr), c.f1.f64};
  }
};

std::string Regs(const Args& c) {
  return std::format("r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} r9={:08X} r10={:08X} "
                     "f1={:g} lr={:08X}",
                     c.r3, c.r4, c.r5, c.r6, c.r7, c.r8, c.r9, c.r10, c.f1, c.lr);
}

std::string Rect(uint8_t* base, uint32_t address) {
  if (!fcp::guest::Readable(base, address, 16)) {
    return address ? std::format("?{:08X}", address) : "null";
  }
  auto i = [&](uint32_t off) { return static_cast<int32_t>(fcp::guest::U32(base, address + off)); };
  return std::format("({},{})-({},{})", i(0), i(4), i(8), i(12));
}

std::string Vec4(uint8_t* base, uint32_t address) {
  if (!fcp::guest::Readable(base, address, 16)) {
    return address ? std::format("?{:08X}", address) : "null";
  }
  auto f = [&](uint32_t off) {
    const uint32_t bits = fcp::guest::U32(base, address + off);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
  };
  return std::format("({:g},{:g},{:g},{:g})", f(0), f(4), f(8), f(12));
}

const char* MultiSample(uint32_t type) {
  switch (type) {
    case 0:
      return "NONE";
    case 1:
      return "2_SAMPLES";
    case 2:
      return "4_SAMPLES";
    default:
      return "?";
  }
}

// Decoded arguments of the functions the open questions of the D3D map are about. The register
// assignment follows the XDK signatures (device in r3, floats in f1).
std::string Detail(int id, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6, uint32_t r7, uint32_t r8,
                   double f1, uint8_t* base) {
  if (id == k_D3DDevice_BeginTiling) {
    std::string rects;
    for (uint32_t i = 0; i < r5 && i < 8; ++i) {
      rects += " " + Rect(base, r6 + 16 * i);
    }
    return std::format(" | flags={:X} tiles={}:{} color={} z={:g} stencil={:X}", r4, r5, rects,
                       Vec4(base, r7), f1, r8);
  }
  if (id == k_D3DDevice_EndTiling) {
    return std::format(" | flags={:X} rect={} dest={:08X} color={}", r4, Rect(base, r5), r6,
                       Vec4(base, r7));
  }
  if (id == k_D3DDevice_Resolve) {
    std::string point = "null";
    if (fcp::guest::Readable(base, r7, 8)) {
      point = std::format("({},{})", static_cast<int32_t>(fcp::guest::U32(base, r7)),
                          static_cast<int32_t>(fcp::guest::U32(base, r7 + 4)));
    }
    return std::format(" | flags={:X} src={} dest={:08X} point={} level={}", r4, Rect(base, r5), r6,
                       point, r8);
  }
  if (id == k_D3DDevice_ClearF) {
    return std::format(" | flags={:X} rect={} color={} z={:g}", r4, Rect(base, r5), Vec4(base, r6), f1);
  }
  if (id == k_D3DDevice_SetPredication) {
    return std::format(" | mask={:X}", r4);
  }
  if (id == k_D3DDevice_CreateRenderTarget) {
    return std::format(" | {}x{} format={:08X} msaa={} ({})", r3, r4, r5, r6, MultiSample(r6));
  }
  if (id == k_D3DDevice_CreateTexture) {
    return std::format(" | {}x{} levels={} format={:08X}", r3, r4, r6, r8);
  }
  return {};
}

void DumpDevice(uint8_t* base, uint64_t frame) {
  const uint32_t device = g_device.load();
  if (!device || !base) {
    FCPLOG("d3d device dump at frame {} skipped: device unknown", frame);
    return;
  }
  const auto path = fcp::log::Folder() / std::format("device_{}.bin", frame);
  if (FILE* f = _wfopen(path.c_str(), L"wb")) {
    std::fwrite(fcp::guest::Ptr(base, device), 1, kDeviceSize, f);
    std::fclose(f);
    FCPLOG("d3d device dump: {:08X}, {} bytes, frame {} -> {}", device, kDeviceSize, frame,
           path.filename().string());
  }
}

// Shader containers 0x102A0E00 (pixel) / 0x102A0E01 (vertex): size = virtualSize + physicalSize.
void OnShaderCreate(Id id, const Args& entry, const PPCContext& after, uint8_t* base) {
  const bool vertex = id == k_D3DDevice_CreateVertexShader;
  const uint32_t want = vertex ? 0x102A0E01u : 0x102A0E00u;
  int reg = -1;
  uint32_t container = 0;
  const uint32_t candidates[3] = {entry.r3, entry.r4, entry.r5};
  for (int i = 0; i < 3; ++i) {
    const uint32_t a = candidates[i];
    if ((a & 3) == 0 && fcp::guest::Readable(base, a, 12)) {
      const uint32_t magic = fcp::guest::U32(base, a);
      if ((magic & 0xFFFFFFFEu) == 0x102A0E00u) {
        reg = 3 + i;
        container = a;
        break;
      }
    }
  }
  const uint32_t object = after.r3.u32;
  if (reg < 0) {
    g_shaders_bad_magic.fetch_add(1);
    FCPLOG("d3d shader {}: no container 0x102A0E0x in r3-r5 ({}) -> {:08X}", kNames[id], Regs(entry),
           object);
    return;
  }
  const uint32_t magic = fcp::guest::U32(base, container);
  const uint32_t virtual_size = fcp::guest::U32(base, container + 4);
  const uint32_t physical_size = fcp::guest::U32(base, container + 8);
  const uint64_t size = uint64_t(virtual_size) + physical_size;
  uint64_t hash = 0;
  if (size >= 12 && size <= (4u << 20)) {
    hash = rex::HashXXH3(std::span<const uint8_t>(fcp::guest::Ptr(base, container), size));
  }
  const uint64_t n = g_shaders_created[vertex ? 0 : 1].fetch_add(1) + 1;
  FCPLOG("d3d shader {} #{}: container r{}={:08X} magic={:08X}{} size={} (v={} p={}) xxh3={:016X} "
         "-> object {:08X} lr={:08X}",
         vertex ? "VS" : "PS", n, reg, container, magic, magic == want ? "" : " [KIND MISMATCH]", size,
         virtual_size, physical_size, hash, object, entry.lr);
  if (object) {
    std::lock_guard lock(g_shader_mutex);
    g_shader_kind[object] = (magic & 1) ? 'V' : 'P';
  }
  // Native renderer (M2): the container goes to the ShaderLibrary (pack, disk cache or translation).
  if (const nr::NrApi* api = nr::GetNrApi(); api && api->shader_created && object && hash) {
    nr::ShaderCreatedRecord r{};
    r.kind = (magic & 1) ? nr::kVertexShader : nr::kPixelShader;
    r.object = object;
    r.container = container;
    r.container_data = fcp::guest::Ptr(base, container);
    r.max_size = static_cast<uint32_t>(size);
    api->shader_created(api->context, &r);
  }
  if (g_dump_shaders && hash && size <= (4u << 20)) {
    const auto folder = fcp::log::Folder() / "shaders";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto path = folder / std::format("{:016X}.{}", hash, (magic & 1) ? "vs" : "ps");
    if (!std::filesystem::exists(path, ec)) {
      if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        std::fwrite(fcp::guest::Ptr(base, container), 1, size, f);
        std::fclose(f);
      }
    }
  }
}

void CheckKind(Check& check, uint32_t object, char want) {
  if (!object) {
    return;
  }
  std::lock_guard lock(g_shader_mutex);
  const auto it = g_shader_kind.find(object);
  if (it == g_shader_kind.end()) {
    check.unknown.fetch_add(1);
  } else if (it->second == want) {
    check.match.fetch_add(1);
  } else {
    check.mismatch.fetch_add(1);
  }
}

// Shader argument of Set*Shader: the first register that names a known shader object (r4, then r3).
uint32_t ShaderArg(const PPCContext& c) {
  std::lock_guard lock(g_shader_mutex);
  if (g_shader_kind.contains(c.r4.u32)) {
    return c.r4.u32;
  }
  if (g_shader_kind.contains(c.r3.u32)) {
    return c.r3.u32;
  }
  return c.r4.u32;
}

struct Call {
  Id id;
  Args entry;
  uint64_t n;
  bool log;
};

// Low frequency functions: always logged with arguments and result.
bool AlwaysLog(Id id) {
  switch (id) {
    case k_Direct3D_CreateDevice:
    case k_D3D_InitializeDevice:
    case k_D3DDevice_CreateRenderTarget:
    case k_D3DDevice_Suspend:
    case k_D3DDevice_Resume:
    case k_D3DDevice_PersistDisplay:
    case k_D3DDevice_Release:
    case k_D3D_CreateWorkerThread:
    case k_D3D_SwapWorkerThread:
      return true;
    default:
      return false;
  }
}

void Enter(Call& call, PPCContext& ctx, uint8_t* base) {
  fcp::watch::SetGuestBase(base);
  HookState& h = g_hooks[call.id];
  call.n = h.calls.fetch_add(1, std::memory_order_relaxed) + 1;
  h.frame_calls.fetch_add(1, std::memory_order_relaxed);
  const bool pass_call = call.id == k_D3DDevice_BeginTiling || call.id == k_D3DDevice_EndTiling ||
                         call.id == k_D3DDevice_Resolve || call.id == k_D3DDevice_ClearF;
  call.log = call.n <= g_log_first || AlwaysLog(call.id) || InTrace() || (pass_call && call.n <= 12);
  if (call.id == k_D3DDevice_Swap && ctx.r3.u32 && g_device.load() != ctx.r3.u32) {
    g_device.store(ctx.r3.u32);
    FCPLOG("d3d device (from Swap r3): {:08X}", ctx.r3.u32);
    for (uint64_t f : g_dump_frames) {
      if (f == 0) {
        DumpDevice(base, 0);
      }
    }
  }
  if (call.id == k_D3DDevice_SetVertexShader || call.id == k_D3DDevice_SetPixelShader ||
      call.id == k_D3DVertexShader_Release || call.id == k_D3DPixelShader_Release) {
    const bool vs = call.id == k_D3DDevice_SetVertexShader || call.id == k_D3DVertexShader_Release;
    const bool set = call.id == k_D3DDevice_SetVertexShader || call.id == k_D3DDevice_SetPixelShader;
    if (call.n <= 200000) {
      const uint32_t object = set ? ShaderArg(ctx) : ctx.r3.u32;
      CheckKind(set ? (vs ? g_check_set_vs : g_check_set_ps) : (vs ? g_check_rel_vs : g_check_rel_ps),
                object, vs ? 'V' : 'P');
    }
  }
  if (call.log) {
    const Args& e = call.entry;
    FCPLOG("d3d > {} #{} f{} {}{}", kNames[call.id], call.n, fcp::watch::SwapCount(), Regs(e),
           Detail(call.id, e.r3, e.r4, e.r5, e.r6, e.r7, e.r8, e.f1, base));
  }
}

void Leave(Call& call, PPCContext& ctx, uint8_t* base) {
  if (call.id == k_D3DDevice_CreateVertexShader || call.id == k_D3DDevice_CreatePixelShader) {
    OnShaderCreate(call.id, call.entry, ctx, base);
  }
  if (call.log) {
    FCPLOG("d3d < {} #{} -> r3={:08X} f1={:g}", kNames[call.id], call.n, ctx.r3.u32, ctx.f1.f64);
  }
  if (call.id == k_D3DDevice_Swap) {
    fcp::watch::OnSwap();
    const uint64_t frame = fcp::watch::SwapCount();
    if (frame >= g_trace_first && frame <= g_trace_first + g_trace_count) {
      std::string counts;
      for (int i = 0; i < kHookCount; ++i) {
        const uint64_t c = g_hooks[i].frame_calls.load();
        if (c) {
          counts += std::format(" {}={}", kNames[i], c);
        }
      }
      FCPLOG("d3d frame {} calls:{}", frame - 1, counts);
    }
    for (HookState& h : g_hooks) {
      h.frame_calls.store(0, std::memory_order_relaxed);
    }
    for (uint64_t f : g_dump_frames) {
      if (f != 0 && f == frame) {
        DumpDevice(base, frame);
      }
    }
  }
}

// --- native renderer (NrApi, src/fcp/render_bridge.h) ------------------------------------------------
// Only the outermost D3D call of a thread goes to the renderer: the library's own calls from inside a
// hooked function (EndTiling resolves each tile, sets the predication) are its business, the renderer
// replaces the whole function. The originals still run after the renderer (observe
// mode): the ring is filled and the null driver drains it.
thread_local int t_depth = 0;
thread_local int t_outer = -1;  // Id of the outermost hooked call of this thread
// BeginTiling clears the tile surfaces itself with a nested ClearF (flags 0x3F, rect of one tile; measured in
// 00_training, lr inside the library). Without it the main pass keeps last frame's colour and depth.
// FCP_RENDER_TILING_CLEAR=0 drops it again (state before 2026-11-12).
bool g_forward_tiling_clear = true;
// FCP_RENDER_QUERY=visible: D3DQuery_GetData answers every occlusion query with "ready, FCP_RENDER_QUERY_SAMPLES
// samples" (default 0x4000) after the original ran, the safe value until host queries exist. Default: the
// original's answer (null driver), as in the boot.
bool g_query_visible = false;
uint32_t g_query_samples = 0x4000;
std::atomic<uint64_t> g_query_overrides{0};
std::atomic<uint64_t> g_query_not_ready{0};
uint64_t g_query_ab = 0;

nr::GuestCall MakeCall(const Args& a) {
  nr::GuestCall c{};
  const uint32_t r[8] = {a.r3, a.r4, a.r5, a.r6, a.r7, a.r8, a.r9, a.r10};
  std::memcpy(c.r, r, sizeof(r));
  c.f[0] = a.f1;
  c.lr = a.lr;
  c.host_thread = GetCurrentThreadId();
  return c;
}

// Hot path check of guest pointers the original reads as well (UP vertex data, rects, colours): a range
// check like the original's own use, no VirtualQuery per call (measured in the frame rate audit: VirtualQuery
// took 72-88 % of the render thread in 21_dunea).
// FCP_D3D_CHECK_READABLE=1 restores the VirtualQuery check.
bool g_check_readable = false;
inline bool HotReadable(uint8_t* base, uint32_t address, uint32_t size) {
  if (g_check_readable) return fcp::guest::Readable(base, address, size);
  return size && fcp::guest::Plausible(address) && fcp::guest::Plausible(address + size - 1) && address + size > address;
}

bool ReadRect(uint8_t* base, uint32_t address, int32_t out[4]) {
  if (!address || !HotReadable(base, address, 16)) return false;
  for (int i = 0; i < 4; ++i) out[i] = static_cast<int32_t>(fcp::guest::U32(base, address + 4 * i));
  return true;
}

bool ReadVec4(uint8_t* base, uint32_t address, float out[4]) {
  if (!address || !HotReadable(base, address, 16)) return false;
  for (int i = 0; i < 4; ++i) {
    const uint32_t bits = fcp::guest::U32(base, address + 4 * i);
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

// Before the original: draws, clear, resolve, tiling, predication, swap.
void ForwardBefore(Id id, const Args& a, uint8_t* base) {
  const nr::NrApi* api = nr::GetNrApi();
  if (!api) return;
  const uint32_t frame = static_cast<uint32_t>(fcp::watch::SwapCount());
  switch (id) {
    case k_D3DDevice_DrawIndexedVertices:
    case k_D3DDevice_DrawVertices:
    case k_D3DDevice_DrawVerticesUP: {
      if (!api->draw) return;
      nr::DrawRecord r{};
      r.device = a.r3;
      r.frame = frame;
      r.primitive = a.r4;
      if (id == k_D3DDevice_DrawIndexedVertices) {
        r.kind = nr::kDrawIndexed;
        r.base_vertex = static_cast<int32_t>(a.r5);
        r.start = a.r6;
        r.count = a.r7;
      } else if (id == k_D3DDevice_DrawVertices) {
        r.kind = nr::kDrawVertices;
        r.start = a.r5;
        r.count = a.r6;
      } else {
        r.kind = nr::kDrawVerticesUP;
        r.count = a.r5;
        r.up_guest = a.r6;
        r.up_stride = a.r7;
        r.up_bytes = a.r5 * a.r7;
        if (r.up_bytes && r.up_bytes < (8u << 20) && HotReadable(base, a.r6, r.up_bytes))
          r.up_data = fcp::guest::Ptr(base, a.r6);
      }
      r.call = MakeCall(a);
      api->draw(api->context, &r);
      return;
    }
    case k_D3DDevice_ClearF: {
      if (!api->clear) return;
      nr::ClearRecord r{};
      r.device = a.r3;
      r.frame = frame;
      r.flags = a.r4;
      r.has_rect = ReadRect(base, a.r5, r.rect);
      if (!ReadVec4(base, a.r6, r.color)) r.color[0] = r.color[1] = r.color[2] = r.color[3] = 0.0f;
      r.z = static_cast<float>(a.f1);
      r.stencil = a.r7;
      r.call = MakeCall(a);
      api->clear(api->context, &r);
      return;
    }
    case k_D3DDevice_Resolve: {
      if (!api->resolve) return;
      nr::ResolveRecord r{};
      r.device = a.r3;
      r.frame = frame;
      r.flags = a.r4;
      r.has_rect = ReadRect(base, a.r5, r.rect);
      r.dest_texture = a.r6;
      if (a.r7 && HotReadable(base, a.r7, 8)) {
        r.has_point = 1;
        r.point[0] = static_cast<int32_t>(fcp::guest::U32(base, a.r7));
        r.point[1] = static_cast<int32_t>(fcp::guest::U32(base, a.r7 + 4));
      }
      r.dest_level = a.r8;
      r.dest_slice = a.r9;
      r.has_clear_color = ReadVec4(base, a.r10, r.clear_color);
      r.clear_z = static_cast<float>(a.f1);
      r.call = MakeCall(a);
      api->resolve(api->context, &r);
      return;
    }
    case k_D3DDevice_BeginTiling:
    case k_D3DDevice_EndTiling: {
      if (!api->tiling) return;
      nr::TilingRecord r{};
      r.device = a.r3;
      r.frame = frame;
      r.decoded = 1;
      r.flags = a.r4;
      if (id == k_D3DDevice_BeginTiling) {
        r.begin = 1;
        r.rect_count = a.r5;
        r.rects = a.r6;
        r.has_clear_color = ReadVec4(base, a.r7, r.clear_color);
        r.clear_stencil = a.r8;
      } else {
        r.rect_count = a.r5 ? 1 : 0;
        r.rects = a.r5;
        r.dest_texture = a.r6;
        r.has_clear_color = ReadVec4(base, a.r7, r.clear_color);
        r.clear_stencil = a.r8;
        r.parameters = a.r9;
      }
      r.clear_z = static_cast<float>(a.f1);
      r.call = MakeCall(a);
      api->tiling(api->context, &r);
      return;
    }
    case k_D3DDevice_SetPredication: {
      if (!api->predication) return;
      nr::PredicationRecord r{};
      r.device = a.r3;
      r.frame = frame;
      r.mask = a.r4;
      r.call = MakeCall(a);
      api->predication(api->context, &r);
      return;
    }
    case k_D3DDevice_BeginConditionalSurvey:
    case k_D3DDevice_EndConditionalSurvey:
    case k_D3DDevice_BeginConditionalRendering:
    case k_D3DDevice_EndConditionalRendering:
    case k_D3DQuery_Issue: {
      // Observed before the original (the original still runs). Arguments: Begin*(device, Identifier, Flags),
      // EndConditionalSurvey(device, Flags), Query_Issue(query, flags 2 = BEGIN, 1 = END).
      if (!api->query) return;
      nr::QueryRecord r{};
      r.frame = frame;
      if (id == k_D3DQuery_Issue) {
        r.op = (a.r4 & 2) ? nr::kQueryIssueBegin : nr::kQueryIssueEnd;
        r.query = a.r3;
        r.flags = a.r4;
      } else {
        r.device = a.r3;
        r.op = id == k_D3DDevice_BeginConditionalSurvey   ? nr::kSurveyBegin
               : id == k_D3DDevice_EndConditionalSurvey   ? nr::kSurveyEnd
               : id == k_D3DDevice_BeginConditionalRendering ? nr::kConditionalBegin
                                                             : nr::kConditionalEnd;
        r.flags = a.r4;
      }
      r.call = MakeCall(a);
      uint32_t samples = 0;
      api->query(api->context, &r, &samples);
      return;
    }
    case k_D3D_LockResource: {
      // CPU access to a resource: the renderer writes a host-only resolve copy back to guest memory first.
      if (api->size < sizeof(nr::NrApi) || !api->lock) return;
      nr::LockRecord r{};
      r.frame = frame;
      r.call = MakeCall(a);
      api->lock(api->context, &r);
      return;
    }
    case k_D3DDevice_Swap: {
      if (!api->swap) return;
      nr::SwapRecord r{};
      r.device = a.r3;
      r.frame = frame;
      r.front_buffer = a.r4;
      r.flags = a.r5;
      r.call = MakeCall(a);
      api->swap(api->context, &r);
      return;
    }
    default:
      return;
  }
}

// After the original: device creation.
void ForwardAfter(Id id, const Args& a, const PPCContext& after, uint8_t* base) {
  const nr::NrApi* api = nr::GetNrApi();
  if (!api || id != k_Direct3D_CreateDevice || !api->device_created) return;
  if (a.r8 && fcp::guest::Readable(base, a.r8, 4)) {
    const uint32_t device = fcp::guest::U32(base, a.r8);
    if (device && after.r3.u32 == 0) api->device_created(api->context, device);
  }
}

template <Id kId>
inline void Run(PPCContext& ctx, uint8_t* base, PPCFunc& original) {
  if (!g_hooks[kId].enabled) {
    original(ctx, base);
    return;
  }
  Call call{kId, Args::From(ctx), 0, false};
  Enter(call, ctx, base);
  const bool outer = t_depth == 0;
  if (outer) {
    t_outer = kId;
    fcp::render::SetGuestBase(base);
    ForwardBefore(kId, call.entry, base);
  } else if (kId == k_D3DDevice_ClearF && t_depth == 1 && t_outer == k_D3DDevice_BeginTiling && g_forward_tiling_clear) {
    ForwardBefore(kId, call.entry, base);
  }
  ++t_depth;
  original(ctx, base);
  --t_depth;
  if constexpr (kId == k_D3DQuery_GetData) {
    // GetData(query, pData, size, flags) -> S_OK / S_FALSE (measured: r4 = pData, r5 = 4).
    auto store = [&](uint32_t v) {
      uint8_t* p = fcp::guest::Ptr(base, call.entry.r4);
      p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    };
    const bool writable = call.entry.r4 && call.entry.r5 >= 4 && HotReadable(base, call.entry.r4, 4);
    // FCP_RENDER_QUERY_AB=<n> (diagnosis): in every second block of n frames the fixed "visible" answer instead
    // of the host query, to compare both in one run at nearly the same place.
    const bool ab_visible = g_query_ab && ((fcp::watch::SwapCount() / g_query_ab) & 1);
    if ((g_query_visible || ab_visible) && writable) {
      store(g_query_samples);
      ctx.r3.u64 = 0;
      g_query_overrides.fetch_add(1, std::memory_order_relaxed);
    } else if (outer && writable) {
      // FCP_RENDER_QUERY=host: the renderer answers from its host query (never blocks). Unsupported keeps the
      // original's answer. Not ready: as the original's own not-ready path, *pData = 1 and S_FALSE.
      if (const nr::NrApi* api = nr::GetNrApi(); api && api->query) {
        nr::QueryRecord r{};
        r.frame = static_cast<uint32_t>(fcp::watch::SwapCount());
        r.op = nr::kQueryGetData;
        r.query = call.entry.r3;
        r.flags = call.entry.r6;
        r.call = MakeCall(call.entry);
        uint32_t samples = 0;
        const uint32_t st = api->query(api->context, &r, &samples);
        if (st == nr::kQueryReady) {
          store(samples);
          ctx.r3.u64 = 0;
          g_query_overrides.fetch_add(1, std::memory_order_relaxed);
        } else if (st == nr::kQueryNotReady) {
          store(1);
          ctx.r3.u64 = 1;
          g_query_not_ready.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }
  Leave(call, ctx, base);
  if (outer) ForwardAfter(kId, call.entry, ctx, base);
}

}  // namespace

void Configure() {
  const std::string trace = fcp::env::Get("FCP_TRACE_D3D");
  if (!trace.empty()) {
    unsigned long long first = 0, count = 1;
    if (std::sscanf(trace.c_str(), "%llu,%llu", &first, &count) >= 1) {
      g_trace_first = first;
      g_trace_count = count ? count : 1;
    }
  }
  const std::string dump = fcp::env::Get("FCP_DUMP_DEVICE");
  for (size_t pos = 0; pos < dump.size();) {
    const size_t comma = dump.find(',', pos);
    const std::string part = dump.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!part.empty()) {
      g_dump_frames.push_back(std::strtoull(part.c_str(), nullptr, 0));
    }
    if (comma == std::string::npos) {
      break;
    }
    pos = comma + 1;
  }
  g_dump_shaders = fcp::env::Flag("FCP_DUMP_SHADERS");
  g_forward_tiling_clear = fcp::env::Flag("FCP_RENDER_TILING_CLEAR", true);
  g_query_visible = fcp::env::Get("FCP_RENDER_QUERY") == "visible";
  g_query_samples = static_cast<uint32_t>(fcp::env::Int("FCP_RENDER_QUERY_SAMPLES", 0x4000));
  g_query_ab = static_cast<uint64_t>(fcp::env::Int("FCP_RENDER_QUERY_AB", 0));
  g_check_readable = fcp::env::Flag("FCP_D3D_CHECK_READABLE", false);
  g_log_first = static_cast<uint64_t>(fcp::env::Int("FCP_D3D_LOG_FIRST", 3));
  std::string disabled;
  for (int i = 0; i < kHookCount; ++i) {
    const std::string name = std::string("FCP_D3D_HOOK_") + kNames[i];
    g_hooks[i].enabled = fcp::env::Flag(name.c_str(), true);
    if (!g_hooks[i].enabled) {
      disabled += std::string(" ") + kNames[i];
    }
  }
  FCPLOG("d3d hooks: {} (logging, originals run), trace frame {} count {}, dump frames {}, shaders {}{}{}",
         int(kHookCount), g_trace_first == UINT64_MAX ? std::string("-") : std::to_string(g_trace_first),
         g_trace_count, dump.empty() ? "-" : dump, g_dump_shaders ? "dumped" : "logged",
         disabled.empty() ? "" : ", passthrough:", disabled);
}

void WriteSummary() {
  FCPLOG("d3d summary: swaps={} device={:08X} query overrides={} not ready={}", fcp::watch::SwapCount(), g_device.load(),
         g_query_overrides.load(), g_query_not_ready.load());
  for (int i = 0; i < kHookCount; ++i) {
    FCPLOG("d3d count {:>10} {}", g_hooks[i].calls.load(), kNames[i]);
  }
  FCPLOG("d3d shaders created: VS={} PS={} without container={} distinct objects={}",
         g_shaders_created[0].load(), g_shaders_created[1].load(), g_shaders_bad_magic.load(),
         g_shader_kind.size());
  auto line = [](const char* what, const Check& c) {
    FCPLOG("d3d name check {}: match={} mismatch={} unknown_object={}", what, c.match.load(),
           c.mismatch.load(), c.unknown.load());
  };
  line("SetVertexShader(arg is a VS object)", g_check_set_vs);
  line("SetPixelShader(arg is a PS object)", g_check_set_ps);
  line("D3DVertexShader_Release(r3 is a VS object)", g_check_rel_vs);
  line("D3DPixelShader_Release(r3 is a PS object)", g_check_rel_ps);
}

}  // namespace fcp::d3d

// --- the hooks ------------------------------------------------------------------------------------

#define FCP_X(name)                                                       \
  REX_EXTERN(__imp__##name);                                              \
  REX_HOOK_RAW(name) {                                                    \
    ::fcp::d3d::Run<::fcp::d3d::k_##name>(ctx, base, __imp__##name);      \
  }
FCP_D3D_HOOK_LIST(FCP_X)
#undef FCP_X
