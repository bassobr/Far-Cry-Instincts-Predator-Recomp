// GameRenderer (renderer/game_renderer.h): the in-game draw path.
//
// Original code. Based on King Kong Recompiled (e3e5d3f, native-renderer/backend/game_renderer.cpp, MIT,
// src/renderer/kk/LICENSE-KingKongRecompiled.txt): the mapping of the blend, depth and raster registers
// (BlendTargetFrom, BuildPipelineDesc), the viewport computation (ComputeViewport, there after the SDK's Xenos
// driver) and the construction of the vertex fetch entries are taken from there and adapted to our structures.
#include "renderer/game_renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <thread>
#include <atomic>

#include <nvrhi/nvrhi.h>

#include "fcshaders/library.h"
#include "kk/backend/guest_memory.h"
#include "kk/resources/buffers.h"
#include "kk/resources/guest_texture.h"
#include "kk/resources/nvrhi_format.h"
#include "kk/resources/render_targets.h"
#include "kk/resources/resolve.h"
#include "kk/resources/texture_convert.h"
#include "kk/resources/xenos.h"
#include "overlay_pass.h"
#include "smaa_pass.h"
#include "renderer/builtin_shaders.h"
#include "renderer/diagnostics.h"
#include "renderer/guest_device.h"
#include "renderer/shader_library.h"
#include "renderer/texture_replace.h"
#include "renderer/translated_layout.h"

namespace fcp::nr {

namespace {

using F = DeviceField;
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

uint64_t Mix(uint64_t h, uint64_t v) {
  h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
  return h * 0xFF51AFD7ED558CCDull;
}

// --- Register -> NVRHI (King Kong, game_renderer.cpp) ----------------------------------------------------------

nvrhi::BlendFactor BlendFactorFrom(uint32_t f) {
  using B = nvrhi::BlendFactor;
  switch (f) {
    case 0: return B::Zero;
    case 1: return B::One;
    case 4: return B::SrcColor;
    case 5: return B::InvSrcColor;
    case 6: return B::SrcAlpha;
    case 7: return B::InvSrcAlpha;
    case 8: return B::DstColor;
    case 9: return B::InvDstColor;
    case 10: return B::DstAlpha;
    case 11: return B::InvDstAlpha;
    case 12: return B::ConstantColor;
    case 13: return B::InvConstantColor;
    case 14: return B::ConstantColor;
    case 15: return B::InvConstantColor;
    case 16: return B::SrcAlphaSaturate;
    default: return B::One;
  }
}

nvrhi::BlendOp BlendOpFrom(uint32_t op) {
  using O = nvrhi::BlendOp;
  switch (op) {
    case 0: return O::Add;
    case 1: return O::Subtract;
    case 2: return O::Min;
    case 3: return O::Max;
    case 4: return O::ReverseSubtract;
    default: return O::Add;
  }
}

nvrhi::BlendState::RenderTarget BlendTargetFrom(uint32_t control, uint32_t mask) {
  nvrhi::BlendState::RenderTarget t;
  const uint32_t cs = control & 0x1F, cop = (control >> 5) & 7, cd = (control >> 8) & 0x1F;
  const uint32_t as = (control >> 16) & 0x1F, aop = (control >> 21) & 7, ad = (control >> 24) & 0x1F;
  const bool color_off = cs == 1 && cd == 0 && cop == 0;
  const bool alpha_off = as == 1 && ad == 0 && aop == 0;
  t.blendEnable = !(color_off && alpha_off);
  t.srcBlend = BlendFactorFrom(cs);
  t.destBlend = BlendFactorFrom(cd);
  t.blendOp = BlendOpFrom(cop);
  auto to_alpha = [](nvrhi::BlendFactor f) {
    switch (f) {
      case nvrhi::BlendFactor::SrcColor: return nvrhi::BlendFactor::SrcAlpha;
      case nvrhi::BlendFactor::InvSrcColor: return nvrhi::BlendFactor::InvSrcAlpha;
      case nvrhi::BlendFactor::DstColor: return nvrhi::BlendFactor::DstAlpha;
      case nvrhi::BlendFactor::InvDstColor: return nvrhi::BlendFactor::InvDstAlpha;
      default: return f;
    }
  };
  t.srcBlendAlpha = to_alpha(BlendFactorFrom(as));
  t.destBlendAlpha = to_alpha(BlendFactorFrom(ad));
  t.blendOpAlpha = BlendOpFrom(aop);
  t.colorWriteMask = nvrhi::ColorMask(mask & 0xF);
  return t;
}

nvrhi::StencilOp StencilOpFrom(uint32_t op) { return nvrhi::StencilOp((op & 7) + 1); }
nvrhi::ComparisonFunc CompareFrom(uint32_t f) { return nvrhi::ComparisonFunc((f & 7) + 1); }

// ColorRenderTargetFormat (Xenia/ReXGlue xenos.h numbering, BSD-3) -> Host.
nvrhi::Format ColorTargetFormat(uint32_t f) {
  using N = nvrhi::Format;
  switch (f) {
    case 0: case 1: return N::RGBA8_UNORM;
    case 2: case 10: return N::R10G10B10A2_UNORM;
    case 3: case 12: return N::RGBA16_FLOAT;
    case 4: return N::RG16_SNORM;
    case 5: return N::RGBA16_SNORM;
    case 6: return N::RG16_FLOAT;
    case 7: return N::RGBA16_FLOAT;
    case 14: return N::R32_FLOAT;
    case 15: return N::RG32_FLOAT;
    default: return N::RGBA8_UNORM;
  }
}

// Texture format of a resolve destination texture -> host format of the host copy (logical RGBA, no swizzle).
nvrhi::Format ResolveTextureFormat(res::TextureFormat f) {
  using T = res::TextureFormat;
  using N = nvrhi::Format;
  switch (f) {
    case T::k_16_16_16_16_FLOAT: case T::k_16_16_FLOAT: return N::RGBA16_FLOAT;
    case T::k_32_FLOAT: case T::k_24_8: case T::k_24_8_FLOAT: return N::R32_FLOAT;
    case T::k_32_32_32_32_FLOAT: return N::RGBA32_FLOAT;
    case T::k_2_10_10_10: return N::R10G10B10A2_UNORM;
    case T::k_16_16_16_16: return N::RGBA16_UNORM;
    default: return N::RGBA8_UNORM;
  }
}

nvrhi::SamplerAddressMode AddressFrom(uint32_t clamp) {
  using A = nvrhi::SamplerAddressMode;
  switch (clamp) {
    case 0: return A::Wrap;
    case 1: return A::Mirror;
    case 2: case 4: return A::Clamp;
    case 3: case 5: return A::MirrorOnce;
    case 6: case 7: return A::Border;
    default: return A::Clamp;
  }
}

// Guest memory through the hooks' host pointer (virtual: base + a; physical: base + 0xA0000000 + p).
class HookMemory final : public GuestMemory {
 public:
  const uint8_t* base = nullptr;
  const uint8_t* Virtual(uint32_t a) const override { return base && a ? base + a : nullptr; }
  const uint8_t* Physical(uint32_t p) const override { return base ? base + 0xA0000000u + (p & 0x1FFFFFFFu) : nullptr; }
};

struct ShaderRef {
  const ShaderEntry* entry = nullptr;
  nvrhi::ShaderHandle handle;
};

struct Target {
  nvrhi::TextureHandle tex;
  uint32_t width = 0, height = 0;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  bool depth = false;
  uint32_t srv_slot = 0;  // slot in Tex2D (colour targets, for resolve blits)
  uint32_t guest_format = 0, surface_w = 0, surface_h = 0, msaa = 0, base = 0;
  uint32_t host_samples = 1;  // MSAA of the host target
  uint32_t surface = 0;       // guest address of the surface (diagnostics)
  nvrhi::TextureHandle single;  // MSAA targets: resolved single-sample copy (source of the resolve blits)
  // Render scaling: logical size in guest pixels (width/height are host pixels) and factor.
  uint32_t gw = 0, gh = 0;
  float sx = 1.0f, sy = 1.0f;
};

struct GuestTex {
  nvrhi::TextureHandle tex;
  uint32_t slot = 0;  // with bit 31 for 2D arrays
  Table table = Table::kTex2D;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  res::TextureRanges ranges;
  res::HostTexturePlan plan;
  bool failed = false;
  bool watched = false;        // ranges are under write watch
  bool volatile_data = false;  // changes constantly: hash every frame, do not watch
  uint64_t last_change_frame = ~0ull;
  int replace_kind = -1;       // TextureReplaceSource::Classify (button glyphs), -1: never replaced
};

struct GuestBuf {
  nvrhi::BufferHandle buf;
  uint32_t slot = 0;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  bool watched = false, volatile_data = false;
  uint64_t last_change_frame = ~0ull;
};

struct Resolved {
  nvrhi::TextureHandle tex;
  uint32_t slot = 0;
  uint32_t width = 0, height = 0;
  nvrhi::Format format = nvrhi::Format::UNKNOWN;
  uint64_t frame = 0;
  uint64_t written_back_frame = ~0ull;  // state of the last write-back into guest memory
  uint32_t base = 0;                    // physical base (key in `resolved`)
  std::unordered_map<uint16_t, uint32_t> views;  // constant swizzle (0/1 per component) -> slot
  // Render scaling: guest size (width/height = host) and factor; guest_copy: guest-sized copy for CPU locks.
  uint32_t gw = 0, gh = 0;
  float sx = 1.0f, sy = 1.0f;
  nvrhi::TextureHandle guest_copy;
  bool Scaled() const { return width != gw || height != gh; }
};

// Host size of a guest dimension at factor s (exactly the guest dimension at 1).
uint32_t HostSize(uint32_t guest, float s) { return s == 1.0f ? guest : std::max(1u, uint32_t(std::lround(double(guest) * s))); }

// View of a resolve copy: it holds logical RGBA (as the shader wrote it); of the fetch swizzle only the constants
// matter (X8R8G8B8: alpha = 1). Key: 2 bits per component (0 channel, 1 zero, 2 one).
uint16_t ConstantSwizzleKey(uint16_t swizzle) {
  uint16_t key = 0;
  for (int i = 0; i < 4; ++i) {
    const uint8_t s = res::SwizzleComponent(swizzle, i);
    key |= uint16_t((s == res::kSwz0 ? 1 : s == res::kSwz1 ? 2 : 0) << (2 * i));
  }
  return key;
}

// Base address from the fetch image of a texture object (+16): it holds the CPU address in one of the physical
// views; the 0xE0000000 view lies 4 KB before physical memory (measured: resolve target 0x1D09F000 in the object,
// 0x1D0A0000 in the device's fetch constant when sampling). The fetch constants in the device already carry the
// GPU address.
uint32_t ObjectBase(const res::TextureFetch& f) { return res::CpuToPhysical(f.words[1] & 0xFFFFF000u); }

nvrhi::ComponentMapping MappingFromConstantKey(uint16_t key) {
  nvrhi::ComponentSwizzle c[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t k = (key >> (2 * i)) & 3;
    c[i] = k == 1 ? nvrhi::ComponentSwizzle::Zero : k == 2 ? nvrhi::ComponentSwizzle::One : nvrhi::ComponentSwizzle(i);
  }
  return nvrhi::ComponentMapping{c[0], c[1], c[2], c[3]};
}

struct ViewportSetup {
  nvrhi::Viewport viewport;
  nvrhi::Rect scissor;
  float ndc_scale[3] = {1, 1, 1}, ndc_offset[3] = {0, 0, 0};
};

constexpr uint32_t kUpRingBytes = 16u << 20;
constexpr uint32_t kIndexRingBytes = 16u << 20;
constexpr uint32_t kPatternVertices = 65536;
constexpr uint32_t kMaxDrawsPerList = 900;

// A pipeline record (fixed layout, also the file format of the pipeline list; version in kPipeListMagic).
struct PipeRec {
  uint64_t vs = 0, ps = 0;  // container hashes (ps 0: no PS)
  uint32_t topology = 0;
  uint32_t formats[4] = {}, controls[4] = {}, masks[4] = {};
  uint32_t depth_format = 0, depth_control = 0, ref_mask = 0;
  uint32_t mode = 0, clip = 0, poly_scale = 0, poly_offset = 0, pad = 0;
  uint64_t Key() const {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(this);
    uint64_t h = 0x50495045ull;
    for (size_t i = 0; i < sizeof(PipeRec) / 4; ++i) h = Mix(h, w[i]);
    return h;
  }
};
static_assert(sizeof(PipeRec) % 8 == 0);
constexpr uint64_t kSolidPsKey = 0x501D;
constexpr uint32_t kPipeListMagic = 0x31504346;  // "FCP1"

nvrhi::GraphicsPipelineDesc PipelineDescFrom(const PipeRec& r, nvrhi::IShader* vs, nvrhi::IShader* ps,
                                             const nvrhi::BindingLayoutVector& layouts) {
  nvrhi::GraphicsPipelineDesc pd;
  pd.primType = nvrhi::PrimitiveType(r.topology);
  pd.VS = vs;
  pd.PS = ps;
  pd.bindingLayouts = layouts;
  for (uint32_t i = 0; i < 4; ++i)
    if (r.formats[i]) pd.renderState.blendState.targets[i] = BlendTargetFrom(r.controls[i], r.masks[i]);
  nvrhi::DepthStencilState& ds = pd.renderState.depthStencilState;
  const uint32_t dc = r.depth_control;
  if (r.depth_format) {
    ds.depthTestEnable = (dc & 2) != 0;
    ds.depthWriteEnable = ds.depthTestEnable && (dc & 4) != 0;
    ds.depthFunc = CompareFrom(dc >> 4);
    ds.stencilEnable = (dc & 1) != 0;
    ds.stencilReadMask = uint8_t(r.ref_mask >> 8);
    ds.stencilWriteMask = uint8_t(r.ref_mask >> 16);
    ds.dynamicStencilRef = true;
    ds.frontFaceStencil.stencilFunc = CompareFrom(dc >> 8);
    ds.frontFaceStencil.failOp = StencilOpFrom(dc >> 11);
    ds.frontFaceStencil.passOp = StencilOpFrom(dc >> 14);
    ds.frontFaceStencil.depthFailOp = StencilOpFrom(dc >> 17);
    if (dc & 0x80) {
      ds.backFaceStencil.stencilFunc = CompareFrom(dc >> 20);
      ds.backFaceStencil.failOp = StencilOpFrom(dc >> 23);
      ds.backFaceStencil.passOp = StencilOpFrom(dc >> 26);
      ds.backFaceStencil.depthFailOp = StencilOpFrom(dc >> 29);
    } else {
      ds.backFaceStencil = ds.frontFaceStencil;
    }
  } else {
    ds.depthTestEnable = ds.depthWriteEnable = ds.stencilEnable = false;
  }
  nvrhi::RasterState& rs = pd.renderState.rasterState;
  rs.cullMode = (r.mode & 1) ? nvrhi::RasterCullMode::Front : ((r.mode & 2) ? nvrhi::RasterCullMode::Back : nvrhi::RasterCullMode::None);
  rs.frontCounterClockwise = ((r.mode >> 2) & 1) == 0;
  rs.fillMode = nvrhi::RasterFillMode::Solid;
  rs.scissorEnable = true;
  rs.depthClipEnable = r.clip == 0;
  if (r.depth_format && (r.mode & (1u << 11))) {
    float sc, of;
    std::memcpy(&sc, &r.poly_scale, 4);
    std::memcpy(&of, &r.poly_offset, 4);
    rs.slopeScaledDepthBias = sc / 16.0f;
    rs.depthBias = int(of * 16777216.0f);
  }
  return pd;
}

nvrhi::FramebufferInfo FramebufferInfoFrom(const PipeRec& r) {
  nvrhi::FramebufferInfo fi;
  for (uint32_t i = 0; i < 4; ++i)
    if (r.formats[i]) fi.colorFormats.push_back(nvrhi::Format(r.formats[i]));
  fi.depthFormat = nvrhi::Format(r.depth_format);
  fi.sampleCount = std::max(1u, r.pad);  // pad = sample count (0 in old lists: 1)
  fi.sampleQuality = 0;
  return fi;
}

}  // namespace

// Who waits for the renderer lock: the game's render thread per call kind, the SDK's present thread. Only an
// actual wait (try_lock failed) is counted; sums are kept under the lock.
enum LockSite : uint32_t { kSiteDraw, kSiteQuery, kSiteLock, kSiteClear, kSiteResolve, kSiteSwap, kSiteOther, kSitePresent, kSiteCount };
constexpr const char* kSiteNames[kSiteCount] = {"Draw", "Query", "Lock", "Clear", "Resolve", "Swap", "other", "Present"};

struct GameRenderer::Impl {
  GameRendererOptions opt;
  mutable std::mutex mutex;
  struct LockStat {
    uint64_t contended = 0, wait_us = 0;
  };
  std::array<LockStat, kSiteCount> lock_stats{};
  uint64_t present_hold_us = 0, present_frame_wait_us = 0, present_acquire_us = 0, presents_in_window = 0;
  uint64_t swap_wait_us = 0, swap_waits = 0;
  std::array<uint64_t, 8> swap_fences{};  // fence per swap (back pressure, opt.frames_ahead)
  // Lock with measurement (like lock_guard).
  class TimedLock {
   public:
    TimedLock(Impl& m, LockSite site) : m_(m) {
      if (m_.mutex.try_lock()) return;
      const auto t0 = Clock::now();
      m_.mutex.lock();
      LockStat& s = m_.lock_stats[site];
      ++s.contended;
      s.wait_us += uint64_t(MsSince(t0) * 1000.0);
    }
    ~TimedLock() { m_.mutex.unlock(); }
    TimedLock(const TimedLock&) = delete;
    TimedLock& operator=(const TimedLock&) = delete;

   private:
    Impl& m_;
  };
  HookMemory memory;
  res::GuestMemory phys;
  GuestLayout layout;
  // Object offsets, read once from the layout (instead of several name lookups per draw).
  struct LayoutOffsets {
    uint32_t surf_size = 24, surf_info = 48, surf_color = 52, vb_fetch = 12, query_device = 0;
    void Load(const GuestLayout& l) {
      surf_size = l.surface.get("fetch_size_word", 24);
      surf_info = l.surface.get("surface_info", 48);
      surf_color = l.surface.get("color_info", 52);
      vb_fetch = l.vertex_buffer.get("fetch", 12);
      query_device = l.query.get("device", 0);
    }
  } lo;
  ShaderLibrary shaders;
  bool shaders_open = false;
  std::unique_ptr<HostDevice> host;
  nvrhi::IDevice* dev = nullptr;
  nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::VULKAN;
  TranslatedLayout tl;
  bool ready = false, failed = false, shut_down = false;
  void* hwnd = nullptr;
  uint32_t win_w = 0, win_h = 0;
  bool resize_pending = false;
  uint32_t resize_w = 0, resize_h = 0;

  nvrhi::CommandListHandle cl, present_cl;
  bool cl_open = false;
  uint32_t cl_draws = 0;
  uint64_t cl_epoch = 0;

  std::unordered_map<const ShaderEntry*, nvrhi::ShaderHandle> shader_handles;
  nvrhi::ShaderHandle solid_ps, blit_vs, blit_ps, blit_gamma_ps;
  std::unordered_map<uint64_t, uint32_t> samplers;
  std::vector<nvrhi::SamplerHandle> keep_samplers;
  uint32_t linear_clamp = 0;

  // Key: the 6 words of the fetch constant (a std::string of 24 bytes would cost one heap allocation per lookup).
  struct FetchKey {
    std::array<uint32_t, 6> w;
    bool operator==(const FetchKey& o) const { return w == o.w; }
  };
  struct FetchKeyHash {
    size_t operator()(const FetchKey& k) const {
      uint64_t h = 0x7465785Full;
      for (uint32_t v : k.w) h = Mix(h, v);
      return size_t(h);
    }
  };
  std::unordered_map<FetchKey, GuestTex, FetchKeyHash> textures;
  std::unordered_map<uint64_t, GuestBuf> buffers;
  std::vector<std::pair<uint64_t, nvrhi::TextureHandle>> retired;

  // Replacement textures (renderer/texture_replace.h, button glyphs): one host texture with a Tex2D slot per image
  // (id). Slots are never freed but reused for a new image (at most kReplaceSlots).
  TextureReplaceSource* replace_source = nullptr;
  struct ReplaceTex {
    uint64_t id = 0;
    nvrhi::TextureHandle tex;
    uint32_t slot = 0;
    uint64_t used = 0;  // frame of the last binding
  };
  static constexpr size_t kReplaceSlots = 12;
  std::vector<ReplaceTex> replace_cache;
  uint64_t replace_uploads = 0, replace_binds = 0, replace_classified = 0;

  nvrhi::BufferHandle up_ring, index_ring, quad_pattern, fan_pattern;
  uint32_t up_slot = 0;
  uint64_t up_used = 0, index_used = 0;

  std::unordered_map<uint64_t, Target> targets;  // key: guest address of the surface | depth << 32
  std::map<std::array<nvrhi::ITexture*, 5>, nvrhi::FramebufferHandle> framebuffers;
  std::unordered_map<uint32_t, Resolved> resolved;  // key: physical base of the destination texture
  std::set<uint32_t> resolve_objects;                // texture objects ever resolved into (lock write-back)
  std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> pipelines;
  std::unordered_map<uint64_t, nvrhi::GraphicsPipelineHandle> blit_pipelines;
  std::unordered_map<nvrhi::ITexture*, nvrhi::FramebufferHandle> back_fbs;

  // --- Overlay (settings menu, fps display; renderer/overlay.h) -------------------------------------------
  std::unique_ptr<OverlayPass> overlay;  // created on the first overlay frame
  // SMAA 1x ([graphics] smaa): front buffer (with gamma ramp) -> smaa_in, SMAA -> smaa_out, then centred into the
  // swapchain as usual. Created on the first frame with SMAA.
  std::unique_ptr<SmaaPass> smaa;
  bool smaa_failed = false;
  nvrhi::TextureHandle smaa_in, smaa_out;
  nvrhi::FramebufferHandle smaa_in_fb;
  uint32_t smaa_out_slot = 0, smaa_w = 0, smaa_h = 0;
  uint64_t smaa_frames = 0;
  bool overlay_failed = false;
  nvrhi::CommandListHandle overlay_cl;
  uint64_t overlay_frames = 0, overlay_draws = 0;
  std::string present_dump;  // RequestPresentDump: at the next EndPresent

  // --- Occlusion queries (opt.query_mode == 2) -----------------------------------------------------------
  // A guest query can span several command lists (flush after kMaxDrawsPerList); it then has one host slot per
  // list, and the result is the sum.
  struct HostQuery {
    std::vector<uint32_t> slots;
    uint32_t guest_samples = 1, host_samples = 1;
    double pixel_scale = 1.0;      // host pixels per guest pixel of the target (render scaling)
    nvrhi::EventQueryHandle done;  // set once the list containing End has been submitted
    bool ended = false, submitted = false, failed = false;
    Clock::time_point ended_at{};
    uint64_t frame = 0;
  };
  OcclusionQueries* occ = nullptr;
  std::unordered_map<uint32_t, HostQuery> queries;
  uint32_t active_query = 0;     // guest query between Begin and End (0: none)
  bool active_segment = false;   // its host slot has been begun in the open list
  uint32_t next_slot = 0;
  std::vector<uint32_t> unsubmitted;
  uint64_t q_begins = 0, q_ready = 0, q_not_ready = 0, q_fallback = 0, q_zero = 0, q_logged = 0;

  void BeginSegment(nvrhi::ICommandList* c, HostQuery& q) {
    if (api == nvrhi::GraphicsAPI::VULKAN) c->clearState();  // query not inside a render pass
    const uint32_t slot = next_slot++ % occ->capacity();
    occ->Begin(c, slot);
    q.slots.push_back(slot);
    active_segment = true;
  }
  void EndSegment(nvrhi::ICommandList* c, HostQuery& q) {
    if (!active_segment || q.slots.empty()) return;
    if (api == nvrhi::GraphicsAPI::VULKAN) c->clearState();
    occ->End(c, q.slots.back());
    active_segment = false;
  }

  uint32_t QueryBegin(uint32_t query) {
    if (!occ || !query) return kQueryUnsupported;
    if (active_query) {
      // Xenos has only one counter address; a second open query is a bug in the guest or in this layer.
      HostQuery& old = queries[active_query];
      EndSegment(CL(), old);
      old.failed = true;
      active_query = 0;
    }
    HostQuery& q = queries[query];
    q = HostQuery{};
    q.frame = frame;
    // Guest samples per pixel: MSAA of the bound target at Begin (the game divides by its MSAA itself).
    if (const uint8_t* qo = memory.Virtual(query)) {
      const GuestDeviceView d = View(LoadBE32(qo + lo.query_device));
      if (d.valid()) {
        uint32_t surface = d.u32(F::render_targets, 0);
        bool depth = false;
        if (!surface) surface = d.u32(F::depth_stencil), depth = true;
        if (Target* t = GetTarget(surface, depth)) q.guest_samples = 1u << std::min(t->msaa, 2u), q.host_samples = t->host_samples, q.pixel_scale = double(t->sx) * t->sy;
      }
    }
    ++q_begins;
    if (Tracing()) Log("renderer: T f%llu QueryBegin %08X (MSAA %u)", (unsigned long long)frame, query, q.guest_samples);
    BeginSegment(CL(), q);
    active_query = query;
    return kQueryReady;
  }

  uint32_t QueryEnd(uint32_t query) {
    if (!occ) return kQueryUnsupported;
    auto it = queries.find(query);
    if (it == queries.end() || active_query != query) {
      if (it != queries.end()) it->second.failed = true;
      return kQueryUnsupported;
    }
    HostQuery& q = it->second;
    if (Tracing()) Log("renderer: T f%llu QueryEnd %08X", (unsigned long long)frame, query);
    EndSegment(CL(), q);
    q.ended = true;
    q.ended_at = Clock::now();
    active_query = 0;
    unsubmitted.push_back(query);
    return kQueryReady;
  }

  uint32_t QueryGetData(uint32_t query, uint32_t* samples) {
    auto it = queries.find(query);
    if (!occ || it == queries.end() || it->second.failed || !it->second.ended || it->second.slots.empty()) {
      ++q_fallback;
      return kQueryUnsupported;
    }
    HostQuery& q = it->second;
    // Do not submit just because of a query: the game asks once per frame and otherwise keeps the old value
    // (sub_8237E630); a flush per query cost half the frame rate in a level (measured). The swap submits.
    // If a caller polls in a loop, the original's answer takes over after 2 s.
    bool done = q.submitted && q.done && dev->pollEventQuery(q.done);
    uint64_t sum = 0;
    for (uint32_t s : q.slots) {
      uint64_t v = 0;
      if (!done || !occ->Read(s, &v)) {
        done = false;
        break;
      }
      sum += v;
    }
    if (!done) {
      ++q_not_ready;
      // Safety net: a result still missing after 2 s will not arrive (device lost or similar).
      if (MsSince(q.ended_at) > 2000.0) {
        q.failed = true;
        ++q_fallback;
        return kQueryUnsupported;
      }
      return kQueryNotReady;
    }
    // Host count in guest samples: x guest MSAA / host MSAA / scale^2 (at factor 1 the previous computation).
    const uint64_t guest = q.pixel_scale == 1.0 ? sum * q.guest_samples / std::max(1u, q.host_samples)
                                                : uint64_t(std::llround(double(sum) * q.guest_samples / std::max(1u, q.host_samples) / q.pixel_scale));
    *samples = uint32_t(std::min<uint64_t>(guest, 0xFFFFFFFFull));
    ++q_ready;
    if (!guest) ++q_zero;
    if (q_logged < 24 || Tracing()) {
      ++q_logged;
      Log("renderer: Query %08X f%llu (Begin f%llu): host %llu, guest %u (MSAA %u/%u, %zu list(s))", query, (unsigned long long)frame,
          (unsigned long long)q.frame, (unsigned long long)sum, *samples, q.guest_samples, q.host_samples, q.slots.size());
    }
    return kQueryReady;
  }

  bool in_tiling = false;
  int32_t survey_id = -1, conditional_id = -1;  // open survey or conditional block (-1: none)
  uint64_t frame_conditional = 0, frame_surveys = 0;
  uint32_t tile_w = 0, tile_h = 0;
  uint32_t predication = 0;

  // --- Render scaling ---------------------------------------------------------------------------------------
  // Classes: screen targets (guest width >= scale_min_width, landscape) get `scale`, shadow maps (square, from
  // 1024) `shadow_scale`, everything else (bloom/adaptation chain, 512/256 targets) stays at console size: the game
  // passes their pixel sizes in constants, and the CPU reads the 5x3 luminance.
  float cur_scale = 1.0f;       // current factor of the screen targets (with auto from the swapchain height)
  float wanted_scale = 1.0f;    // after a resize (auto), applied at the next swap
  uint64_t rescales = 0;
  uint64_t diag_rescale_frame = 0;
  float diag_rescale_to = 1.0f;
  // Factor per axis; with Hor+ screen-sized targets are cur_wide times wider in x as well (anamorphic).
  void ScaleFor(uint32_t w, uint32_t h, float& sx, float& sy) const {
    if (w == h && w >= 1024) {
      sx = sy = opt.shadow_scale;
    } else if (IsScreenTarget(w, h)) {
      sx = cur_wide == 1.0f ? cur_scale : cur_scale * cur_wide;
      sy = cur_scale;
    } else {
      sx = sy = 1.0f;
    }
  }
  bool IsScreenTarget(uint32_t w, uint32_t h) const { return w >= opt.scale_min_width && w > h; }

  // --- Hor+ -------------------------------------------------------------------------------------------------
  // Anamorphic: screen-sized targets are cur_wide times wider on the host, the guest coordinates stay (1280 wide).
  // The camera (fcp/camera_fov.cpp) draws the world that much wider into these 1280 guest pixels; the host stretches
  // it back. A picture with a 3D view is shown in the wide shape, all others (menus, videos, loading screens) at 16:9
  // (showing them undoes the stretch). Over a 3D picture, 2D draws after its post processing (HUD, texts, menus over
  // the game) go into the 16:9 middle (narrower host viewport), unless they cover the whole width (post processing,
  // fades, bars).
  float cur_wide = 1.0f, wanted_wide = 1.0f;
  std::atomic<float> wide_published{1.0f};  // for the camera hook (Wide())
  bool frame_3d = false;      // since the last swap: a draw with depth test into a screen-sized target
  bool post_3d = false;       // after that: a resolve from the 3D view's target (post processing begins)
  uint32_t view_surface = 0;  // colour target of the 3D view
  bool front_wide = false;    // the shown picture has a 3D view (shown in the wide shape)
  uint64_t wide_frames = 0, narrow_draws = 0, full_draws = 0, unknown_boxes = 0;
  std::set<uint64_t> wide_traced;  // FCP_TRACE_WIDE: kinds already logged
  // Aspect of the 3D picture -> factor over 16:9 (1: off). Up to 1 % above 16:9 counts as 16:9.
  float WideFor(uint32_t swap_w, uint32_t swap_h) const {
    const float want = opt.aspect > 0.0f ? opt.aspect : (swap_w && swap_h ? float(swap_w) / float(swap_h) : 16.0f / 9.0f);
    const float w = want / (16.0f / 9.0f);
    return w <= 1.01f ? 1.0f : std::min(w, 4.0f);
  }
  uint32_t screen_samples = 0;  // [graphics] msaa = 2|4|8, limited to the device (0: like the console)
  static float AutoScale(uint32_t swap_h) { return swap_h >= 720 ? float(swap_h) / 720.0f : 1.0f; }
  fcshaders::ScaleConstants scale_c{}, last_scale_c{};
  uint64_t scale_epoch = 0;
  float tex_inv_x = 1.0f, tex_inv_y = 1.0f;  // from Texture(): guest/host of the last returned texture

  // Scales a rectangle edge (guest pixels -> host pixels).
  static int32_t ScaleEdge(int32_t v, float s) { return s == 1.0f ? v : int32_t(std::lround(double(v) * s)); }

  // New factor (scale = auto after a resize): recreate the targets (EDRAM content only lives within a frame), carry
  // resolve textures with content over to the new size (textures resolved once are preserved that way).
  void ApplyScale(float s, float wide) {
    if (s == cur_scale && wide == cur_wide) return;
    Flush();
    dev->waitForIdle();
    Log("renderer: render scale %.3f -> %.3f, Hor+ %.4f -> %.4f (targets recreated, %zu resolve textures carried over)", cur_scale, s,
        cur_wide, wide, resolved.size());
    cur_scale = s;
    cur_wide = wide;
    wide_published.store(wide);
    ++rescales;
    targets.clear();
    framebuffers.clear();
    // First all copies (reading through the old slots), then wait, then rewrite the slots: descriptor tables take
    // effect immediately, so a copy not yet executed would otherwise already read the new texture.
    std::vector<std::pair<Resolved*, nvrhi::TextureHandle>> moved;
    for (auto& [base, r] : resolved) {
      float nsx = 1.0f, nsy = 1.0f;
      ScaleFor(r.gw, r.gh, nsx, nsy);
      const uint32_t w = HostSize(r.gw, nsx), h = HostSize(r.gh, nsy);
      if (w == r.width && h == r.height) continue;
      nvrhi::TextureDesc d = r.tex->getDesc();
      d.width = w;
      d.height = h;
      nvrhi::TextureHandle nt = dev->createTexture(d);
      if (!nt) continue;
      nvrhi::ICommandList* c = CL();
      nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(nt));
      c->setTextureState(r.tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      const float uv[4] = {0, 0, 1, 1};
      Blit(c, fb, r.slot, uv, nvrhi::Viewport(0, float(w), 0, float(h), 0, 1));
      c->setTextureState(nt, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      moved.emplace_back(&r, nt);
    }
    Flush();
    dev->waitForIdle();
    for (auto& [rp, nt] : moved) {
      Resolved& r = *rp;
      retired.emplace_back(frame, r.tex);
      r.tex = nt;
      r.width = nt->getDesc().width, r.height = nt->getDesc().height;
      r.sx = float(r.width) / float(r.gw), r.sy = float(r.height) / float(r.gh);
      r.guest_copy = nullptr;
      if (front_slot == r.slot) front = r.tex, front_w = r.width, front_h = r.height;
      tl.WriteTextureView(Table::kTex2D, r.slot, r.tex, nvrhi::ComponentMapping());
      for (const auto& [key, slot] : r.views) tl.WriteTextureView(Table::kTex2D, slot, r.tex, MappingFromConstantKey(key));
    }
  }

  // Gamma ramp (SetGammaRamp writes the copy in the device, field gamma_ramp): read at swap, applied at present as
  // a 256x1 table (FCP_RENDER_GAMMA=0: off). Identity: no detour.
  nvrhi::TextureHandle gamma_lut;
  uint32_t gamma_lut_slot = 0;
  std::array<uint16_t, 768> gamma_last{};
  std::vector<uint16_t> gamma_rgba = std::vector<uint16_t>(256 * 4, 0);
  bool gamma_dirty = false, gamma_identity = true, gamma_seen = false;
  uint32_t gamma_logs = 0;

  void ReadGammaRamp(uint32_t device) {
    const GuestDeviceView d = View(device);
    const uint8_t* p = d.valid() ? d.at(F::gamma_ramp) : nullptr;
    if (!p) return;
    std::array<uint16_t, 768> v;
    for (uint32_t i = 0; i < 768; ++i) v[i] = LoadBE16(p + 2 * i);
    if (gamma_seen && v == gamma_last) return;
    gamma_seen = true;
    gamma_last = v;
    bool all_zero = true, identity = true;
    for (uint32_t c = 0; c < 3; ++c)
      for (uint32_t i = 0; i < 256; ++i) {
        const uint16_t x = v[c * 256 + i];
        all_zero = all_zero && x == 0;
        identity = identity && std::abs(int(x) - int(i * 257)) <= 257;
        gamma_rgba[i * 4 + c] = x;
      }
    for (uint32_t i = 0; i < 256; ++i) gamma_rgba[i * 4 + 3] = 0xFFFF;
    gamma_identity = identity || all_zero;  // never set (all 0): treated as identity
    gamma_dirty = true;
    if (gamma_logs++ < 12)
      Log("renderer: gamma ramp frame %llu: R[0,64,128,192,255] = %u %u %u %u %u, G[128] %u, B[128] %u -> %s", (unsigned long long)frame,
          v[0], v[64], v[128], v[192], v[255], v[256 + 128], v[512 + 128],
          all_zero ? "empty (no effect)" : identity ? "identity" : opt.gamma_ramp ? "applied" : "not applied (FCP_RENDER_GAMMA=0)");
  }

  nvrhi::TextureHandle front;  // the last shown frame (resolve copy)
  uint32_t front_slot = 0, front_w = 0, front_h = 0;  // host size (dumps)
  uint32_t front_gw = 0, front_gh = 0;                // guest size (aspect ratio at present)
  nvrhi::ITexture* back = nullptr;

  std::array<float, 1024> vs_c{}, ps_c{}, last_vs{}, last_ps{};
  // FCP_RENDER_FAST_CONSTANTS: big-endian raw data of the last conversion (vs_c/ps_c are its converted form).
  std::array<uint8_t, 4096> raw_vs{}, raw_ps{};
  bool raw_vs_valid = false, raw_ps_valid = false;
  uint64_t fast_const_skips = 0, fast_const_converts = 0;
  fcshaders::DrawConstants last_dc{};
  uint64_t constants_epoch = 0, constant_uploads = 0;
  // Versions of the volatile constant buffers (VS/PS/Draw) per frame and maximum in the statistics interval.
  std::array<uint32_t, 3> cb_frame{}, cb_max{};
  std::vector<uint32_t> scratch_indices;
  std::vector<res::VertexElement> elements;

  // Counters
  uint64_t frame = 0;
  uint64_t frame_calls = 0, frame_draws = 0, frame_skipped = 0, frame_up = 0, frame_clears = 0, frame_resolves = 0;
  uint64_t total_draws = 0, total_skipped = 0, total_presents = 0, total_swaps = 0;
  std::map<std::string, uint64_t> skip_reasons;
  uint64_t tex_check_us = 0, tex_convert_us = 0, frame_draw_us = 0;
  // Per frame (FCP_TRACE_HITCHES / slow frames): values at the last swap
  uint64_t buffer_us = 0, frame_buffer_bytes = 0;
  struct FrameMark {
    Clock::time_point t = Clock::now();
    uint64_t draw_us = 0, conv_us = 0, buf_us = 0, pipe_us = 0, pipes = 0, convs = 0, wait_us = 0;
  } mark;
  uint64_t draw_us_total = 0, slow_frames_logged = 0;
  uint64_t textures_converted = 0, textures_rewritten = 0, buffers_written = 0;
  std::set<uint64_t> logged_surfaces;
  uint32_t first_draw_logs = 0;
  Clock::time_point stats_since = Clock::now();
  std::vector<uint64_t> dump_list;
  std::set<std::string> logged_once;
  uint64_t consts_logged_frame = ~0ull;
  uint64_t frame_new_pipelines = 0;

  // --- Pipeline list (opt.pipeline_list): append new records, create them in the background at startup -------
  std::set<uint64_t> listed_keys;
  std::thread prewarm;
  std::atomic<bool> prewarm_stop{false};

  nvrhi::GraphicsPipelineHandle CreatePipelineFrom(const PipeRec& r, nvrhi::IShader* vs_h, nvrhi::IShader* ps_h) {
    return dev->createGraphicsPipeline(PipelineDescFrom(r, vs_h, ps_h, tl.layouts()), FramebufferInfoFrom(r));
  }

  void AppendPipelineRecord(const PipeRec& r) {
    if (opt.pipeline_list.empty() || !listed_keys.insert(r.Key()).second) return;
    if (FILE* f = std::fopen(opt.pipeline_list.c_str(), "ab")) {
      std::fseek(f, 0, SEEK_END);
      if (std::ftell(f) == 0) {
        const uint32_t head[2] = {kPipeListMagic, uint32_t(sizeof(PipeRec))};
        std::fwrite(head, 4, 2, f);
      }
      std::fwrite(&r, sizeof(r), 1, f);
      std::fclose(f);
    }
  }

  // Reads the list and creates every pipeline whose shaders are in the pack or disk cache, on a thread (the driver
  // compiles in parallel with the game; the draw path picks it up as soon as it is in the memory cache).
  void StartPrewarm() {
    if (opt.pipeline_list.empty() || !opt.pipeline_prewarm) return;
    std::vector<PipeRec> recs;
    if (FILE* f = std::fopen(opt.pipeline_list.c_str(), "rb")) {
      uint32_t head[2] = {};
      if (std::fread(head, 4, 2, f) == 2 && head[0] == kPipeListMagic && head[1] == sizeof(PipeRec)) {
        PipeRec r;
        while (std::fread(&r, sizeof(r), 1, f) == 1) recs.push_back(r);
      }
      std::fclose(f);
    }
    for (const PipeRec& r : recs) listed_keys.insert(r.Key());
    if (recs.empty()) return;
    Log("renderer: pipeline list %s: %zu entries, prewarming in the background", opt.pipeline_list.c_str(), recs.size());
    prewarm = std::thread([this, recs = std::move(recs)] {
      const auto t0 = Clock::now();
      size_t made = 0, missing = 0;
      for (const PipeRec& r : recs) {
        if (prewarm_stop) break;
        nvrhi::IShader* vs_h = nullptr;
        nvrhi::IShader* ps_h = nullptr;
        const ShaderEntry* vs = shaders.FindByHash(r.vs);
        const ShaderEntry* ps = r.ps ? shaders.FindByHash(r.ps) : nullptr;
        {
          std::lock_guard lock(mutex);
          if (!ready || shut_down) break;
          if (pipelines.count(r.Key())) continue;
          vs_h = Handle(vs);
          ps_h = r.ps ? Handle(ps) : nullptr;
        }
        if (!vs_h || (r.ps && !ps_h)) {
          ++missing;
          continue;
        }
        nvrhi::GraphicsPipelineHandle p = CreatePipelineFrom(r, vs_h, ps_h);
        std::lock_guard lock(mutex);
        if (p && pipelines.emplace(r.Key(), p).second) ++made;
      }
      std::lock_guard lock(mutex);
      Log("renderer: prewarm done: %zu pipelines in %.0f ms, %zu without shaders", made, MsSince(t0), missing);
    });
  }
  float k_min_dist = 1e30f;
  const bool diag_zero_translation = std::getenv("FCP_DIAG_ZERO_T") != nullptr;
  const bool diag_dump_textures = std::getenv("FCP_DIAG_DUMP_TEXTURES") != nullptr;  // converted RGBA8 textures >= 256 as PNG
  const int64_t diag_only_draw = std::getenv("FCP_DIAG_ONLY_DRAW") ? std::atoll(std::getenv("FCP_DIAG_ONLY_DRAW")) : -1;
  std::array<float, 6> k_min_t{};

  // Draw probe (FCP_RENDER_PROBE): pixel (x,y) of colour target 0 after every step of a traced frame.
  struct ProbeEvent {
    std::string what;
    const Target* target;
    nvrhi::Format format;
    uint32_t column;
  };
  static constexpr uint32_t kProbeStagingWidth = 65536;
  std::map<nvrhi::Format, std::pair<nvrhi::StagingTextureHandle, uint32_t>> probe_staging;
  std::vector<ProbeEvent> probe_events;

  void Probe(const Target* t, std::string what) {
    if (opt.probe_x < 0 || !Tracing() || !t || t->depth || probe_events.size() >= 8192) return;
    if (t->host_samples > 1 && !t->single) return;
    if (uint32_t(opt.probe_x) >= t->width || uint32_t(opt.probe_y) >= t->height) return;
    auto& [st, next] = probe_staging[t->format];
    if (!st) {
      nvrhi::TextureDesc d;
      d.width = kProbeStagingWidth;
      d.height = 1;
      d.format = t->format;
      d.initialState = nvrhi::ResourceStates::CopyDest;
      d.keepInitialState = true;
      d.debugName = "probe";
      st = dev->createStagingTexture(d, nvrhi::CpuAccessMode::Read);
      if (!st) return;
    }
    if (next >= 8192) return;
    nvrhi::ICommandList* c = CL();
    nvrhi::ITexture* src = t->tex;
    if (t->host_samples > 1) {
      // MSAA target: resolved value (as a resolve would see it).
      c->resolveTexture(t->single, nvrhi::AllSubresources, t->tex, nvrhi::AllSubresources);
      src = t->single;
    }
    // NVRHI (Vulkan, computePlacedBufferOffset) computes the destination offset in texels instead of bytes: column x
    // lands at byte x. Hence origin = column x bytes per texel, so the value lies at byte column x bpp, as read.
    // (D3D12 computes correctly; there bpp = 1 is used.)
    const uint32_t bpp = api == nvrhi::GraphicsAPI::VULKAN ? nvrhi::getFormatInfo(t->format).bytesPerBlock : 1;
    if ((next + 1) * bpp > kProbeStagingWidth) return;
    c->copyTexture(st, nvrhi::TextureSlice().setOrigin(next * bpp, 0).setWidth(1).setHeight(1), src,
                      nvrhi::TextureSlice().setOrigin(uint32_t(opt.probe_x), uint32_t(opt.probe_y)).setWidth(1).setHeight(1));
    probe_events.push_back({std::move(what), t, t->format, next++});
  }

  void ProbeReport() {
    if (probe_events.empty()) return;
    Flush();
    dev->waitForIdle();
    std::map<nvrhi::Format, std::pair<const uint8_t*, size_t>> maps;
    for (auto& [f, sp] : probe_staging) {
      size_t pitch = 0;
      maps[f] = {static_cast<const uint8_t*>(dev->mapStagingTexture(sp.first, nvrhi::TextureSlice().setWidth(kProbeStagingWidth).setHeight(1),
                                                                    nvrhi::CpuAccessMode::Read, &pitch)), pitch};
    }
    std::map<const Target*, std::string> last;
    for (const ProbeEvent& e : probe_events) {
      const uint8_t* p = maps[e.format].first;
      if (!p) continue;
      const nvrhi::FormatInfo& fi = nvrhi::getFormatInfo(e.format);
      std::string hex;
      char b[4];
      for (uint32_t i = 0; i < fi.bytesPerBlock; ++i) std::snprintf(b, sizeof(b), "%02X", p[e.column * fi.bytesPerBlock + i]), hex += b;
      if (e.format == nvrhi::Format::RGBA16_FLOAT) {
        auto h2f = [](uint16_t h) {
          const uint32_t s = (h >> 15) & 1, ex = (h >> 10) & 31, m = h & 1023;
          float v = ex == 0 ? std::ldexp(float(m), -24) : ex == 31 ? INFINITY : std::ldexp(float(m | 1024), int(ex) - 25);
          return s ? -v : v;
        };
        const uint16_t* q = reinterpret_cast<const uint16_t*>(p + e.column * 8);
        char f[96];
        std::snprintf(f, sizeof(f), " (%.3f %.3f %.3f %.3f)", h2f(q[0]), h2f(q[1]), h2f(q[2]), h2f(q[3]));
        hex += f;
      }
      std::string& prev = last[e.target];
      if (hex != prev)
        Log("renderer: P f%llu (%d,%d) target %08X %ux%u %s -> %s", (unsigned long long)frame, opt.probe_x, opt.probe_y,
            e.target->surface, e.target->width, e.target->height, e.what.c_str(), hex.c_str());
      prev = hex;
    }
    for (auto& [f, sp] : probe_staging) dev->unmapStagingTexture(sp.first), sp.second = 0;
    probe_events.clear();
  }

  void Log(const char* fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (opt.log) opt.log(buffer);
    else LogLine("%s", buffer);
  }
  void Once(const std::string& key, const char* fmt, const char* detail) {
    if (logged_once.size() > 4096 || !logged_once.insert(key).second) return;
    Log(fmt, detail);
  }
  bool Tracing() const { return frame >= opt.trace_first && frame < opt.trace_first + opt.trace_count; }
  void Skip(const char* reason) {
    if (Tracing()) Log("renderer: T f%llu draw skipped: %s", (unsigned long long)frame, reason);
    ++frame_skipped;
    ++total_skipped;
    ++skip_reasons[reason];
  }

  GuestDeviceView View(uint32_t device) const { return GuestDeviceView(layout, memory.Virtual(device)); }

  // --- Device ---------------------------------------------------------------------------------------------
  bool CreateHost(void* window, uint32_t w, uint32_t h, std::string* error) {
    HostOptions ho;
    ho.api = opt.api;
    ho.window = window;
    ho.width = std::max(1u, w);
    ho.height = std::max(1u, h);
    ho.vsync = opt.vsync;
    ho.debug = opt.debug;
    ho.split_present = opt.present_unlocked;
    ho.pipeline_cache_path = opt.api == HostApi::kVulkan ? opt.driver_pipeline_cache : std::string();
    const auto t0 = Clock::now();
    host = CreateHostDevice(ho, error);
    if (!host) return false;
    dev = host->device();
    api = dev->getGraphicsAPI();
    Log("renderer: device %s (%s), swapchain %s %ux%u, %.0f ms", host->description().c_str(), HostApiName(opt.api),
        host->has_swapchain() ? "yes" : "no", host->swapchain_width(), host->swapchain_height(), MsSince(t0));
    cur_scale = wanted_scale = opt.scale_auto && host->has_swapchain() ? AutoScale(host->swapchain_height()) : opt.scale;
    cur_wide = wanted_wide = host->has_swapchain() || opt.aspect > 0.0f ? WideFor(host->swapchain_width(), host->swapchain_height()) : 1.0f;
    wide_published.store(cur_wide);
    Log("renderer: Hor+ %s: aspect %s, factor %.4f (screen targets %ux720 guest -> %ux%u host)%s",
        cur_wide != 1.0f ? "on" : "off", opt.aspect > 0.0f ? "fixed" : "auto (output)", cur_wide, 1280u,
        HostSize(1280, cur_scale * cur_wide), HostSize(720, cur_scale),
        cur_wide != 1.0f ? "" : " - 16:9 or narrower: bars as before");
    char an[16];
    std::snprintf(an, sizeof(an), "%ux", opt.aniso);
    if (opt.msaa && opt.msaa_samples > 1) {
      screen_samples = std::min(opt.msaa_samples, std::max(1u, host->max_samples()));
      if (screen_samples < 2) screen_samples = 0;
    }
    char ms[64];
    if (!opt.msaa) std::snprintf(ms, sizeof(ms), "off");
    else if (!screen_samples) std::snprintf(ms, sizeof(ms), "like the console");
    else std::snprintf(ms, sizeof(ms), "screen targets %ux (requested %u, device up to %u)", screen_samples, opt.msaa_samples, host->max_samples());
    Log("renderer: graphics: scale %s%.3f (screen targets from guest width %u), shadows %.3f, anisotropy %s, MSAA %s, SMAA %s",
        opt.scale_auto ? "auto = " : "", cur_scale, opt.scale_min_width, opt.shadow_scale, opt.aniso > 1 ? an : "as the game",
        ms, opt.smaa ? "on" : "off");
    return InitObjects(error);
  }

  bool InitObjects(std::string* error) {
    TableCapacities caps;
    // Lower bound: one list (at most kMaxDrawsPerList draws plus blits) must never use up all versions, otherwise a
    // draw would get the previous one's constants (NVRHI error "insufficient").
    if (opt.constant_versions) caps.constant_versions = std::max(opt.constant_versions, 2 * kMaxDrawsPerList + 64);
    if (!tl.Init(dev, caps, error)) return false;
    std::string e;
    solid_ps = CreateBuiltinShader(dev, opt.api, BuiltinShader::kSolidPS, &e);
    blit_vs = CreateBuiltinShader(dev, opt.api, BuiltinShader::kBlitVS, &e);
    blit_ps = CreateBuiltinShader(dev, opt.api, BuiltinShader::kBlitPS, &e);
    blit_gamma_ps = CreateBuiltinShader(dev, opt.api, BuiltinShader::kBlitGammaPS, &e);
    if (!blit_gamma_ps) Log("renderer: gamma blit not available: %s", e.c_str());
    {
      nvrhi::TextureDesc d;
      d.width = 256;
      d.height = 1;
      d.format = nvrhi::Format::RGBA16_UNORM;
      d.initialState = nvrhi::ResourceStates::ShaderResource;
      d.keepInitialState = true;
      d.debugName = "gamma ramp";
      gamma_lut = dev->createTexture(d);
      if (gamma_lut) gamma_lut_slot = tl.AddTexture(Table::kTex2D, gamma_lut);
    }
    if (!solid_ps || !blit_vs || !blit_ps) return error && (*error = "built-in shaders: " + e, false);
    auto buffer = [&](uint64_t bytes, bool index, bool raw, const char* name) {
      nvrhi::BufferDesc d;
      d.byteSize = bytes;
      d.isIndexBuffer = index;
      d.canHaveRawViews = raw;
      d.initialState = index ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::ShaderResource;
      d.keepInitialState = true;
      d.debugName = name;
      return dev->createBuffer(d);
    };
    up_ring = buffer(kUpRingBytes, false, true, "DrawVerticesUP ring");
    index_ring = buffer(kIndexRingBytes, true, false, "index ring");
    quad_pattern = buffer(uint64_t(kPatternVertices) / 4 * 6 * 4, true, false, "quad pattern");
    fan_pattern = buffer(uint64_t(kPatternVertices) * 3 * 4, true, false, "fan pattern");
    up_slot = tl.AddBuffer(up_ring);
    nvrhi::SamplerHandle lin = dev->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    keep_samplers.push_back(lin);
    linear_clamp = tl.AddSampler(lin);
    const auto params = nvrhi::CommandListParameters().setEnableImmediateExecution(false);
    cl = dev->createCommandList(params);
    present_cl = dev->createCommandList(params);
    ring_cl = dev->createCommandList(params);
    {
      std::vector<uint32_t> quads, fans;
      for (uint32_t q = 0; q < kPatternVertices / 4; ++q) {
        const uint32_t v = q * 4;
        quads.insert(quads.end(), {v, v + 1, v + 2, v, v + 2, v + 3});
      }
      for (uint32_t i = 1; i < kPatternVertices; ++i) fans.insert(fans.end(), {0u, i, i + 1});
      fans.resize(size_t(kPatternVertices) * 3);
      cl->open();
      cl->writeBuffer(quad_pattern, quads.data(), quads.size() * 4);
      cl->writeBuffer(fan_pattern, fans.data(), fans.size() * 4);
      cl->close();
      dev->executeCommandList(cl);
    }
    if (opt.query_mode == 2) {
      occ = host->occlusion_queries();
      Log("renderer: Occlusion Queries: %s", occ ? "host (FCP_RENDER_QUERY=host)" : "not available, original's answer");
    }
    ready = true;
    StartPrewarm();
    return true;
  }

  bool EnsureReady() {
    if (ready) return true;
    if (failed || !memory.base) return false;
    std::string error;
    if (!CreateHost(nullptr, 0, 0, &error)) {
      failed = true;
      Log("renderer: windowless device failed: %s", error.c_str());
      return false;
    }
    return true;
  }

  nvrhi::ICommandList* CL() {
    if (!cl_open) {
      cl->open();
      cl_open = true;
      cl_draws = 0;
      ++cl_epoch;
      up_used = 0;  // the rings are in queue order, copies run after the readers before them
      index_used = 0;
      if (active_query) BeginSegment(cl, queries[active_query]);  // the query continues in the new list
    }
    return cl;
  }

  // FCP_RENDER_RING_BATCH: UP data and ring indices of a list are collected, one copy per ring in a separate list
  // before the draw list (instead of one copy per draw in the middle of the render pass).
  nvrhi::CommandListHandle ring_cl;
  std::vector<uint8_t> up_stage, index_stage;
  uint64_t ring_uploads = 0, ring_upload_bytes = 0;
  void StageRing(std::vector<uint8_t>& stage, uint64_t offset, const void* data, uint64_t bytes) {
    if (stage.size() < offset + bytes) stage.resize(offset + bytes);
    std::memcpy(stage.data() + offset, data, bytes);
  }

  void Flush() {
    if (!cl_open) return;
    if (active_query) EndSegment(cl, queries[active_query]);
    cl->close();
    if (opt.ring_batch && (!up_stage.empty() || !index_stage.empty())) {
      ring_cl->open();
      if (!up_stage.empty()) {
        ring_cl->writeBuffer(up_ring, up_stage.data(), up_stage.size());
        ring_upload_bytes += up_stage.size();
      }
      if (!index_stage.empty()) {
        ring_cl->writeBuffer(index_ring, index_stage.data(), index_stage.size());
        ring_upload_bytes += index_stage.size();
      }
      ring_cl->close();
      nvrhi::ICommandList* lists[2] = {ring_cl.Get(), cl.Get()};
      dev->executeCommandLists(lists, 2);
      up_stage.clear();
      index_stage.clear();
      ++ring_uploads;
    } else {
      dev->executeCommandList(cl);
    }
    cl_open = false;
    if (!unsubmitted.empty() || !stage_unsubmitted.empty()) {
      nvrhi::EventQueryHandle ev = dev->createEventQuery();
      dev->setEventQuery(ev, nvrhi::CommandQueue::Graphics);
      for (uint32_t qa : unsubmitted)
        if (auto it = queries.find(qa); it != queries.end()) it->second.done = ev, it->second.submitted = true;
      unsubmitted.clear();
      for (const auto& [b, i] : stage_unsubmitted)
        if (auto it = readbacks.find(b); it != readbacks.end()) it->second.stages[i].ev = ev;
      stage_unsubmitted.clear();
    }
  }

  // --- Shader ---------------------------------------------------------------------------------------------
  nvrhi::IShader* Handle(const ShaderEntry* e) {
    if (!e || e->source == ShaderSource::kFailed) return nullptr;
    auto it = shader_handles.find(e);
    if (it != shader_handles.end()) return it->second;
    nvrhi::ShaderHandle h = tl.CreateShader(*e);
    if (!h) Log("renderer: shader object %016llX failed", (unsigned long long)e->container_hash);
    return shader_handles.emplace(e, h).first->second;
  }

  // --- Sampler ------------------------------------------------------------------------------------------
  uint32_t Sampler(const uint32_t words[6], const fcshaders::SamplerBinding& b) {
    const res::TextureFetch f = res::TextureFetch::FromWords(words);
    const uint32_t mag = b.magFilter == 3 ? f.MagFilter() : b.magFilter;
    const uint32_t min = b.minFilter == 3 ? f.MinFilter() : b.minFilter;
    const uint32_t mip = b.mipFilter == 3 ? f.MipFilter() : b.mipFilter;
    uint32_t aniso = b.anisoFilter == 7 ? f.AnisoFilter() : b.anisoFilter;
    // Graphics extras: anisotropic filtering (FCP_RENDER_ANISO) only for linearly filtered samplers (point samplers,
    // e.g. lookup tables, would otherwise not stay point-sampled under D3D12). Xenos levels 1..5 = 1x..16x; the higher
    // level wins.
    if (opt.aniso > 1 && mag == 1 && min == 1) {
      uint32_t want = 1;
      while ((1u << want) <= opt.aniso && want < 5) ++want;  // 2 -> 2, 4 -> 3, 8 -> 4, 16 -> 5
      if (aniso < want || aniso > 5) aniso = want;
    }
    uint64_t key = f.ClampX() | f.ClampY() << 3 | f.ClampZ() << 6 | mag << 9 | min << 11 | mip << 13 | aniso << 15 |
                   uint64_t(f.BorderColor()) << 18 | uint64_t(uint32_t(f.LodBiasFixed()) & 0x3FF) << 20;
    if (auto it = samplers.find(key); it != samplers.end()) return it->second;
    nvrhi::SamplerDesc d;
    d.setAddressU(AddressFrom(f.ClampX())).setAddressV(AddressFrom(f.ClampY())).setAddressW(AddressFrom(f.ClampZ()));
    d.setMagFilter(mag == 1).setMinFilter(min == 1).setMipFilter(mip == 1);
    if (aniso >= 1 && aniso <= 5) d.setMaxAnisotropy(float(1u << (aniso - 1)));
    d.setMipBias(float(f.LodBiasFixed()) / 32.0f);
    d.setBorderColor(f.BorderColor() == 1 ? nvrhi::Color(1, 1, 1, 1) : nvrhi::Color(0, 0, 0, f.BorderColor() == 0 ? 0.0f : 1.0f));
    nvrhi::SamplerHandle s = dev->createSampler(d);
    const uint32_t slot = s ? tl.AddSampler(s) : 0;
    if (s) keep_samplers.push_back(s);
    samplers.emplace(key, slot);
    return slot;
  }

  // --- Write watch (SetWriteWatch) ------------------------------------------------------------------------
  // One bit per 4 KB page of physical guest memory (512 MB): set by the writing thread (OnGuestWrite), cleared when
  // the texture is checked. No lock: the SDK calls under its own.
  void (*watch_fn)(void*, uint32_t, uint32_t) = nullptr;
  void* watch_ctx = nullptr;
  std::unique_ptr<std::atomic<uint64_t>[]> dirty_pages{new std::atomic<uint64_t>[(0x20000000u >> 12) / 64]()};
  uint64_t watch_skips = 0, watch_checks = 0, watch_volatile = 0, writebacks = 0;
  uint32_t diag_watch_a = 0, diag_watch_n = 0;  // FCP_DIAG_WATCH=<physical hex>,<bytes hex>
  std::atomic<uint32_t> diag_logged{0};
  std::atomic<bool> diag_rewatch{true};
  void DiagRewatch() {
    if (diag_watch_n && watch_fn && diag_rewatch.exchange(false)) watch_fn(watch_ctx, diag_watch_a, diag_watch_n);
  }

  // Separate bits for the index cache: index and vertex data often share a page, and TakeDirty clears.
  std::unique_ptr<std::atomic<uint64_t>[]> dirty_pages_idx{new std::atomic<uint64_t>[(0x20000000u >> 12) / 64]()};
  void MarkDirty(uint32_t physical, uint32_t bytes) {
    if (!bytes) return;
    const uint32_t p0 = (physical & 0x1FFFFFFFu) >> 12, p1 = std::min<uint64_t>((uint64_t(physical & 0x1FFFFFFFu) + bytes - 1) >> 12, 0x1FFFF);
    for (uint32_t p = p0; p <= p1; ++p) {
      dirty_pages[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_relaxed);
      dirty_pages_idx[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_relaxed);
    }
  }
  // Checks and clears the bits of a range; true if a page was written.
  bool TakeDirty(uint32_t physical, uint32_t bytes) { return TakeDirtyIn(dirty_pages.get(), physical, bytes); }
  bool TakeDirtyIn(std::atomic<uint64_t>* bits, uint32_t physical, uint32_t bytes) {
    if (!bytes) return false;
    const uint32_t p0 = (physical & 0x1FFFFFFFu) >> 12, p1 = std::min<uint64_t>((uint64_t(physical & 0x1FFFFFFFu) + bytes - 1) >> 12, 0x1FFFF);
    bool any = false;
    for (uint32_t p = p0; p <= p1;) {
      const uint32_t w = p >> 6, lo = p & 63, hi = std::min<uint32_t>(63, lo + (p1 - p));
      const uint64_t mask = (hi == 63 ? ~0ull : ((1ull << (hi + 1)) - 1)) & (~0ull << lo);
      if (bits[w].load(std::memory_order_relaxed) & mask) any = true, bits[w].fetch_and(~mask, std::memory_order_relaxed);
      p += hi - lo + 1;
    }
    return any;
  }
  bool Watching() const { return watch_fn && opt.texture_watch; }

  // --- Textures -------------------------------------------------------------------------------------------
  uint64_t HashRanges(const res::TextureRanges& r) {
    uint64_t h = 0;
    auto add = [&](uint32_t a, uint32_t n) {
      if (!n) return;
      if (const uint8_t* p = phys.At(a, n)) h = Mix(h, fcshaders::containerHash(p, n));
    };
    add(r.base, r.base_bytes);
    add(r.mip, r.mip_bytes);
    return h;
  }

  bool CreateTexture(GuestTex& t, const res::HostTextureData& data, nvrhi::ICommandList* c, bool rewrite) {
    const res::HostTexturePlan& p = data.plan;
    nvrhi::TextureDesc d;
    d.width = p.width;
    d.height = p.height;
    d.mipLevels = p.levels;
    d.format = res::ToNvrhi(p.format);
    Table table = Table::kTex2D;
    bool stacked = false;
    switch (p.dimension) {
      case res::Dimension::k3D: d.dimension = nvrhi::TextureDimension::Texture3D, d.depth = p.depth, table = Table::kTex3D; break;
      case res::Dimension::kCube: d.dimension = nvrhi::TextureDimension::TextureCube, d.arraySize = 6, table = Table::kTexCube; break;
      default:
        if (p.layers > 1) d.dimension = nvrhi::TextureDimension::Texture2DArray, d.arraySize = p.layers, table = Table::kTex2DArray, stacked = true;
        else d.dimension = nvrhi::TextureDimension::Texture2D;
        break;
    }
    if (d.format == nvrhi::Format::UNKNOWN) return false;
    const bool same = t.tex && t.plan.width == p.width && t.plan.height == p.height && t.plan.levels == p.levels &&
                      t.plan.format == p.format && t.plan.layers == p.layers && t.plan.depth == p.depth &&
                      t.plan.dimension == p.dimension;
    if (!same) {
      d.initialState = nvrhi::ResourceStates::ShaderResource;
      d.keepInitialState = true;
      d.debugName = "guest texture";
      nvrhi::TextureHandle nt = dev->createTexture(d);
      if (!nt) return false;
      if (t.tex) retired.emplace_back(frame, t.tex);
      const auto mapping = res::ToNvrhiMapping(p.view_swizzle);
      if (rewrite && t.slot && t.table == table) {
        tl.WriteTextureView(table, t.slot & 0x7FFFFFFFu, nt, mapping, d.dimension);
      } else {
        const uint32_t slot = tl.AddTextureView(table, nt, mapping, d.dimension);
        if (!slot) return false;
        t.slot = slot | (stacked ? 0x80000000u : 0u);
      }
      t.tex = nt;
      t.table = table;
    }
    for (const res::HostSubresource& s : data.subresources)
      c->writeTexture(t.tex, s.layer, s.level, data.bytes.data() + s.offset, s.row_pitch, s.depth_pitch);
    c->setTextureState(t.tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    t.plan = p;
    return true;
  }

  // --- Replacement textures (button glyphs, renderer/texture_replace.h) ------------------------------------
  // After a conversion: BC2 (or DXT3 unpacked as RGBA8), one level, 2D -> the source decides.
  int ClassifyForReplace(const res::HostTextureData& data) {
    const res::HostTexturePlan& p = data.plan;
    if (!replace_source || p.dimension != res::Dimension::k2D || p.layers != 1 || p.levels != 1 || p.min_level != 0 ||
        data.subresources.empty())
      return -1;
    const bool bc2 = p.format == res::HostFormat::BC2_UNORM;
    const bool dxt3_rgba = p.format == res::HostFormat::RGBA8_UNORM && p.conversion == res::Conversion::kDXT3ToRGBA8;
    if (!bc2 && !dxt3_rgba) return -1;
    if ((p.width & 3) || (p.height & 3) || !replace_source->Wants(p.width, p.height)) return -1;
    const res::HostSubresource& s = data.subresources.front();
    std::vector<uint8_t> rgba(size_t(p.width) * p.height * 4);
    std::vector<uint8_t> blocks;
    if (bc2) {
      const uint32_t bw = p.width / 4, bh = p.height / 4;
      blocks.resize(size_t(bw) * bh * 16);
      uint8_t texels[64];
      for (uint32_t by = 0; by < bh; ++by) {
        const uint8_t* row = data.bytes.data() + s.offset + size_t(by) * s.row_pitch;
        std::memcpy(blocks.data() + size_t(by) * bw * 16, row, size_t(bw) * 16);
        for (uint32_t bx = 0; bx < bw; ++bx) {
          res::DecodeBC2(row + size_t(bx) * 16, texels);
          for (uint32_t y = 0; y < 4; ++y)
            std::memcpy(rgba.data() + (size_t(by * 4 + y) * p.width + bx * 4) * 4, texels + y * 16, 16);
        }
      }
    } else {
      for (uint32_t y = 0; y < p.height; ++y)
        std::memcpy(rgba.data() + size_t(y) * p.width * 4, data.bytes.data() + s.offset + size_t(y) * s.row_pitch,
                    size_t(p.width) * 4);
    }
    ++replace_classified;
    return replace_source->Classify(p.width, p.height, blocks.empty() ? nullptr : blocks.data(), blocks.size(), rgba.data());
  }

  // Binding of a classified texture: the source's current replacement image (uploaded and cached) or `slot`.
  uint32_t ReplacedSlot(const GuestTex& t, uint32_t slot, nvrhi::ICommandList* c) {
    const std::shared_ptr<const ReplacementImage> img = replace_source->Current(t.replace_kind, cur_scale);
    if (!img || img->rgba.size() != size_t(img->width) * img->height * 4 || !img->width) return slot;
    ++replace_binds;
    for (ReplaceTex& r : replace_cache) {
      if (r.id == img->id) {
        r.used = frame;
        return r.slot;
      }
    }
    nvrhi::TextureDesc d;
    d.width = img->width;
    d.height = img->height;
    d.format = nvrhi::Format::RGBA8_UNORM;
    d.dimension = nvrhi::TextureDimension::Texture2D;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = "replacement texture";
    nvrhi::TextureHandle tex = dev->createTexture(d);
    if (!tex) return slot;
    c->writeTexture(tex, 0, 0, img->rgba.data(), size_t(img->width) * 4);
    c->setTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    const auto mapping = res::ToNvrhiMapping(t.plan.view_swizzle);
    ReplaceTex* entry = nullptr;
    if (replace_cache.size() >= kReplaceSlots) {
      // reuse the least recently used slot (not needed in the current or the last few frames)
      ReplaceTex* oldest = nullptr;
      for (ReplaceTex& r : replace_cache)
        if (!oldest || r.used < oldest->used) oldest = &r;
      if (!oldest || oldest->used + 3 >= frame) return slot;
      retired.emplace_back(frame, oldest->tex);
      tl.WriteTextureView(Table::kTex2D, oldest->slot, tex, mapping, nvrhi::TextureDimension::Texture2D);
      entry = oldest;
    } else {
      const uint32_t s = tl.AddTextureView(Table::kTex2D, tex, mapping, nvrhi::TextureDimension::Texture2D);
      if (!s) return slot;
      replace_cache.push_back({});
      entry = &replace_cache.back();
      entry->slot = s;
    }
    entry->id = img->id;
    entry->tex = tex;
    entry->used = frame;
    ++replace_uploads;
    return entry->slot;
  }

  uint32_t Texture(const uint32_t words[6], nvrhi::ICommandList* c) {
    GuestTex* g = nullptr;
    const uint32_t slot = TextureOf(words, c, g);
    if (g && g->replace_kind >= 0 && replace_source && slot) return ReplacedSlot(*g, slot, c);
    return slot;
  }

  uint32_t TextureOf(const uint32_t words[6], nvrhi::ICommandList* c, GuestTex*& out) {
    const res::TextureFetch f = res::TextureFetch::FromWords(words);
    tex_inv_x = tex_inv_y = 1.0f;
    if (f.Type() != 2) return 0;
    if (auto it = resolved.find(f.BaseAddress()); it != resolved.end() && f.BaseAddress()) {
      Resolved& r = it->second;
      tex_inv_x = 1.0f / r.sx, tex_inv_y = 1.0f / r.sy;  // guest texels per host texel (kk_GuestSize in the shader)
      const uint16_t key = ConstantSwizzleKey(f.Swizzle());
      if (!key) return r.slot;
      if (auto v = r.views.find(key); v != r.views.end()) return v->second;
      const uint32_t slot = tl.AddTextureView(Table::kTex2D, r.tex, MappingFromConstantKey(key));
      r.views.emplace(key, slot);
      return slot;
    }
    FetchKey key;
    std::memcpy(key.w.data(), words, 24);
    auto [it, inserted] = textures.try_emplace(key);
    GuestTex& t = it->second;
    if (t.failed) return 0;
    out = &t;
    if (!inserted && (t.checked_frame == frame || !opt.texture_check)) return t.slot;
    const auto t0 = Clock::now();
    if (inserted) t.ranges = res::GetTextureRanges(f);
    const bool watch = Watching() && !t.volatile_data;
    if (watch) {
      // Watched: only hash when the SDK has reported a write to a page. Clear the bits and re-watch first, then
      // read: a write after that falls into the next check.
      const bool dirty = TakeDirty(t.ranges.base, t.ranges.base_bytes) | TakeDirty(t.ranges.mip, t.ranges.mip_bytes);
      if (!inserted && t.watched && !dirty) {
        t.checked_frame = frame;
        ++watch_skips;
        tex_check_us += uint64_t(MsSince(t0) * 1000);
        return t.slot;
      }
      if (t.ranges.base_bytes) watch_fn(watch_ctx, t.ranges.base, t.ranges.base_bytes);
      if (t.ranges.mip_bytes) watch_fn(watch_ctx, t.ranges.mip, t.ranges.mip_bytes);
      t.watched = true;
      ++watch_checks;
    }
    const uint64_t h = HashRanges(t.ranges);
    t.checked_frame = frame;
    tex_check_us += uint64_t(MsSince(t0) * 1000);
    if (!inserted && h == t.hash) return t.slot;
    if (!inserted && watch) {
      // Data that changes at short intervals (Bink planes) costs one page fault per page and frame when watched:
      // fall back to hashing every frame (changed twice in quick succession).
      if (t.last_change_frame != ~0ull && frame - t.last_change_frame <= 30) t.volatile_data = true, ++watch_volatile;
      t.last_change_frame = frame;
    }
    const auto t1 = Clock::now();
    res::HostTextureData data;
    std::string why;
    if (!res::ConvertTexture(f, phys, data, &why) || !CreateTexture(t, data, c, !inserted)) {
      if (inserted) {
        t.failed = true;
        char line[160];
        std::snprintf(line, sizeof(line), "format %u %ux%u: %s", unsigned(f.Format()), f.Width(), f.Height(), why.c_str());
        Once("tex" + std::to_string(unsigned(f.Format())), "renderer: texture not convertible (%s)", line);
      }
      return 0;
    }
    t.hash = h;
    t.replace_kind = ClassifyForReplace(data);
    tex_convert_us += uint64_t(MsSince(t1) * 1000);
    if ((Tracing() || diag_dump_textures) && !opt.dump_pattern.empty() && data.plan.format == res::HostFormat::RGBA8_UNORM &&
        t.table == Table::kTex2D && data.plan.width >= 256) {
      // Diagnostics: converted guest texture (RGBA8, level 0) from the guest data as PNG.
      const res::HostSubresource& s0 = data.subresources.front();
      std::vector<uint8_t> rgba(size_t(data.plan.width) * data.plan.height * 4);
      for (uint32_t y = 0; y < data.plan.height; ++y)
        std::memcpy(rgba.data() + size_t(y) * data.plan.width * 4, data.bytes.data() + s0.offset + size_t(y) * s0.row_pitch,
                    size_t(data.plan.width) * 4);
      char name[64];
      std::snprintf(name, sizeof(name), "_tex%08X.png", f.BaseAddress());
      std::string error;
      WriteImage(DumpPath(opt.dump_pattern, frame) + name, data.plan.width, data.plan.height, rgba, &error);
    }
    if (inserted) ++textures_converted;
    else ++textures_rewritten;
    return t.slot;
  }

  // --- Vertex buffers -------------------------------------------------------------------------------------
  uint32_t Buffer(uint32_t physical, uint32_t bytes, nvrhi::ICommandList* c) {
    if (!bytes || bytes > (64u << 20)) return 0;
    const uint8_t* data = phys.At(physical, bytes);
    if (!data) return 0;
    const uint64_t key = (uint64_t(physical) << 32) | bytes;
    auto [it, inserted] = buffers.try_emplace(key);
    GuestBuf& b = it->second;
    if (!inserted && b.checked_frame == frame) return b.slot;
    const auto t0 = Clock::now();
    const bool watch = Watching() && opt.buffer_watch && !b.volatile_data;
    if (watch) {
      const bool dirty = TakeDirty(physical, bytes);
      if (!inserted && b.watched && !dirty) {
        b.checked_frame = frame;
        buffer_us += uint64_t(MsSince(t0) * 1000);
        return b.slot;
      }
      watch_fn(watch_ctx, physical, bytes);
      b.watched = true;
    }
    const uint64_t h = fcshaders::containerHash(data, bytes);
    b.checked_frame = frame;
    if (!inserted && h == b.hash) {
      buffer_us += uint64_t(MsSince(t0) * 1000);
      return b.slot;
    }
    if (!inserted && watch) {
      if (b.last_change_frame != ~0ull && frame - b.last_change_frame <= 30) b.volatile_data = true;
      b.last_change_frame = frame;
    }
    if (inserted) {
      nvrhi::BufferDesc d;
      d.byteSize = (bytes + 3) & ~3u;
      d.canHaveRawViews = true;
      d.initialState = nvrhi::ResourceStates::ShaderResource;
      d.keepInitialState = true;
      d.debugName = "guest vertex buffer";
      b.buf = dev->createBuffer(d);
      if (!b.buf) return 0;
      b.slot = tl.AddBuffer(b.buf);
      if (!b.slot) return 0;
    }
    c->writeBuffer(b.buf, data, bytes & ~3u);
    if (bytes & 3) {
      uint8_t tail[4] = {};
      std::memcpy(tail, data + (bytes & ~3u), bytes & 3);
      c->writeBuffer(b.buf, tail, 4, bytes & ~3u);
    }
    c->setBufferState(b.buf, nvrhi::ResourceStates::ShaderResource);
    b.hash = h;
    ++buffers_written;
    frame_buffer_bytes += bytes;
    buffer_us += uint64_t(MsSince(t0) * 1000);
    return b.slot;
  }

  // --- Index cache (opt.index_cache) ---------------------------------------------------------------------
  // Whole guest index buffer as a host buffer (32 bit, 0xFFFF -> 0xFFFFFFFF as in the ring path). Only with the
  // write watch: checked before every draw (separate bits), not once per frame, so that a buffer rewritten within
  // the frame takes effect immediately. Buffers that change constantly take the old path (ring per draw).
  struct GuestIdx {
    nvrhi::BufferHandle buf;
    uint64_t hash = 0;
    bool watched = false, volatile_data = false;
    uint64_t last_change_frame = ~0ull;
  };
  std::unordered_map<uint64_t, GuestIdx> index_buffers;
  uint64_t idx_hits = 0, idx_converts = 0, idx_volatile = 0, idx_bytes = 0;
  std::vector<uint32_t> idx_scratch;
  nvrhi::IBuffer* CachedIndices(const res::IndexBufferInfo& ib, nvrhi::ICommandList* c) {
    if (!Watching() || !opt.buffer_watch || !ib.size_bytes || ib.size_bytes > (32u << 20)) return nullptr;
    const uint32_t isize = ib.index32 ? 4 : 2;
    const uint32_t n = ib.size_bytes / isize;
    if (!n) return nullptr;
    const uint8_t* data = phys.At(ib.physical, n * isize);
    if (!data) return nullptr;
    const uint64_t key = (uint64_t(ib.physical) << 32) | (uint64_t(ib.index32) << 31) | (ib.size_bytes & 0x7FFFFFFFu);
    auto [it, inserted] = index_buffers.try_emplace(key);
    GuestIdx& e = it->second;
    if (e.volatile_data) return nullptr;
    const bool dirty = TakeDirtyIn(dirty_pages_idx.get(), ib.physical, n * isize);
    if (!inserted && e.watched && !dirty) return ++idx_hits, e.buf.Get();
    watch_fn(watch_ctx, ib.physical, n * isize);
    e.watched = true;
    const uint64_t h = fcshaders::containerHash(data, size_t(n) * isize);
    if (!inserted && h == e.hash) return ++idx_hits, e.buf.Get();
    if (!inserted) {
      if (e.last_change_frame != ~0ull && frame - e.last_change_frame <= 30) {
        e.volatile_data = true;
        ++idx_volatile;
        return nullptr;
      }
      e.last_change_frame = frame;
    }
    if (inserted) {
      nvrhi::BufferDesc d;
      d.byteSize = uint64_t(n) * 4;
      d.isIndexBuffer = true;
      d.initialState = nvrhi::ResourceStates::IndexBuffer;
      d.keepInitialState = true;
      d.debugName = "guest index buffer";
      e.buf = dev->createBuffer(d);
      if (!e.buf) {
        e.volatile_data = true;  // never try again: old path
        return nullptr;
      }
    }
    idx_scratch.resize(n);
    if (ib.index32) {
      for (uint32_t i = 0; i < n; ++i) idx_scratch[i] = LoadBE32(data + size_t(i) * 4);
    } else {
      for (uint32_t i = 0; i < n; ++i) {
        const uint16_t v = LoadBE16(data + size_t(i) * 2);
        idx_scratch[i] = v == 0xFFFF ? 0xFFFFFFFFu : v;
      }
    }
    c->writeBuffer(e.buf, idx_scratch.data(), size_t(n) * 4);
    e.hash = h;
    ++idx_converts;
    idx_bytes += uint64_t(n) * 4;
    return e.buf.Get();
  }

  // --- Targets --------------------------------------------------------------------------------------------
  Target* GetTarget(uint32_t surface, bool depth) {
    if (!surface) return nullptr;
    const uint8_t* s = memory.Virtual(surface);
    if (!s) return nullptr;
    const uint32_t size_word = LoadBE32(s + lo.surf_size);
    const uint32_t surface_info = LoadBE32(s + lo.surf_info);
    const uint32_t color_info = LoadBE32(s + lo.surf_color);
    const uint32_t w = (size_word & 0x1FFF) + 1, h = ((size_word >> 13) & 0x1FFF) + 1;
    const uint32_t fmt = (color_info >> 16) & (depth ? 1 : 15);
    uint32_t want_w = w, want_h = h;
    if (in_tiling && tile_w && tile_h) want_w = std::max(want_w, tile_w), want_h = std::max(want_h, tile_h);
    auto it = targets.find(uint64_t(surface) | (uint64_t(depth) << 32));
    if (it != targets.end() && it->second.gw >= want_w && it->second.gh >= want_h && it->second.depth == depth &&
        it->second.guest_format == fmt)
      return &it->second;
    Target t;
    if (it != targets.end()) want_w = std::max(want_w, it->second.gw), want_h = std::max(want_h, it->second.gh);
    // Render scaling: logical size in guest pixels, host = guest x factor of the class.
    float tsx = 1.0f, tsy = 1.0f;
    ScaleFor(want_w, want_h, tsx, tsy);
    t.gw = want_w;
    t.gh = want_h;
    t.width = HostSize(want_w, tsx);
    t.height = HostSize(want_h, tsy);
    t.sx = float(t.width) / float(t.gw);
    t.sy = float(t.height) / float(t.gh);
    t.depth = depth;
    t.guest_format = fmt;
    t.surface = surface;
    t.surface_w = w;
    t.surface_h = h;
    t.msaa = (surface_info >> 16) & 3;
    t.base = color_info & 0xFFF;
    t.format = depth ? nvrhi::Format::D32S8 : ColorTargetFormat(fmt);
    // FCP_RENDER_MSAA=1: host target with the guest's MSAA level (RB_SURFACE_INFO), otherwise without MSAA.
    t.host_samples = opt.msaa && t.msaa ? (1u << std::min(t.msaa, 2u)) : 1u;
    // [graphics] msaa = 2|4|8: screen targets with guest MSAA (colour and depth alike) use the chosen level.
    // Queries compute with host_samples, resolves average in hardware as before.
    if (t.host_samples > 1 && screen_samples && IsScreenTarget(want_w, want_h)) t.host_samples = screen_samples;
    nvrhi::TextureDesc d;
    d.width = t.width;
    d.height = t.height;
    d.format = t.format;
    d.sampleCount = t.host_samples;
    if (t.host_samples > 1) d.dimension = nvrhi::TextureDimension::Texture2DMS;
    d.isRenderTarget = true;
    d.initialState = depth ? nvrhi::ResourceStates::DepthWrite : nvrhi::ResourceStates::RenderTarget;
    d.keepInitialState = true;
    d.setClearValue(depth ? nvrhi::Color(1.0f, 0.0f, 0.0f, 0.0f) : nvrhi::Color(0.0f));
    d.debugName = depth ? "guest depth" : "guest color";
    t.tex = dev->createTexture(d);
    if (!t.tex) return nullptr;
    // Colour targets for resolve blits, depth targets for depth resolves (shadow map, D32S8 -> R32F).
    if (t.host_samples > 1) {
      // Readable single-sample copy for resolves (colour: hardware resolve; depth: not supported).
      if (!depth) {
        nvrhi::TextureDesc sd = d;
        sd.sampleCount = 1;
        sd.dimension = nvrhi::TextureDimension::Texture2D;
        sd.initialState = nvrhi::ResourceStates::ShaderResource;
        sd.debugName = "guest color (resolved)";
        t.single = dev->createTexture(sd);
        if (t.single) t.srv_slot = tl.AddTexture(Table::kTex2D, t.single);
      }
    } else {
      t.srv_slot = tl.AddTexture(Table::kTex2D, t.tex);
    }
    if (logged_surfaces.insert(surface).second && logged_surfaces.size() < 64)
      Log("renderer: target %08X %s %ux%u (surface %ux%u, format %u, MSAA %u, EDRAM %03X, pitch %u) -> host %ux%u%s", surface,
          depth ? "depth" : "color", t.gw, t.gh, w, h, fmt, t.msaa, t.base, surface_info & 0x3FFF, t.width, t.height,
          in_tiling ? " (Tiling)" : "");
    if (it != targets.end()) {
      framebuffers.clear();
      it->second = std::move(t);
      return &it->second;
    }
    return &targets.emplace(uint64_t(surface) | (uint64_t(depth) << 32), std::move(t)).first->second;
  }

  nvrhi::IFramebuffer* Framebuffer(const std::array<Target*, 4>& colors, Target* depth) {
    std::array<nvrhi::ITexture*, 5> key{};
    nvrhi::FramebufferDesc d;
    for (int i = 0; i < 4; ++i)
      if (colors[i]) key[i] = colors[i]->tex, d.addColorAttachment(colors[i]->tex);
    if (depth) key[4] = depth->tex, d.setDepthAttachment(depth->tex);
    if (auto it = framebuffers.find(key); it != framebuffers.end()) return it->second;
    nvrhi::FramebufferHandle fb = dev->createFramebuffer(d);
    return framebuffers.emplace(key, fb).first->second;
  }

  // --- Viewport (King Kong ComputeViewport) -------------------------------------------------------------
  ViewportSetup ComputeViewport(const GuestDeviceView& d, uint32_t width, uint32_t height) const {
    ViewportSetup v;
    const uint32_t vte = d.u32(F::pa_cl_vte_cntl);
    const uint32_t clip = d.u32(F::pa_cl_clip_cntl);
    const uint32_t mode = d.u32(F::pa_su_sc_mode_cntl);
    const uint32_t vtx = d.u32(F::pa_su_vtx_cntl);
    const float sx = (vte & 1) ? d.f32(F::pa_cl_vport, 0) : 1.0f;
    const float ox = (vte & 2) ? d.f32(F::pa_cl_vport, 1) : 0.0f;
    const float sy = (vte & 4) ? d.f32(F::pa_cl_vport, 2) : 1.0f;
    const float oy = (vte & 8) ? d.f32(F::pa_cl_vport, 3) : 0.0f;
    const float sz = (vte & 16) ? d.f32(F::pa_cl_vport, 4) : 1.0f;
    const float oz = (vte & 32) ? d.f32(F::pa_cl_vport, 5) : 0.0f;
    float add_x = 0.0f, add_y = 0.0f;
    const uint32_t window_offset = d.u32(F::pa_sc_window_offset);
    const int32_t wx = int32_t(window_offset << 17) >> 17;
    const int32_t wy = int32_t((window_offset >> 16) << 17) >> 17;
    if (mode & (1u << 16)) add_x += float(wx), add_y += float(wy);
    if ((vtx & 1) == 0) add_x += 0.5f, add_y += 0.5f;
    const float w = float(std::max(width, 1u)), h = float(std::max(height, 1u));
    v.ndc_scale[0] = sx * 2.0f / w;
    v.ndc_offset[0] = (ox + add_x) * 2.0f / w - 1.0f;
    v.ndc_scale[1] = -sy * 2.0f / h;
    v.ndc_offset[1] = 1.0f - (oy + add_y) * 2.0f / h;
    v.viewport = nvrhi::Viewport(0.0f, w, 0.0f, h, 0.0f, 1.0f);
    if (clip & (1u << 16)) {
      v.ndc_scale[2] = sz;
      v.ndc_offset[2] = oz;
    } else {
      float z0 = std::clamp(oz, 0.0f, 1.0f), z1 = std::clamp(oz + sz, 0.0f, 1.0f);
      if (z0 > z1) {
        std::swap(z0, z1);
        v.ndc_scale[2] = -1.0f;
        v.ndc_offset[2] = 1.0f;
      }
      v.viewport.minZ = z0;
      v.viewport.maxZ = z1;
    }
    const uint32_t tl_ = d.u32(F::pa_sc_window_scissor_tl), br = d.u32(F::pa_sc_window_scissor_br);
    int32_t x0 = int32_t(tl_ & 0x7FFF), y0 = int32_t((tl_ >> 16) & 0x7FFF);
    int32_t x1 = int32_t(br & 0x7FFF), y1 = int32_t((br >> 16) & 0x7FFF);
    if (!(tl_ & 0x80000000u)) x0 += wx, x1 += wx, y0 += wy, y1 += wy;
    x0 = std::clamp(x0, 0, int32_t(width));
    x1 = std::clamp(x1, 0, int32_t(width));
    y0 = std::clamp(y0, 0, int32_t(height));
    y1 = std::clamp(y1, 0, int32_t(height));
    // Without tiles the whole pass applies: a window scissor that the library sets to just one tile would otherwise
    // cut off parts of the frame.
    if (in_tiling) x0 = 0, y0 = 0, x1 = int32_t(width), y1 = int32_t(height);
    if (x1 <= x0 || y1 <= y0) x0 = 0, y0 = 0, x1 = int32_t(width), y1 = int32_t(height);
    v.scissor = nvrhi::Rect(x0, x1, y0, y1);
    return v;
  }

  // --- Hor+: classification per draw ------------------------------------------------------------------------
  // Box (x0, y0, x1, y1) of a draw's positions as the vertices hold them: element POSITION 0 as floats (the game's 2D
  // draws carry guest pixels, measured; the caller recognises a quad in clip space -1..1), from the UP data or the
  // vertex buffer, at most 4096 vertices. false: not readable (no float, another kind).
  bool DrawBox(const DrawRecord& r, const GuestDeviceView& d, float box[4]) {
    const res::VertexElement* pos = nullptr;
    for (const res::VertexElement& e : elements)
      if (e.usage == 0 && e.usage_index == 0) {
        pos = &e;
        break;
      }
    if (!pos || pos->type.EndianMode() != res::Endian::k8in32) return false;
    const uint32_t f = uint32_t(pos->type.Format());
    if (f != 37 && f != 38 && f != 57) return false;  // k_32_32_FLOAT, k_32_32_32_32_FLOAT, k_32_32_32_FLOAT
    const uint8_t* base = nullptr;
    uint32_t stride = 0, n = 0;
    if (r.kind == kDrawVerticesUP) {
      if (pos->stream != 0 || !r.up_data || !r.up_stride) return false;
      stride = r.up_stride;
      n = std::min(r.count, r.up_bytes / r.up_stride);
      if (pos->offset + 8u > stride) return false;
      base = r.up_data + pos->offset;
    } else {
      const uint8_t* obj = memory.Virtual(d.u32(F::stream_buffer, pos->stream));
      res::VertexBufferInfo info;
      if (!obj || !res::DecodeVertexBuffer(obj, info)) return false;
      info.physical = res::CpuToPhysical(res::VertexFetch::FromGuest(obj + lo.vb_fetch).Address());
      stride = uint32_t(d.u8(F::stream_stride, pos->stream)) * 4;
      if (!stride || pos->offset + 8u > stride) return false;
      uint32_t first = r.start;
      n = r.count;
      if (r.kind == kDrawIndexed) {
        res::IndexBufferInfo ib;
        const uint8_t* io = memory.Virtual(d.u32(F::index_buffer));
        if (!io || !res::DecodeIndexBuffer(io, ib) || r.count > 4096) return false;
        const uint32_t isize = ib.index32 ? 4 : 2;
        const uint8_t* src = phys.At(ib.physical + r.start * isize, r.count * isize);
        if (!src) return false;
        uint32_t lo_i = ~0u, hi_i = 0;
        for (uint32_t i = 0; i < r.count; ++i) {
          const uint32_t v = ib.index32 ? LoadBE32(src + size_t(i) * 4) : LoadBE16(src + size_t(i) * 2);
          if (v == 0xFFFF && !ib.index32) continue;
          lo_i = std::min(lo_i, v), hi_i = std::max(hi_i, v);
        }
        if (lo_i > hi_i) return false;
        first = uint32_t(int32_t(lo_i) + r.base_vertex);
        n = hi_i - lo_i + 1;
      }
      n = std::min(n, 4096u);
      base = phys.At(info.physical + d.u32(F::stream_offset, pos->stream) + pos->offset + first * stride, n * stride);
    }
    if (!base || !n) return false;
    n = std::min(n, 4096u);
    box[0] = box[1] = 1e30f, box[2] = box[3] = -1e30f;
    for (uint32_t i = 0; i < n; ++i) {
      const float x = LoadBEFloat(base + size_t(i) * stride), y = LoadBEFloat(base + size_t(i) * stride + 4);
      if (!std::isfinite(x) || !std::isfinite(y)) return false;
      box[0] = std::min(box[0], x), box[1] = std::min(box[1], y), box[2] = std::max(box[2], x), box[3] = std::max(box[3], y);
    }
    return true;
  }

  // Decides whether a draw belongs in the 16:9 middle, and keeps the picture's state:
  // - 3D view: a draw with depth test into a screen-sized target (measured in 21_dunea: the whole world goes with
  //   depth test into the tiling target 1280x736, then a resolve, bloom in 976x360 and smaller, compositing and the
  //   HUD without depth test into the target 1280x720).
  // - Post processing: begins with the first resolve from the 3D view's target (ResolveRect).
  // - After that every 2D draw (no depth test) into another target of the view's width goes into the middle, unless
  //   it spans the whole width and is a single quad or covers the whole height as well (compositing, bloom, fades,
  //   bars; box in guest pixels or a quad in clip space).
  bool HorPlusNarrow(const DrawRecord& r, const GuestDeviceView& d, const ShaderEntry* vs, const ShaderEntry* ps, const Target* color,
                     const Target* depth, uint32_t depth_control, uint32_t color_mask) {
    if (!color || !IsScreenTarget(color->gw, color->gh)) return false;
    const bool depth_test = depth && (depth_control & 2);
    if (depth_test) {
      if (!frame_3d) frame_3d = true, view_surface = color->surface, view_gw = color->gw;
      return false;
    }
    // (Not into the view's own target: after a resolve in the middle of the scene more world draws without depth test
    // go there, measured VS 7AFF2B5C in 21_dunea; the HUD goes into its own target 1280x720.)
    if (!frame_3d || !post_3d || color->surface == view_surface || color->gw != view_gw || !(color_mask & 0xF)) return false;
    float box[4];
    const bool known = DrawBox(r, d, box);
    bool full = false;
    if (known) {
      const float w = float(color->gw), h = float(color->gh);
      const bool single = r.count <= 6;
      const bool clip = single && box[0] <= -0.99f && box[2] >= 0.99f && box[2] - box[0] <= 2.5f && box[1] <= -0.99f &&
                        box[3] >= 0.99f && box[3] - box[1] <= 2.5f;
      const bool across = box[0] <= 1.0f && box[2] >= w - 1.0f;
      const bool tall = box[1] <= 1.0f && box[3] >= h - 1.0f;
      full = clip || (across && (single || tall));
    } else {
      ++unknown_boxes;
    }
    const bool narrow = !full;
    ++(narrow ? narrow_draws : full_draws);
    if (opt.trace_wide && wide_traced.size() < 4000) {
      const uint64_t key = Mix(Mix(vs->container_hash, ps ? ps->container_hash : 0),
                               (known ? uint64_t(int32_t(box[0]) >> 3 & 0xFFF) << 36 | uint64_t(int32_t(box[2]) >> 3 & 0xFFF) << 24 |
                                            uint64_t(int32_t(box[1]) >> 3 & 0xFFF) << 12 | uint64_t(int32_t(box[3]) >> 3 & 0xFFF)
                                      : 0xFFFFFFFFFFull) ^ uint64_t(narrow) << 62 ^ uint64_t(r.kind) << 60);
      if (wide_traced.insert(key).second)
        Log("renderer: hor+ f%llu %s VS %016llX PS %016llX kind %u prim %u n=%u target %ux%u %08X box %s", (unsigned long long)frame,
            narrow ? "middle" : "wide", (unsigned long long)vs->container_hash, (unsigned long long)(ps ? ps->container_hash : 0), r.kind,
            r.primitive, r.count, color->gw, color->gh, color->surface,
            known ? (std::to_string(box[0]) + "," + std::to_string(box[1]) + " .. " + std::to_string(box[2]) + "," + std::to_string(box[3])).c_str()
                  : "unknown");
    }
    return narrow;
  }
  uint32_t view_gw = 0;

  // --- Draw -----------------------------------------------------------------------------------------------
  void Draw(const DrawRecord& r) {
    const auto t_draw = Clock::now();
    ++frame_calls;
    const GuestDeviceView d = View(r.device);
    if (!d.valid()) return Skip("no device");
    const ShaderEntry* vs = shaders.Find(d.u32(F::vertex_shader));
    const uint32_t ps_object = d.u32(F::pixel_shader);
    const ShaderEntry* ps = ps_object ? shaders.Find(ps_object) : nullptr;
    if (!vs) return Skip("VS unknown");
    nvrhi::IShader* vs_h = Handle(vs);
    if (!vs_h) return Skip("VS failed");
    nvrhi::IShader* ps_h = nullptr;
    bool solid = false;
    if (ps_object) {
      ps_h = Handle(ps);
      if (!ps_h) ps_h = solid_ps, solid = true;
    }
    const Switches& sw = Switches::Get();
    if (sw.IsMarked(vs->container_hash, ps ? ps->container_hash : 0)) ps_h = solid_ps, solid = true;
    if (ps && opt.skip_ps.count(ps->container_hash)) return Skip("FCP_RENDER_SKIP_PS");
    if (diag_only_draw >= 0 && vs->container_hash == opt.trace_consts && int64_t(frame_calls) - 1 != diag_only_draw)
      return Skip("FCP_DIAG_ONLY_DRAW");
    if (conditional_id >= 0) {
      ++frame_conditional;
      if (opt.conditional_mode == 1) return Skip("conditional (FCP_RENDER_CONDITIONAL=1)");
    }

    enum class Expand { kNone, kQuads, kFan } expand = Expand::kNone;
    nvrhi::PrimitiveType topology;
    switch (r.primitive) {
      case kPrimTriangleList: topology = nvrhi::PrimitiveType::TriangleList; break;
      case kPrimTriangleStrip: topology = nvrhi::PrimitiveType::TriangleStrip; break;
      case kPrimTriangleFan: topology = nvrhi::PrimitiveType::TriangleList, expand = Expand::kFan; break;
      case kPrimQuadList: topology = nvrhi::PrimitiveType::TriangleList, expand = Expand::kQuads; break;
      case kPrimPointList: topology = nvrhi::PrimitiveType::PointList; break;
      case kPrimLineList: topology = nvrhi::PrimitiveType::LineList; break;
      case kPrimLineStrip: topology = nvrhi::PrimitiveType::LineStrip; break;
      default: return Skip("primitive type (rect list)");
    }
    if (!r.count) return Skip("empty");

    std::array<Target*, 4> colors{};
    Target* depth = nullptr;
    for (uint32_t i = 0; i < 4; ++i) colors[i] = GetTarget(d.u32(F::render_targets, i), false);
    depth = GetTarget(d.u32(F::depth_stencil), true);
    Target* size_from = colors[0] ? colors[0] : depth;
    for (Target* c : colors)
      if (!size_from && c) size_from = c;
    if (!size_from) return Skip("no target");
    // All attachments the same size: smaller targets (depth of another size) are left out.
    for (Target*& c : colors)
      if (c && (c->width != size_from->width || c->height != size_from->height)) c = nullptr;
    if (depth && (depth->width != size_from->width || depth->height != size_from->height)) depth = nullptr;
    // All attachments with the same sample count (MSAA): mismatching ones are left out.
    for (Target*& c : colors)
      if (c && c->host_samples != size_from->host_samples) c = nullptr;
    if (depth && depth->host_samples != size_from->host_samples) depth = nullptr;
    nvrhi::IFramebuffer* fb = Framebuffer(colors, depth);
    if (!fb) return Skip("Framebuffer");

    const uint32_t color_mask = d.u32(F::rb_color_mask);
    const uint32_t depth_control = d.u32(F::rb_depth_control);
    const uint32_t mode = d.u32(F::pa_su_sc_mode_cntl);
    const uint32_t clip_cntl = d.u32(F::pa_cl_clip_cntl);
    const uint32_t ref_mask = d.u32(F::rb_stencil_ref_mask);
    if ((mode & 1) && (mode & 2)) return Skip("all culled");

    // Pipeline record: everything that determines the pipeline. The key is its hash; the same record is stored in the
    // pipeline list on disk (prewarmed at the next start, StartPrewarm).
    PipeRec rec{};
    rec.vs = vs->container_hash;
    rec.ps = ps_h == solid_ps ? kSolidPsKey : (ps ? ps->container_hash : 0);
    rec.topology = uint32_t(topology);
    for (uint32_t i = 0; i < 4; ++i) {
      if (!colors[i]) continue;
      uint32_t m = (color_mask >> (4 * i)) & 0xF;
      if (!solid && (!ps || !(ps->reflection.pixelOutputs & (1u << i)))) m = 0;
      if (solid && i == 0) m = 0xF;
      rec.masks[i] = m;
      rec.controls[i] = solid ? 0x00010001u : (i == 0 ? d.u32(F::rb_blend_control0) : d.u32(F::rb_blend_control1, i - 1));
      rec.formats[i] = uint32_t(colors[i]->format);
    }
    if (depth) {
      rec.depth_format = uint32_t(depth->format);
      rec.depth_control = depth_control;
      rec.ref_mask = ref_mask & 0xFFFF00u;
    }
    rec.mode = mode & 0x80F;
    rec.pad = size_from->host_samples > 1 ? size_from->host_samples : 0;
    rec.clip = clip_cntl & (1u << 16);
    if (depth && (mode & (1u << 11))) {
      const float ps0 = d.f32(F::pa_su_poly_offset, 0), po = d.f32(F::pa_su_poly_offset, 1);
      std::memcpy(&rec.poly_scale, &ps0, 4);
      std::memcpy(&rec.poly_offset, &po, 4);
    }
    const uint64_t key = rec.Key();
    nvrhi::IGraphicsPipeline* pipeline = nullptr;
    if (auto it = pipelines.find(key); it != pipelines.end()) {
      pipeline = it->second;
      Counters::Get().pipelines_cached++;
    } else {
      const auto t0 = Clock::now();
      nvrhi::GraphicsPipelineHandle p = CreatePipelineFrom(rec, vs_h, ps_h);
      Counters& cn = Counters::Get();
      cn.pipeline_create_us += uint64_t(MsSince(t0) * 1000);
      if (p) cn.pipelines_created++, ++frame_new_pipelines;
      pipeline = pipelines.emplace(key, p).first->second;
      if (p && !solid) AppendPipelineRecord(rec);
    }
    if (!pipeline) return Skip("pipeline failed");

    nvrhi::ICommandList* c = CL();
    fcshaders::DrawConstants dc{};
    for (uint32_t i = 0; i < 8; ++i) dc.boolConstants[i] = d.u32(F::bool_constants, i);
    for (uint32_t i = 0; i < 32; ++i) dc.loopConstants[i] = d.u32(F::loop_constants, i);
    auto bind_textures = [&](const ShaderEntry& s) {
      for (const fcshaders::TextureUse& t : s.reflection.textures) {
        uint32_t words[6];
        d.fetch_constant(t.slot & 31, words);
        dc.textureIndex[t.slot & 31] = Texture(words, c);
        scale_c.texInvScale[t.slot & 31][0] = tex_inv_x;
        scale_c.texInvScale[t.slot & 31][1] = tex_inv_y;
      }
    };
    // b3 (render scaling): factor of the target; per fetch slot guest/host of the bound texture (otherwise 1).
    for (auto& s : scale_c.texInvScale) s[0] = s[1] = s[2] = s[3] = 1.0f;
    scale_c.renderScale[0] = size_from->sx, scale_c.renderScale[1] = size_from->sy;
    scale_c.renderScale[2] = 1.0f / size_from->sx, scale_c.renderScale[3] = 1.0f / size_from->sy;
    bind_textures(*vs);
    if (ps && !solid) bind_textures(*ps);
    for (size_t i = 0; i < vs->reflection.samplers.size() && i < 32; ++i) {
      uint32_t words[6];
      d.fetch_constant(vs->reflection.samplers[i].slot & 31, words);
      dc.vertexSamplers[i] = Sampler(words, vs->reflection.samplers[i]);
    }
    if (ps && !solid)
      for (size_t i = 0; i < ps->reflection.samplers.size() && i < 32; ++i) {
        uint32_t words[6];
        d.fetch_constant(ps->reflection.samplers[i].slot & 31, words);
        dc.pixelSamplers[i] = Sampler(words, ps->reflection.samplers[i]);
      }

    // Vertex data
    uint64_t up_base = 0;
    if (r.kind == kDrawVerticesUP) {
      if (!r.up_data || !r.up_bytes) return Skip("UP without data");
      const uint64_t bytes = (uint64_t(r.up_bytes) + 255) & ~255ull;
      if (up_used + bytes > kUpRingBytes) {
        Flush();
        c = CL();
      }
      up_base = up_used;
      if (opt.ring_batch) {
        StageRing(up_stage, up_base, r.up_data, r.up_bytes);
      } else {
        c->writeBuffer(up_ring, r.up_data, r.up_bytes, up_base);
        c->setBufferState(up_ring, nvrhi::ResourceStates::ShaderResource);
      }
      up_used += bytes;
    }
    elements.clear();
    if (const uint8_t* decl = memory.Virtual(d.u32(F::vertex_declaration))) res::DecodeVertexDeclaration(decl, elements);
    for (size_t b = 0; b < vs->reflection.vertexBindings.size() && b < 16; ++b) {
      const fcshaders::VertexBinding& vb = vs->reflection.vertexBindings[b];
      if (vb.raw) continue;  // instruction mode: container shaders have none
      const res::VertexElement* element = nullptr;
      for (const res::VertexElement& e : elements)
        if (e.usage == uint8_t(vb.usage) && e.usage_index == vb.usageIndex) {
          element = &e;
          break;
        }
      if (!element) {
        if (Tracing()) Log("renderer: T   binding %zu usage %u/%u missing from the declaration (%zu elements)", b, unsigned(vb.usage), vb.usageIndex, elements.size());
        continue;
      }
      const uint32_t stream = element->stream;
      if (stream >= 16) continue;
      uint32_t slot = 0, offset = 0, stride = 0, endian = 2;
      if (r.kind == kDrawVerticesUP) {
        if (stream != 0) continue;
        slot = up_slot;
        offset = uint32_t(up_base);
        stride = r.up_stride;
      } else {
        const uint32_t vb_object = d.u32(F::stream_buffer, stream);
        res::VertexBufferInfo info;
        const uint8_t* obj = memory.Virtual(vb_object);
        if (!obj || !res::DecodeVertexBuffer(obj, info)) continue;
        // The fetch constant in the object carries a CPU address; the 0xE0000000 view lies 4 KB before physical
        // memory (kk/backend/guest_layout.h). DecodeVertexBuffer only masks.
        info.physical = res::CpuToPhysical(res::VertexFetch::FromGuest(obj + lo.vb_fetch).Address());
        slot = Buffer(info.physical, info.size_bytes, c);
        if (!slot) continue;
        offset = d.u32(F::stream_offset, stream);
        stride = uint32_t(d.u8(F::stream_stride, stream)) * 4;
        endian = opt.element_endian ? uint32_t(element->type.EndianMode()) : uint32_t(info.endian);
      }
      const res::DeclType type = element->type;
      using namespace fcshaders::vertex_format;
      dc.vertexFetch[b][0] = slot;
      dc.vertexFetch[b][1] = offset + element->offset;
      dc.vertexFetch[b][2] = stride;
      dc.vertexFetch[b][3] = uint32_t(type.Format()) | (type.Signed() ? kSigned : 0) | (type.Integer() ? kInteger : 0) |
                             (endian << kEndianShift) | (uint32_t(type.Swizzle()) << kSwizzleShift);
      if (Tracing() && r.kind == kDrawVerticesUP) {
        std::string raw;
        for (uint32_t k = 0; k < 4 && element->offset + 4 * k + 4 <= r.up_stride; ++k) {
          char w[16];
          std::snprintf(w, sizeof(w), " %08X", LoadBE32(r.up_data + element->offset + 4 * k));
          raw += w;
        }
        Log("renderer: T   UP binding %zu usage %u/%u off %u stride %u type %08X n %u v0:%s", b, unsigned(vb.usage),
            vb.usageIndex, element->offset, stride, element->type.value, r.count, raw.c_str());
      }
      if (Tracing() && r.kind != kDrawVerticesUP) {
        const uint32_t vb_object = d.u32(F::stream_buffer, stream);
        const uint8_t* obj = memory.Virtual(vb_object);
        const res::VertexFetch vf = obj ? res::VertexFetch::FromGuest(obj + 12) : res::VertexFetch{};
        // Diagnostics: all vertex data of this draw's buffer into a file (FCP_TRACE_CONSTS = VS hash).
        if (opt.trace_consts == vs->container_hash && b == 0 && !opt.dump_pattern.empty()) {
          const uint32_t pa = res::CpuToPhysical(vf.Address());
          if (const uint8_t* p = phys.At(pa, vf.SizeBytes())) {
            const std::string path = DumpPath(opt.dump_pattern, frame) + "_vb" + std::to_string(frame_draws) + ".bin";
            if (FILE* fo = std::fopen(path.c_str(), "wb")) {
              std::fwrite(p, 1, vf.SizeBytes(), fo);
              std::fclose(fo);
            }
          }
        }
        std::string raw;
        const uint32_t phys0 = res::CpuToPhysical(vf.Address()) + offset + element->offset;
        if (const uint8_t* p = phys.At(phys0, 16))
          for (int k = 0; k < 4; ++k) {
            char w[16];
            std::snprintf(w, sizeof(w), " %08X", LoadBE32(p + 4 * k));
            raw += w;
          }
        Log("renderer: T   binding %zu usage %u/%u stream %u vb %08X fetch %08X %08X off %u+%u stride %u type %08X slot %u v0:%s",
            b, unsigned(vb.usage), vb.usageIndex, stream, vb_object, vf.words[0], vf.words[1], offset, element->offset, stride,
            type.value, slot, raw.c_str());
      }
    }
    if (!(clip_cntl & (1u << 16))) dc.clipPlaneMask = clip_cntl & 0x3F;
    for (uint32_t p = 0; p < 6; ++p)
      if (const uint8_t* cp = d.at(F::clip_planes, p))
        for (uint32_t k = 0; k < 4; ++k) dc.clipPlanes[p][k] = LoadBEFloat(cp + 4 * k);
    // NDC from the guest viewport in guest pixels (independent of render scaling); host viewport and scissor in host
    // pixels (at factor 1 the same values as before).
    ViewportSetup vp = ComputeViewport(d, size_from->gw, size_from->gh);
    if (size_from->sx != 1.0f || size_from->sy != 1.0f) {
      vp.viewport.minX = 0.0f, vp.viewport.maxX = float(size_from->width);
      vp.viewport.minY = 0.0f, vp.viewport.maxY = float(size_from->height);
      vp.scissor = nvrhi::Rect(ScaleEdge(vp.scissor.minX, size_from->sx), ScaleEdge(vp.scissor.maxX, size_from->sx),
                               ScaleEdge(vp.scissor.minY, size_from->sy), ScaleEdge(vp.scissor.maxY, size_from->sy));
    }
    // Hor+: 2D over a wide 3D picture after its post processing into the 16:9 middle.
    if (cur_wide != 1.0f && HorPlusNarrow(r, d, vs, ps, colors[0], depth, depth_control, color_mask)) {
      const float cx = float(size_from->width) * 0.5f, k = 1.0f / cur_wide;
      vp.viewport.minX = cx - cx * k;
      vp.viewport.maxX = cx + cx * k;
      auto nx = [&](int32_t x) { return int32_t(std::lround(cx + (float(x) - cx) * k)); };
      vp.scissor.minX = nx(vp.scissor.minX);
      vp.scissor.maxX = nx(vp.scissor.maxX);
    }
    for (uint32_t k = 0; k < 3; ++k) dc.ndcScale[k] = vp.ndc_scale[k], dc.ndcOffset[k] = vp.ndc_offset[k];
    TranslatedLayout::AdjustNdcForApi(api, dc);
    const uint32_t color_control = d.u32(F::rb_color_control);
    if (color_control & 8) {
      dc.alphaFunc = (color_control & 7) + 1;
      dc.alphaRef = d.f32(F::rb_alpha_ref);
    }
    if (solid) {
      const uint32_t col = sw.solid_color;
      const float f[4] = {((col >> 24) & 255) / 255.0f, ((col >> 16) & 255) / 255.0f, ((col >> 8) & 255) / 255.0f, (col & 255) / 255.0f};
      std::memcpy(dc.clipPlanes[0], f, sizeof(f));
      dc.clipPlaneMask = 0;
      Counters::Get().draws_marked++;
    }
    // Constants: big-endian in the device -> little-endian floats
    auto swap_block = [&](F field, std::array<float, 1024>& out) {
      const uint8_t* p = d.at(field);
      if (!p) return;
      for (uint32_t i = 0; i < 1024; ++i) {
        const uint32_t u = LoadBE32(p + i * 4);
        std::memcpy(&out[i], &u, 4);
      }
    };
    // Fast path: only convert when the guest bytes have changed; upload as before on a new list or a change
    // (bit-exact instead of a float comparison). Diagnostics that modify or read vs_c take the old path.
    const bool fast_consts = opt.fast_constants && !opt.trace_consts && !diag_zero_translation;
    bool vs_changed = false, ps_changed = false;
    auto fast_block = [&](F field, std::array<float, 1024>& out, std::array<uint8_t, 4096>& raw, bool& valid) {
      const uint8_t* p = d.at(field);
      if (!p) return false;
      if (valid && std::memcmp(p, raw.data(), raw.size()) == 0) return ++fast_const_skips, false;
      std::memcpy(raw.data(), p, raw.size());
      valid = true;
      for (uint32_t i = 0; i < 1024; ++i) {
        const uint32_t u = LoadBE32(raw.data() + i * 4);
        std::memcpy(&out[i], &u, 4);
      }
      ++fast_const_converts;
      return true;
    };
    if (fast_consts) {
      vs_changed = fast_block(F::vs_constants, vs_c, raw_vs, raw_vs_valid);
      ps_changed = fast_block(F::ps_constants, ps_c, raw_ps, raw_ps_valid);
    } else {
      swap_block(F::vs_constants, vs_c);
      swap_block(F::ps_constants, ps_c);
      raw_vs_valid = raw_ps_valid = false;
    }
    if (opt.trace_consts && vs->container_hash == opt.trace_consts && frame % 30 == 0 && consts_logged_frame != frame) {
      consts_logged_frame = frame;
      Log("renderer: K f%llu c0 (%g %g %g %g) c1 (%g %g %g %g) c3 (%g %g %g %g) c12 (%g %g %g)", (unsigned long long)frame,
          vs_c[0], vs_c[1], vs_c[2], vs_c[3], vs_c[4], vs_c[5], vs_c[6], vs_c[7], vs_c[12], vs_c[13], vs_c[14], vs_c[15],
          vs_c[48], vs_c[49], vs_c[50]);
    }
    if (diag_zero_translation && vs->container_hash == opt.trace_consts)
      vs_c[88 * 4 + 3] = vs_c[89 * 4 + 3] = vs_c[90 * 4 + 3] = 0.0f;  // diagnostics: remove the instance translation
    if (opt.trace_consts && vs->container_hash == opt.trace_consts && frame % 30 == 0 && size_from->width >= 1280) {
      const float dx = vs_c[88 * 4 + 3] - vs_c[48], dy = vs_c[89 * 4 + 3] - vs_c[49], dz = vs_c[90 * 4 + 3] - vs_c[50];
      const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (dist < k_min_dist) k_min_dist = dist, k_min_t = {vs_c[88 * 4 + 3], vs_c[89 * 4 + 3], vs_c[90 * 4 + 3], vs_c[48], vs_c[49], vs_c[50]};
    }
    if (Tracing() && opt.trace_consts && (vs->container_hash == opt.trace_consts || (ps && ps->container_hash == opt.trace_consts))) {
      const bool vsk = vs->container_hash == opt.trace_consts;
      const std::array<float, 1024>& cc = vsk ? vs_c : ps_c;
      for (uint32_t i = 0; i < 256; ++i)
        if (cc[i * 4] || cc[i * 4 + 1] || cc[i * 4 + 2] || cc[i * 4 + 3])
          Log("renderer: T   %s c%u = (%g %g %g %g)", vsk ? "VS" : "PS", i, cc[i * 4], cc[i * 4 + 1], cc[i * 4 + 2], cc[i * 4 + 3]);
    }
    if (fast_consts) {
      if (constants_epoch != cl_epoch || vs_changed) tl.WriteVertexConstants(c, vs_c.data()), ++constant_uploads, ++cb_frame[0];
      if (constants_epoch != cl_epoch || ps_changed) tl.WritePixelConstants(c, ps_c.data()), ++constant_uploads, ++cb_frame[1];
    } else {
      if (constants_epoch != cl_epoch || vs_c != last_vs) tl.WriteVertexConstants(c, vs_c.data()), last_vs = vs_c, ++constant_uploads, ++cb_frame[0];
      if (constants_epoch != cl_epoch || ps_c != last_ps) tl.WritePixelConstants(c, ps_c.data()), last_ps = ps_c, ++constant_uploads, ++cb_frame[1];
    }
    if (constants_epoch != cl_epoch || std::memcmp(&dc, &last_dc, sizeof(dc)) != 0) tl.WriteDrawConstants(c, dc), last_dc = dc, ++constant_uploads, ++cb_frame[2];
    constants_epoch = cl_epoch;
    if (scale_epoch != cl_epoch || std::memcmp(&scale_c, &last_scale_c, sizeof(scale_c)) != 0)
      tl.WriteScaleConstants(c, scale_c), last_scale_c = scale_c, scale_epoch = cl_epoch;

    nvrhi::GraphicsState gs;
    gs.pipeline = pipeline;
    gs.framebuffer = fb;
    gs.viewport.addViewport(vp.viewport);
    gs.viewport.addScissorRect(vp.scissor);
    tl.Bind(gs);
    gs.blendConstantColor = nvrhi::Color(d.f32(F::rb_blend_rgba, 0), d.f32(F::rb_blend_rgba, 1), d.f32(F::rb_blend_rgba, 2),
                                         d.f32(F::rb_blend_rgba, 3));
    gs.dynamicStencilRefValue = uint8_t(ref_mask & 0xFF);

    nvrhi::DrawArguments args;
    bool indexed = false;
    if (r.kind == kDrawIndexed) {
      res::IndexBufferInfo ib;
      const uint8_t* obj = memory.Virtual(d.u32(F::index_buffer));
      if (!obj || !res::DecodeIndexBuffer(obj, ib)) return Skip("index buffer");
      const uint32_t isize = ib.index32 ? 4 : 2;
      if (uint64_t(r.start + r.count) * isize > ib.size_bytes) return Skip("index range");
      const uint8_t* src = phys.At(ib.physical + r.start * isize, r.count * isize);
      if (!src) return Skip("index memory");
      // Index cache: the whole guest index buffer converted once (like the vertex buffers), the draw takes a slice.
      // Without it every draw copies its indices into the ring: a copy in the middle of the render pass, ending the
      // pass, a barrier, beginning again (GPU-bound).
      if (opt.index_cache && expand == Expand::kNone && !Tracing()) {
        if (nvrhi::IBuffer* hb = CachedIndices(ib, c)) {
          gs.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(hb).setFormat(nvrhi::Format::R32_UINT).setOffset(0);
          args.vertexCount = r.count;
          args.startIndexLocation = r.start;
          args.startVertexLocation = uint32_t(r.base_vertex);
          indexed = true;
        }
      }
    }
    if (r.kind == kDrawIndexed && !indexed) {
      res::IndexBufferInfo ib;
      const uint8_t* obj = memory.Virtual(d.u32(F::index_buffer));
      if (!obj || !res::DecodeIndexBuffer(obj, ib)) return Skip("index buffer");
      const uint32_t isize = ib.index32 ? 4 : 2;
      const uint8_t* src = phys.At(ib.physical + r.start * isize, r.count * isize);
      if (!src) return Skip("index memory");
      scratch_indices.clear();
      auto index = [&](uint32_t i) -> uint32_t {
        if (ib.index32) return LoadBE32(src + size_t(i) * 4);
        const uint16_t v = LoadBE16(src + size_t(i) * 2);
        return v == 0xFFFF ? 0xFFFFFFFFu : v;
      };
      if (expand == Expand::kQuads) {
        for (uint32_t i = 0; i + 3 < r.count; i += 4) {
          const uint32_t q[4] = {index(i), index(i + 1), index(i + 2), index(i + 3)};
          scratch_indices.insert(scratch_indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
        }
      } else if (expand == Expand::kFan) {
        for (uint32_t i = 1; i + 1 < r.count; ++i) scratch_indices.insert(scratch_indices.end(), {index(0), index(i), index(i + 1)});
      } else {
        scratch_indices.resize(r.count);
        for (uint32_t i = 0; i < r.count; ++i) scratch_indices[i] = index(i);
      }
      const uint64_t bytes = scratch_indices.size() * 4;
      if (Tracing() && scratch_indices.size() >= 3) {
        uint32_t lo = ~0u, hi = 0, resets = 0;
        for (uint32_t v : scratch_indices) {
          if (v == 0xFFFFFFFFu) {
            ++resets;
            continue;
          }
          lo = std::min(lo, v), hi = std::max(hi, v);
        }
        Log("renderer: T   indices %08X (%u bytes, %s) start %u n %u base %d: %u %u %u min %u max %u reset %u mode %08X",
            ib.physical, ib.size_bytes, ib.index32 ? "32" : "16", r.start, r.count, r.base_vertex, scratch_indices[0],
            scratch_indices[1], scratch_indices[2], lo, hi, resets, mode);
      }
      if (!bytes) return Skip("empty");
      if (index_used + bytes > kIndexRingBytes) return Skip("index ring full");
      if (opt.ring_batch) StageRing(index_stage, index_used, scratch_indices.data(), bytes);
      else c->writeBuffer(index_ring, scratch_indices.data(), bytes, index_used);
      gs.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(index_ring).setFormat(nvrhi::Format::R32_UINT).setOffset(uint32_t(index_used));
      index_used += (bytes + 255) & ~255ull;
      args.vertexCount = uint32_t(scratch_indices.size());
      args.startVertexLocation = uint32_t(r.base_vertex);
      indexed = true;
    } else if (r.kind == kDrawIndexed) {
      // from the index cache (above)
    } else if (expand != Expand::kNone) {
      if (r.count > kPatternVertices) return Skip("too many vertices");
      gs.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(expand == Expand::kQuads ? quad_pattern : fan_pattern).setFormat(nvrhi::Format::R32_UINT);
      args.vertexCount = expand == Expand::kQuads ? r.count / 4 * 6 : (r.count >= 3 ? (r.count - 2) * 3 : 0);
      args.startVertexLocation = r.kind == kDrawVerticesUP ? 0 : r.start;
      indexed = true;
    } else {
      args.vertexCount = r.count;
      args.startVertexLocation = r.kind == kDrawVerticesUP ? 0 : r.start;
    }
    if (!args.vertexCount) return Skip("empty");
    c->setGraphicsState(gs);
    if (indexed) c->drawIndexed(args);
    else c->draw(args);
    if (opt.probe_x >= 0 && Tracing()) {
      char what[128];
      std::snprintf(what, sizeof(what), "draw %llu VS %016llX PS %016llX", (unsigned long long)frame_draws,
                    (unsigned long long)vs->container_hash, (unsigned long long)(ps ? ps->container_hash : 0));
      Probe(colors[0], what);
    }
    ++frame_draws;
    ++total_draws;
    if (r.kind == kDrawVerticesUP) ++frame_up;
    Counters::Get().draws++;
    if (Tracing()) {
      std::string tex;
      for (uint32_t i = 0; i < 32; ++i)
        if (dc.textureIndex[i]) {
          uint32_t words[6];
          d.fetch_constant(i, words);
          const res::TextureFetch tf = res::TextureFetch::FromWords(words);
          const res::TextureRanges rg = res::GetTextureRanges(tf);
          uint32_t nz = 0;
          if (const uint8_t* p = phys.At(rg.base, rg.base_bytes))
            for (uint32_t k = 0; k < rg.base_bytes; k += 64) nz += p[k] != 0;
          char t[240];
          std::snprintf(t, sizeof(t), " t%u=%X(base %08X fmt %u %ux%u swz %03X tiled %u bytes %u nz %u%s w %08X %08X %08X %08X %08X %08X)",
                        i, dc.textureIndex[i], tf.BaseAddress(), unsigned(tf.Format()), tf.Width(), tf.Height(), tf.Swizzle(),
                        unsigned(tf.Tiled()), rg.base_bytes, nz, resolved.count(tf.BaseAddress()) ? " R" : "", words[0], words[1],
                        words[2], words[3], words[4], words[5]);
          tex += t;
        }
      Log("renderer: T f%llu draw kind %u prim %u n=%u VS %016llX PS %016llX%s target %ux%u rt0 %08X ds %08X blend %08X "
          "mask %X depth %08X mode %08X alpha %08X ref %g ndc (%g %g %g %g)%s",
          (unsigned long long)frame, r.kind, r.primitive, r.count, (unsigned long long)vs->container_hash,
          (unsigned long long)(ps ? ps->container_hash : 0), solid ? " solid" : "", size_from->width, size_from->height,
          d.u32(F::render_targets, 0), d.u32(F::depth_stencil), rec.controls[0], rec.masks[0], depth ? depth_control : 0, mode,
          color_control, dc.alphaRef, dc.ndcScale[0], dc.ndcOffset[0], dc.ndcScale[1], dc.ndcOffset[1], tex.c_str());
    }
    if (first_draw_logs < 6) {
      ++first_draw_logs;
      Log("renderer: Draw f%llu kind %u prim %u n=%u VS %016llX PS %016llX target %ux%u vte=%08X vport=(%g %g %g %g) "
          "scissor %d,%d-%d,%d decl=%zu elements",
          (unsigned long long)frame, r.kind, r.primitive, r.count, (unsigned long long)vs->container_hash,
          (unsigned long long)(ps ? ps->container_hash : 0), size_from->width, size_from->height, d.u32(F::pa_cl_vte_cntl),
          d.f32(F::pa_cl_vport, 0), d.f32(F::pa_cl_vport, 1), d.f32(F::pa_cl_vport, 2), d.f32(F::pa_cl_vport, 3),
          vp.scissor.minX, vp.scissor.minY, vp.scissor.maxX, vp.scissor.maxY, elements.size());
    }
    frame_draw_us += uint64_t(MsSince(t_draw) * 1000);
    draw_us_total += uint64_t(MsSince(t_draw) * 1000);
    if (++cl_draws >= kMaxDrawsPerList) Flush();
  }

  // --- Blit (Resolve, Present) --------------------------------------------------------------------------
  nvrhi::IGraphicsPipeline* BlitPipeline(nvrhi::IFramebuffer* fb, bool gamma = false) {
    const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
    const uint64_t key = (info.colorFormats.empty() ? 0 : uint64_t(info.colorFormats[0])) | (gamma ? 1ull << 32 : 0);
    if (auto it = blit_pipelines.find(key); it != blit_pipelines.end()) return it->second;
    nvrhi::GraphicsPipelineDesc pd;
    pd.primType = nvrhi::PrimitiveType::TriangleList;
    pd.VS = blit_vs;
    pd.PS = gamma ? blit_gamma_ps : blit_ps;
    pd.bindingLayouts = tl.layouts();
    pd.renderState.rasterState.setCullNone();
    pd.renderState.rasterState.scissorEnable = false;
    pd.renderState.depthStencilState.depthTestEnable = false;
    pd.renderState.depthStencilState.depthWriteEnable = false;
    nvrhi::GraphicsPipelineHandle p = dev->createGraphicsPipeline(pd, fb->getFramebufferInfo());
    return blit_pipelines.emplace(key, p).first->second;
  }

  // Draws src_slot (rectangle in UV) into fb at viewport.
  void Blit(nvrhi::ICommandList* c, nvrhi::IFramebuffer* fb, uint32_t src_slot, const float uv[4], const nvrhi::Viewport& viewport,
            uint32_t gamma_lut_slot = 0) {
    nvrhi::IGraphicsPipeline* p = BlitPipeline(fb, gamma_lut_slot != 0);
    if (!p) return;
    fcshaders::DrawConstants dc{};
    dc.textureIndex[0] = src_slot;
    dc.textureIndex[1] = gamma_lut_slot;
    dc.pixelSamplers[0] = linear_clamp;
    std::memcpy(dc.clipPlanes[1], uv, 16);
    static const std::array<float, 1024> zero{};
    tl.WriteConstants(c, zero.data(), zero.data(), dc);
    for (uint32_t& n : cb_frame) ++n;
    constants_epoch = 0;  // the draw constants have been overwritten
    scale_epoch = 0;      // b3 likewise (WriteConstants writes factor 1)
    nvrhi::GraphicsState gs;
    gs.pipeline = p;
    gs.framebuffer = fb;
    gs.viewport.addViewportAndScissorRect(viewport);
    tl.Bind(gs);
    c->setGraphicsState(gs);
    c->draw(nvrhi::DrawArguments().setVertexCount(3));
  }

  Resolved* ResolveTexture(uint32_t texture_object, uint32_t* out_w = nullptr, uint32_t* out_h = nullptr) {
    const uint8_t* obj = memory.Virtual(texture_object);
    if (!obj) return nullptr;
    const res::TextureFetch f = res::TextureFetch::FromGuest(obj + layout.texture.get("fetch", 16));
    const uint32_t base = ObjectBase(f);
    if (!base) return nullptr;
    resolve_objects.insert(texture_object);
    const uint32_t w = f.Width(), h = f.Height();
    const nvrhi::Format fmt = ResolveTextureFormat(f.Format());
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    float ssx = 1.0f, ssy = 1.0f;
    ScaleFor(w, h, ssx, ssy);
    const uint32_t hw = HostSize(w, ssx), hh = HostSize(h, ssy);
    auto it = resolved.find(base);
    if (it != resolved.end() && it->second.gw == w && it->second.gh == h && it->second.width == hw && it->second.height == hh &&
        it->second.format == fmt)
      return &it->second;
    Resolved r;
    r.gw = w;
    r.gh = h;
    r.sx = float(hw) / float(w);
    r.sy = float(hh) / float(h);
    nvrhi::TextureDesc d;
    d.width = hw;
    d.height = hh;
    d.format = fmt;
    d.isRenderTarget = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = "resolve target";
    r.tex = dev->createTexture(d);
    if (!r.tex) return nullptr;
    r.width = hw;
    r.height = hh;
    r.format = fmt;
    r.base = base;
    if (it != resolved.end()) {
      retired.emplace_back(frame, it->second.tex);
      r.slot = it->second.slot;
      tl.WriteTextureView(Table::kTex2D, r.slot, r.tex, nvrhi::ComponentMapping());
      r.views = std::move(it->second.views);
      for (const auto& [key, slot] : r.views) tl.WriteTextureView(Table::kTex2D, slot, r.tex, MappingFromConstantKey(key));
      it->second = std::move(r);
      return &it->second;
    }
    r.slot = tl.AddTexture(Table::kTex2D, r.tex);
    if (resolved.size() < 64)
      Log("renderer: resolve target %08X base %08X %ux%u (host %ux%u) format %u fetch %08X %08X %08X %08X %08X %08X", texture_object,
          base, w, h, hw, hh, unsigned(f.Format()), f.words[0], f.words[1], f.words[2], f.words[3], f.words[4], f.words[5]);
    return &resolved.emplace(base, std::move(r)).first->second;
  }

  // Copies [x0,y0)-(x1,y1) from src to dest (point px,py).
  void ResolveRect(Target* src, Resolved* dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t px, int32_t py) {
    if (Tracing())
      Log("renderer: T f%llu resolve src %08X %s %ux%u rect %d,%d-%d,%d -> %ux%u slot %X at %d,%d", (unsigned long long)frame,
          src ? src->surface : 0, src ? (src->depth ? "depth" : "color") : "-", src ? src->width : 0, src ? src->height : 0, x0, y0, x1, y1,
          dst ? dst->width : 0, dst ? dst->height : 0, dst ? dst->slot : 0, px, py);
    if (!src || !dst || !src->srv_slot) {
      if (src && src->host_samples > 1 && src->depth) ++skip_reasons["resolve from MSAA depth"];
      return;
    }
    if (src->depth && dst->format != nvrhi::Format::R32_FLOAT) return;
    // Hor+: the first resolve from the 3D view's target begins the post processing (HorPlusNarrow).
    if (frame_3d && !src->depth && src->surface == view_surface) post_3d = true;
    // Rectangle and destination point come in guest pixels; the source is scaled by its factor, the destination by
    // its own (render scaling; at factor 1 the previous path, with different factors the blit filters).
    x0 = std::clamp(x0, 0, int32_t(src->gw));
    x1 = std::clamp(x1, 0, int32_t(src->gw));
    y0 = std::clamp(y0, 0, int32_t(src->gh));
    y1 = std::clamp(y1, 0, int32_t(src->gh));
    if (x1 <= x0 || y1 <= y0) return;
    const int32_t dx0 = ScaleEdge(px, dst->sx), dy0 = ScaleEdge(py, dst->sy);
    const int32_t dx1 = ScaleEdge(px + x1 - x0, dst->sx), dy1 = ScaleEdge(py + y1 - y0, dst->sy);
    x0 = ScaleEdge(x0, src->sx), x1 = ScaleEdge(x1, src->sx);
    y0 = ScaleEdge(y0, src->sy), y1 = ScaleEdge(y1, src->sy);
    nvrhi::ICommandList* c = CL();
    if (src->single) {
      // MSAA target: first average the samples (like the 360's resolve), then copy as usual.
      c->resolveTexture(src->single, nvrhi::AllSubresources, src->tex, nvrhi::AllSubresources);
      c->setTextureState(src->single, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    }
    nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(dst->tex));
    if (!src->single) c->setTextureState(src->tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    const float sw = float(src->width), sh = float(src->height);
    const float uv[4] = {x0 / sw, y0 / sh, (x1 - x0) / sw, (y1 - y0) / sh};
    Blit(c, fb, src->srv_slot, uv, nvrhi::Viewport(float(dx0), float(dx1), float(dy0), float(dy1), 0, 1));
    c->setTextureState(dst->tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    dst->frame = frame;
    if (!readbacks.empty()) StageReadback(dst->base, *dst, c);
    ++frame_resolves;
    Counters::Get().resolves++;
    if (opt.dump_resolves_frame == frame && !opt.dump_pattern.empty()) {
      Flush();
      const nvrhi::Format f = dst->format;
      const std::string path = DumpPath(opt.dump_pattern, frame) + "_resolve" + std::to_string(frame_resolves) + ".png";
      std::vector<uint8_t> rgba;
      std::string error;
      if (f == nvrhi::Format::RGBA8_UNORM && ReadbackRgba8(dev, dst->tex, rgba, &error) && WriteImage(path, dst->width, dst->height, rgba, &error))
        Log("renderer: resolve dump %s (%ux%u)", path.c_str(), dst->width, dst->height);
      else
        Log("renderer: resolve dump %llu/%llu not possible (format %u) %s", (unsigned long long)frame,
            (unsigned long long)frame_resolves, unsigned(f), error.c_str());
    }
  }

  // Writes the host copy of a resolve target into guest memory the way the 360's GPU would have stored it there
  // (format of the destination texture, endian, tiled), so that a CPU copy by the game sees the image data (loading
  // screen: the game copies the front buffer by CPU into its own texture; eye adaptation: the game reads the 5x3
  // luminance texture by CPU every frame). RGBA8 copies only.
  //
  // The first lock of a target reads back synchronously (waits for the GPU). After that the target is marked as
  // "read by the CPU": small targets (up to 64K pixels) get a copy into a staging texture after every resolve (ring
  // of 3), and the lock takes the newest finished copy without waiting (1-2 frames old). FCP_RENDER_LOCK_READBACK
  // =sync always reads synchronously.
  struct Stage {
    nvrhi::StagingTextureHandle st;
    nvrhi::EventQueryHandle ev;
    uint64_t frame = ~0ull;  // state (resolve frame) of the copy
  };
  struct ReadbackState {
    std::array<Stage, 3> stages;
    uint32_t next = 0;
    uint64_t written_stage_frame = ~0ull;
  };
  std::unordered_map<uint32_t, ReadbackState> readbacks;  // key: base of the resolve target (cpu_read)
  std::vector<std::pair<uint32_t, uint32_t>> stage_unsubmitted;  // (base, ring slot) in the open list
  uint64_t writebacks_sync = 0, writebacks_async = 0, writebacks_skipped = 0;
  double writeback_sync_ms = 0;

  bool AsyncReadback(const Resolved& r) const {
    return opt.lock_readback_async && r.format == nvrhi::Format::RGBA8_UNORM && uint64_t(r.gw) * r.gh <= 65536;
  }

  // Render scaling: the CPU sees the texture at guest size. A scaled resolve target is downscaled for that by a
  // (linear) blit into a guest-sized copy; unscaled it is the source itself.
  nvrhi::ITexture* GuestSized(Resolved& r, nvrhi::ICommandList* c) {
    if (!r.Scaled()) return r.tex;
    if (!r.guest_copy) {
      nvrhi::TextureDesc d = r.tex->getDesc();
      d.width = r.gw;
      d.height = r.gh;
      d.debugName = "resolve target (guest size)";
      r.guest_copy = dev->createTexture(d);
      if (!r.guest_copy) return nullptr;
    }
    nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(r.guest_copy));
    c->setTextureState(r.tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    const float uv[4] = {0, 0, 1, 1};
    Blit(c, fb, r.slot, uv, nvrhi::Viewport(0, float(r.gw), 0, float(r.gh), 0, 1));
    c->setTextureState(r.guest_copy, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    return r.guest_copy;
  }

  // After a resolve into a target read by the CPU: copy into the next staging texture (ResolveRect).
  void StageReadback(uint32_t base, Resolved& r, nvrhi::ICommandList* c) {
    auto it = readbacks.find(base);
    if (it == readbacks.end() || !AsyncReadback(r)) return;
    ReadbackState& rs = it->second;
    const uint32_t i = rs.next++ % rs.stages.size();
    Stage& s = rs.stages[i];
    nvrhi::ITexture* src = GuestSized(r, c);
    if (!src) return;
    if (!s.st || s.st->getDesc().width != r.gw || s.st->getDesc().height != r.gh) {
      nvrhi::TextureDesc d = src->getDesc();
      d.isRenderTarget = false;
      d.initialState = nvrhi::ResourceStates::CopyDest;
      d.keepInitialState = true;
      d.debugName = "lock readback";
      s.st = dev->createStagingTexture(d, nvrhi::CpuAccessMode::Read);
      if (!s.st) return;
    }
    c->copyTexture(s.st, nvrhi::TextureSlice(), src, nvrhi::TextureSlice());
    s.frame = r.frame;
    s.ev = nullptr;
    stage_unsubmitted.emplace_back(base, i);
  }

  // rgba: rows of `pitch` bytes, RGBA8. Writes the image into guest memory as the content of the destination texture.
  bool WriteGuestImage(const Resolved& r, const res::TextureFetch& f, uint32_t base, const uint8_t* rgba, size_t pitch) {
    // Fetch constant with the physical base (the encoding lays out the ranges relative to it).
    res::TextureFetch pf = f;
    pf.words[1] = (pf.words[1] & 0xFFFu) | (base & 0xFFFFF000u);
    const bool swap = res::ResolveDestSwap(pf);
    // rgba has guest size (scaled targets via GuestSized).
    std::vector<uint8_t> blocks(size_t(r.gw) * r.gh * 4);
    const res::ResolveSource src{};
    const uint32_t raw[2] = {0, 0};
    for (uint32_t y = 0; y < r.gh; ++y)
      for (uint32_t x = 0; x < r.gw; ++x) {
        const uint8_t* p = rgba + y * pitch + size_t(x) * 4;
        const float c[4] = {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f};
        if (!res::PackResolveTexel(pf.Format(), swap, c, src, raw, &blocks[(size_t(y) * r.gw + x) * 4])) return false;
      }
    res::GuestTextureImage image;
    if (!res::EncodeGuestTexture(pf, [&](uint32_t level, uint32_t layer) -> const uint8_t* {
          return level == 0 && layer == 0 ? blocks.data() : nullptr;
        }, image))
      return false;
    const res::TextureRanges rg = res::GetTextureRanges(pf);
    uint8_t* dst = const_cast<uint8_t*>(phys.At(rg.base, uint32_t(image.base.size())));
    if (!dst) return false;
    std::memcpy(dst, image.base.data(), image.base.size());
    MarkDirty(rg.base, uint32_t(image.base.size()));
    return true;
  }

  bool WritebackResolved(uint32_t texture_object) {
    const uint8_t* obj = memory.Virtual(texture_object);
    if (!obj) return false;
    const res::TextureFetch f = res::TextureFetch::FromGuest(obj + layout.texture.get("fetch", 16));
    if (f.Type() != 2) return false;
    const uint32_t base = ObjectBase(f);
    auto it = resolved.find(base);
    if (it == resolved.end()) return false;
    Resolved& r = it->second;
    if (r.format != nvrhi::Format::RGBA8_UNORM) {
      Once("wbfmt" + std::to_string(unsigned(r.format)), "renderer: write-back of a resolve target: format %s not supported", "");
      return false;
    }
    if (r.written_back_frame == r.frame) return true;  // already written back since the last resolve
    auto [rb, first] = readbacks.try_emplace(base);
    if (first && AsyncReadback(r)) {
      // First lock of a small target: kick off a copy, do not wait (guest memory holds the old content for 1-2
      // frames; without this the CPU always read zeros there).
      StageReadback(base, r, CL());
      ++writebacks_skipped;
      return true;
    }
    if (!first && AsyncReadback(r)) {
      // Newest finished staging copy, without waiting.
      Stage* best = nullptr;
      for (Stage& s : rb->second.stages)
        if (s.st && s.ev && s.frame != ~0ull && dev->pollEventQuery(s.ev) && (!best || s.frame > best->frame)) best = &s;
      if (!best || best->frame == rb->second.written_stage_frame) {
        ++writebacks_skipped;  // nothing newer finished: guest memory keeps the last copy
        return true;
      }
      size_t pitch = 0;
      const auto* p = static_cast<const uint8_t*>(dev->mapStagingTexture(best->st, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
      if (!p) return false;
      const bool ok = WriteGuestImage(r, f, base, p, pitch);
      dev->unmapStagingTexture(best->st);
      rb->second.written_stage_frame = best->frame;
      if (best->frame == r.frame) r.written_back_frame = r.frame;
      ++writebacks_async;
      return ok;
    }
    const auto t0 = Clock::now();
    nvrhi::ITexture* src = r.Scaled() ? GuestSized(r, CL()) : r.tex.Get();
    Flush();
    std::vector<uint8_t> rgba;
    std::string error;
    if (!src || !ReadbackRgba8(dev, src, rgba, &error)) {
      Log("renderer: write-back %08X: %s", texture_object, error.c_str());
      return false;
    }
    if (!WriteGuestImage(r, f, base, rgba.data(), size_t(r.gw) * 4)) return false;
    r.written_back_frame = r.frame;
    ++writebacks_sync;
    writeback_sync_ms += MsSince(t0);
    if (writebacks_sync <= 16)
      Log("renderer: resolve target %08X (base %08X, %ux%u) written to guest memory for CPU access (synchronous, %.1f ms)%s",
          texture_object, base, r.gw, r.gh, MsSince(t0), AsyncReadback(r) ? "; further locks read asynchronously" : "");
    return true;
  }

  void ClearTargets(const GuestDeviceView& d, uint32_t flags, const float color[4], float z, uint32_t stencil) {
    nvrhi::ICommandList* c = CL();
    if (Tracing())
      Log("renderer: T f%llu clear flags %X rt0 %08X ds %08X color (%g %g %g %g) z %g stencil %u%s", (unsigned long long)frame,
          flags, d.u32(F::render_targets, 0), d.u32(F::depth_stencil), color[0], color[1], color[2], color[3], z, stencil,
          in_tiling ? " (Tiling)" : "");
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(flags & (1u << i))) continue;
      if (Target* t = GetTarget(d.u32(F::render_targets, i), false))
        c->clearTextureFloat(t->tex, nvrhi::AllSubresources, nvrhi::Color(color[0], color[1], color[2], color[3]));
    }
    if (flags & 0x30) {
      if (Target* t = GetTarget(d.u32(F::depth_stencil), true))
        c->clearDepthStencilTexture(t->tex, nvrhi::AllSubresources, (flags & 0x10) != 0, z, (flags & 0x20) != 0, uint8_t(stencil));
    }
    ++frame_clears;
    Counters::Get().clears++;
    if (opt.probe_x >= 0 && Tracing()) Probe(GetTarget(d.u32(F::render_targets, 0), false), "clear");
  }

  // --- Frame -----------------------------------------------------------------------------------------------
  void Swap(const SwapRecord& r) {
    uint32_t w = 0, h = 0;
    Resolved* f = nullptr;
    if (r.front_buffer) {
      const uint8_t* obj = memory.Virtual(r.front_buffer);
      if (obj) {
        const res::TextureFetch fetch = res::TextureFetch::FromGuest(obj + layout.texture.get("fetch", 16));
        if (auto it = resolved.find(ObjectBase(fetch)); it != resolved.end()) f = &it->second, w = f->width, h = f->height;
      }
    }
    if (!f) Once("swapfront", "renderer: swap without a known front buffer (%s)", r.front_buffer ? "no resolve into it" : "r4 = 0");
    ReadGammaRamp(r.device);
    if (f) {
      front = f->tex;
      front_slot = f->slot;
      front_w = w;
      front_h = h;
      front_gw = f->gw;
      front_gh = f->gh;
    }
    if (opt.probe_x >= 0 && Tracing()) ProbeReport();
    if (k_min_dist < 1e30f) {
      Log("renderer: K f%llu nearest instance %.1f m: T (%.1f %.1f %.1f) camera (%.1f %.1f %.1f)", (unsigned long long)frame,
          k_min_dist, k_min_t[0], k_min_t[1], k_min_t[2], k_min_t[3], k_min_t[4], k_min_t[5]);
      k_min_dist = 1e30f;
    }
    Flush();
    DiagRewatch();
    // Diagnostics FCP_DIAG_RESCALE=<frame>,<factor>: factor change as after a resize (scale = auto).
    if (diag_rescale_frame && frame == diag_rescale_frame) wanted_scale = diag_rescale_to;
    // Hor+: show this picture in the wide shape when it had a 3D view with post processing.
    front_wide = cur_wide != 1.0f && frame_3d && post_3d;
    if (front_wide) ++wide_frames;
    frame_3d = post_3d = false;
    view_surface = 0;
    if (wanted_scale != cur_scale || wanted_wide != cur_wide) ApplyScale(wanted_scale, wanted_wide);
    if (opt.slow_frame_ms > 0) {
      // Slow frames (FCP_TRACE_HITCHES): the renderer's share of the frame since the last swap.
      Counters& cn = Counters::Get();
      FrameMark now;
      now.draw_us = draw_us_total, now.conv_us = tex_convert_us, now.buf_us = buffer_us, now.pipe_us = cn.pipeline_create_us;
      now.pipes = cn.pipelines_created, now.convs = textures_converted + textures_rewritten, now.wait_us = cn.wait_us;
      const double wall = std::chrono::duration<double, std::milli>(now.t - mark.t).count();
      if (wall >= opt.slow_frame_ms && slow_frames_logged < 400) {
        ++slow_frames_logged;
        Log("renderer: slow frame %llu: %.1f ms, draws %.1f ms (of which pipelines %llu / %.1f ms, textures %llu / %.1f ms, "
            "buffers %.1f ms / %llu KB), GPU wait %.1f ms, %llu draws",
            (unsigned long long)frame, wall, (now.draw_us - mark.draw_us) / 1000.0, (unsigned long long)(now.pipes - mark.pipes),
            (now.pipe_us - mark.pipe_us) / 1000.0, (unsigned long long)(now.convs - mark.convs), (now.conv_us - mark.conv_us) / 1000.0,
            (now.buf_us - mark.buf_us) / 1000.0, (unsigned long long)(frame_buffer_bytes >> 10), (now.wait_us - mark.wait_us) / 1000.0,
            (unsigned long long)frame_draws);
      }
      mark = now;
      frame_buffer_bytes = 0;
    }
    for (size_t i = 0; i < cb_frame.size(); ++i) cb_max[i] = std::max(cb_max[i], cb_frame[i]), cb_frame[i] = 0;
    ++total_swaps;
    Counters::Get().swaps++;
    ++frame;
    // retire after three frames
    retired.erase(std::remove_if(retired.begin(), retired.end(), [&](const auto& e) { return e.first + 3 < frame; }), retired.end());
    MaybeDump();
    if (opt.stats_every && frame % opt.stats_every == 0) {
      const double s = std::chrono::duration<double>(Clock::now() - stats_since).count();
      const auto st = shaders.stats();
      std::string reasons;
      for (const auto& [k, v] : skip_reasons) reasons += " " + k + "=" + std::to_string(v);
      Log("renderer: frame %llu: %.1f fps, last frame %llu draws (UP %llu, skipped %llu), %llu clears, %llu resolves; "
          "draw time %.2f ms/frame, texture check %.2f ms/frame, conversion %.1f ms total (%llu new, %llu rewritten), "
          "buffers written %llu; shaders pack %llu cache %llu translated %llu errors %llu; pipelines %zu (new in interval %llu); targets %zu, "
          "resolve targets %zu, textures %zu; skipped total:%s",
          (unsigned long long)frame, opt.stats_every / std::max(s, 1e-3), (unsigned long long)frame_draws,
          (unsigned long long)frame_up, (unsigned long long)frame_skipped, (unsigned long long)frame_clears,
          (unsigned long long)frame_resolves, frame_draw_us / 1000.0 / opt.stats_every, tex_check_us / 1000.0 / opt.stats_every,
          tex_convert_us / 1000.0, (unsigned long long)textures_converted, (unsigned long long)textures_rewritten,
          (unsigned long long)buffers_written, (unsigned long long)st.pack_hits, (unsigned long long)st.cache_hits,
          (unsigned long long)st.translated, (unsigned long long)st.failed, pipelines.size(),
          (unsigned long long)frame_new_pipelines, targets.size(), resolved.size(),
          textures.size(), reasons.c_str());
      if (Watching())
        Log("renderer: write watch: %llu checks without writes skipped, %llu hashed, %llu textures changing constantly (hash per frame)",
            (unsigned long long)watch_skips, (unsigned long long)watch_checks, (unsigned long long)watch_volatile);
      Log("renderer: constant versions per frame at most VS %u PS %u Draw %u (ring %u); raw data unchanged %llu, converted %llu",
          cb_max[0], cb_max[1], cb_max[2], tl.constant_versions(), (unsigned long long)fast_const_skips,
          (unsigned long long)fast_const_converts);
      fast_const_skips = fast_const_converts = 0;
      if (cur_wide != 1.0f) {
        Log("renderer: Hor+ %.4f: %llu wide frames of %u, 2D centred %llu, full width %llu, box unknown %llu",
            cur_wide, (unsigned long long)wide_frames, opt.stats_every, (unsigned long long)narrow_draws, (unsigned long long)full_draws,
            (unsigned long long)unknown_boxes);
        wide_frames = narrow_draws = full_draws = unknown_boxes = 0;
      }
      if (opt.index_cache)
        Log("renderer: index cache: %llu hits, %llu converted (%llu KB), %zu buffers, %llu changing constantly (ring)",
            (unsigned long long)idx_hits, (unsigned long long)idx_converts, (unsigned long long)(idx_bytes >> 10),
            index_buffers.size(), (unsigned long long)idx_volatile);
      idx_hits = idx_converts = idx_bytes = 0;
      if (opt.ring_batch)
        Log("renderer: ring copies batched: %llu lists, %llu KB", (unsigned long long)ring_uploads,
            (unsigned long long)(ring_upload_bytes >> 10));
      ring_uploads = ring_upload_bytes = 0;
      cb_max = {};
      {
        // Lock: wait time per frame by call kind, hold time of the present thread.
        const double per = 1000.0 * opt.stats_every;
        std::string waits;
        for (uint32_t i = 0; i < kSiteCount; ++i) {
          if (!lock_stats[i].contended) continue;
          char b[80];
          std::snprintf(b, sizeof(b), " %s %.3f ms (%llu)", kSiteNames[i], lock_stats[i].wait_us / per,
                        (unsigned long long)lock_stats[i].contended);
          waits += b;
        }
        Log("renderer: lock per frame: waits%s; present holds %.3f ms, GPU frame-ahead wait without lock %.3f ms, acquire %.3f ms "
            "(%llu presents); swap back pressure %.3f ms (%llu); present %s",
            waits.empty() ? " -" : waits.c_str(), present_hold_us / per, present_frame_wait_us / per, present_acquire_us / per,
            (unsigned long long)presents_in_window, swap_wait_us / per, (unsigned long long)swap_waits,
            opt.present_unlocked ? "split" : "under the lock (FCP_RENDER_PRESENT_UNLOCKED=0)");
        lock_stats = {};
        present_hold_us = present_frame_wait_us = present_acquire_us = presents_in_window = 0;
        swap_wait_us = swap_waits = 0;
      }
      stats_since = Clock::now();
      frame_draw_us = tex_check_us = 0;
      frame_new_pipelines = 0;
    }
    frame_calls = frame_draws = frame_skipped = frame_up = frame_clears = frame_resolves = 0;
  }

  void MaybeDump() {
    if (opt.dump_pattern.empty() || !front) return;
    const bool listed = std::find(dump_list.begin(), dump_list.end(), frame) != dump_list.end();
    const bool every = opt.dump_every && frame % opt.dump_every == 0;
    if (!listed && !every) return;
    std::vector<uint8_t> rgba;
    std::string error;
    std::string path = DumpPath(opt.dump_pattern, frame);
    if (opt.dump_present) {
      // FCP_DUMP_PRESENT=1: additionally the shown frame (swapchain at the next present: SMAA, bars, overlay);
      // the front buffer of the same frame then as <name>_front.<ext> (comparison without SMAA in the same run).
      present_dump = path;
      const size_t dot = path.find_last_of('.');
      path = dot == std::string::npos ? path + "_front" : path.substr(0, dot) + "_front" + path.substr(dot);
    }
    if (front->getDesc().format != nvrhi::Format::RGBA8_UNORM && front->getDesc().format != nvrhi::Format::BGRA8_UNORM) {
      Log("renderer: dump frame %llu: front buffer format not RGBA8", (unsigned long long)frame);
      return;
    }
    const bool ok = ReadbackRgba8(dev, front, rgba, &error);
    if (ok && opt.gamma_ramp && !gamma_identity) {
      // The dumps show what the present shows: gamma ramp as in the blit (linear between the 256 sample points).
      for (size_t i = 0; i < rgba.size(); ++i) {
        if ((i & 3) == 3) continue;
        rgba[i] = uint8_t(std::min(255u, (uint32_t(gamma_rgba[size_t(rgba[i]) * 4 + (i & 3)]) * 255u + 32767u) / 65535u));
      }
    }
    if (!ok || !WriteImage(path, front_w, front_h, rgba, &error))
      Log("renderer: dump frame %llu failed: %s", (unsigned long long)frame, error.c_str());
    else
      Log("renderer: dump frame %llu -> %s (%ux%u)", (unsigned long long)frame, path.c_str(), front_w, front_h);
  }

  std::string ShutdownSummary() const {
    const auto st = shaders.stats();
    std::string reasons;
    for (const auto& [k, v] : skip_reasons) reasons += " " + k + "=" + std::to_string(v);
    char line[1400];
    if (occ)
      std::snprintf(line, sizeof(line), "host queries: begin %llu, ready %llu (of which 0 samples %llu), not yet ready %llu, original %llu; ",
                    (unsigned long long)q_begins, (unsigned long long)q_ready, (unsigned long long)q_zero,
                    (unsigned long long)q_not_ready, (unsigned long long)q_fallback);
    else
      line[0] = 0;
    std::string qs = line;
    std::snprintf(line, sizeof(line), "write-back for CPU locks: synchronous %llu (%.0f ms), asynchronous %llu, nothing new %llu; ",
                  (unsigned long long)writebacks_sync, writeback_sync_ms, (unsigned long long)writebacks_async,
                  (unsigned long long)writebacks_skipped);
    qs += line;
    std::snprintf(line, sizeof(line),
                  "%sframes %llu, shown %llu, draws %llu, skipped %llu (%s); shaders created %llu: pack %llu, cache %llu, "
                  "translated %llu (%.0f ms), errors %llu; pipelines %zu; %s", qs.c_str(),
                  (unsigned long long)total_swaps, (unsigned long long)total_presents, (unsigned long long)total_draws,
                  (unsigned long long)total_skipped, reasons.c_str(), (unsigned long long)st.created,
                  (unsigned long long)st.pack_hits, (unsigned long long)st.cache_hits, (unsigned long long)st.translated,
                  st.translate_ms, (unsigned long long)st.failed, pipelines.size(), Counters::Get().Summary().c_str());
    return line;
  }

  void HandleResize() {
    if (!resize_pending) return;
    resize_pending = false;
    back_fbs.clear();
    if (host->Resize(resize_w, resize_h)) {
      Log("renderer: Swapchain %ux%u", host->swapchain_width(), host->swapchain_height());
      if (opt.scale_auto) wanted_scale = AutoScale(host->swapchain_height());  // applied at the next swap
      wanted_wide = WideFor(host->swapchain_width(), host->swapchain_height());   // Hor+ likewise
    }
  }

  // SMAA: pass and intermediate images at frame size (recreated on a size change). false: this frame without SMAA.
  bool SmaaReady(uint32_t w, uint32_t h) {
    if (smaa_failed || !w || !h) return false;
    if (!smaa) {
      smaa = std::make_unique<SmaaPass>();
      std::string error;
      if (!smaa->Init(dev, opt.api, &error)) {
        Log("renderer: SMAA not available: %s", error.c_str());
        smaa_failed = true;
        smaa.reset();
        return false;
      }
      Log("renderer: SMAA 1x ready (threshold 0.05, 16/8 search steps)");
    }
    if (smaa_out && smaa_w == w && smaa_h == h) return true;
    nvrhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = nvrhi::Format::RGBA8_UNORM;
    d.isRenderTarget = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.debugName = "smaa in";
    nvrhi::TextureHandle in = dev->createTexture(d);
    d.debugName = "smaa out";
    nvrhi::TextureHandle out = dev->createTexture(d);
    if (!in || !out) {
      Log("renderer: SMAA: intermediate images %ux%u failed", w, h);
      smaa_failed = true;
      return false;
    }
    if (smaa_out_slot) {
      // The table slot takes effect immediately: first wait until no present reads the old image any more.
      dev->waitForIdle();
      tl.WriteTextureView(Table::kTex2D, smaa_out_slot, out, nvrhi::ComponentMapping());
    } else {
      smaa_out_slot = tl.AddTexture(Table::kTex2D, out);
    }
    smaa_in = in;
    smaa_out = out;
    smaa_in_fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(smaa_in));
    Log("renderer: SMAA intermediate images %ux%u (previously %ux%u)", w, h, smaa_w, smaa_h);
    smaa_w = w;
    smaa_h = h;
    return smaa_in_fb && smaa_out_slot;
  }

  bool BeginPresent() {
    if (!ready || !host || !host->has_swapchain()) return false;
    HandleResize();
    back = host->BeginFrame();
    if (!back) return false;
    auto it = back_fbs.find(back);
    if (it == back_fbs.end()) it = back_fbs.emplace(back, dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(back))).first;
    nvrhi::IFramebuffer* fb = it->second;
    present_cl->open();
    present_cl->clearTextureFloat(back, nvrhi::AllSubresources, nvrhi::Color(0, 0, 0, 1));
    if (front) {
      // Aspect ratio of the frame, centred (bars instead of distortion).
      const float bw = float(back->getDesc().width), bh = float(back->getDesc().height);
      // Hor+: a picture with a 3D view in the host texture's shape (wide), all others in the guest shape (16:9).
      const float aspect = front_wide && front_h ? float(front_w) / float(front_h)
                           : front_gh            ? float(front_gw) / float(front_gh)
                           : front_h             ? float(front_w) / float(front_h)
                                                 : 16.0f / 9.0f;
      float w = bw, h = bw / aspect;
      if (h > bh) h = bh, w = bh * aspect;
      const float x = (bw - w) * 0.5f, y = (bh - h) * 0.5f;
      present_cl->setTextureState(front, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      const float uv[4] = {0, 0, 1, 1};
      if (gamma_dirty && gamma_lut) {
        present_cl->writeTexture(gamma_lut, 0, 0, gamma_rgba.data(), 256 * 8);
        present_cl->setTextureState(gamma_lut, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        gamma_dirty = false;
      }
      const bool gamma = opt.gamma_ramp && blit_gamma_ps && gamma_lut_slot && !gamma_identity;
      if (opt.smaa && SmaaReady(front_w, front_h)) {
        // SMAA on the frame as it is shown (after the gamma ramp, at frame size), then centred without the ramp.
        nvrhi::ITexture* src = front;
        if (gamma) {
          Blit(present_cl, smaa_in_fb, front_slot, uv, nvrhi::Viewport(0, float(front_w), 0, float(front_h), 0, 1), gamma_lut_slot);
          present_cl->setTextureState(smaa_in, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
          src = smaa_in;
        }
        smaa->Run(present_cl, src, smaa_out);
        present_cl->setTextureState(smaa_out, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        Blit(present_cl, fb, smaa_out_slot, uv, nvrhi::Viewport(x, x + w, y, y + h, 0, 1), 0);
        ++smaa_frames;
      } else {
        Blit(present_cl, fb, front_slot, uv, nvrhi::Viewport(x, x + w, y, y + h, 0, 1), gamma ? gamma_lut_slot : 0);
      }
    }
    present_cl->close();
    dev->executeCommandList(present_cl);
    return true;
  }
};

// ----------------------------------------------------------------------------------------------------------

std::unique_ptr<GameRenderer> GameRenderer::Create(const GameRendererOptions& options, std::string* error) {
  std::unique_ptr<GameRenderer> r(new GameRenderer());
  r->impl_ = std::make_unique<Impl>();
  Impl& m = *r->impl_;
  m.opt = options;
  if (!m.layout.Load(options.layout_path, error)) return nullptr;
  m.lo.Load(m.layout);
  ShaderLibrary::Options so;
  so.target = options.api == HostApi::kVulkan ? fcshaders::PackTarget::Spirv : fcshaders::PackTarget::Dxil;
  so.pack_path = options.shader_pack;
  so.cache_dir = options.shader_cache;
  so.dxc_path = options.dxc_path;
  so.accept_stale_pack = Switches::Get().shader_pack_stale_ok;
  std::string note;
  if (!m.shaders.Open(so, error, &note)) return nullptr;
  m.shaders_open = true;
  if (const char* e = std::getenv("FCP_DIAG_RESCALE")) {
    unsigned long long f = 0;
    float s = 1.0f;
    if (std::sscanf(e, "%llu,%f", &f, &s) == 2 && s >= 0.5f && s <= 4.0f) m.diag_rescale_frame = f, m.diag_rescale_to = s;
  }
  if (const char* e = std::getenv("FCP_DIAG_WATCH")) {
    unsigned a = 0, n = 0;
    if (std::sscanf(e, "%x,%x", &a, &n) == 2) m.diag_watch_a = a, m.diag_watch_n = n;
  }
  m.Log("renderer: ShaderLibrary %s, Pack %s (%s), Cache %s", fcshaders::packTargetName(so.target),
        options.shader_pack.empty() ? "-" : options.shader_pack.c_str(), note.empty() ? "ok" : note.c_str(),
        options.shader_cache.empty() ? "-" : options.shader_cache.c_str());
  for (size_t pos = 0; pos < options.dump_frames.size();) {
    const size_t comma = options.dump_frames.find(',', pos);
    const std::string part = options.dump_frames.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!part.empty()) m.dump_list.push_back(std::strtoull(part.c_str(), nullptr, 10));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return r;
}

GameRenderer::~GameRenderer() { Shutdown(); }

void GameRenderer::SetGuestBase(const uint8_t* base) {
  std::lock_guard lock(impl_->mutex);
  impl_->memory.base = base;
  impl_->phys.base = base ? base + 0xA0000000u : nullptr;
}

void GameRenderer::SetWriteWatch(void (*watch)(void*, uint32_t, uint32_t), void* context) {
  std::lock_guard lock(impl_->mutex);
  impl_->watch_ctx = context;
  impl_->watch_fn = watch;
  impl_->Log("renderer: SDK write watch %s", watch ? (impl_->opt.texture_watch ? "active for guest textures" : "present, off (FCP_RENDER_TEXTURE_WATCH=0)") : "off");
}

void GameRenderer::OnGuestWrite(uint32_t physical, uint32_t bytes) {
  Impl& m = *impl_;
  m.MarkDirty(physical, bytes);
  // Diagnostics FCP_DIAG_WATCH=<physical>,<bytes>: every write into the range goes to the log (thread, frame), then
  // the range is watched again (at most 2000 messages).
  if (m.diag_watch_n && physical < m.diag_watch_a + m.diag_watch_n && physical + bytes > m.diag_watch_a && m.diag_logged < 2000) {
    ++m.diag_logged;
    m.diag_rewatch = true;  // re-watch at the next swap (not here: the SDK holds its lock)
    m.Log("renderer: W write %08X+%X (thread %zu, frame %llu)", physical, bytes,
          std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF, (unsigned long long)m.frame);
  }
}

bool GameRenderer::AttachWindow(void* hwnd, uint32_t width, uint32_t height, std::string* error) {
  std::lock_guard lock(impl_->mutex);
  Impl& m = *impl_;
  if (m.host) return true;
  m.hwnd = hwnd;
  m.win_w = width;
  m.win_h = height;
  if (!m.CreateHost(hwnd, width, height, error)) {
    m.failed = true;
    return false;
  }
  return true;
}

void GameRenderer::Resize(uint32_t width, uint32_t height) {
  std::lock_guard lock(impl_->mutex);
  impl_->resize_pending = true;
  impl_->resize_w = width;
  impl_->resize_h = height;
}

namespace {
uint64_t UsBetween(Clock::time_point a, Clock::time_point b) {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
}
}  // namespace

bool GameRenderer::BeginPresent() {
  Impl& m = *impl_;
  if (!m.opt.present_unlocked) {
    Impl::TimedLock lock(m, kSitePresent);
    const auto t0 = Clock::now();
    const bool ok = m.BeginPresent();
    m.present_hold_us += UsBetween(t0, Clock::now());
    ++m.presents_in_window;
    return ok;
  }
  // Split: resize under the lock, then wait for the GPU (frames ahead) and acquire the swapchain image without the
  // lock, then draw and submit under the lock. The game's render thread keeps running meanwhile (otherwise it would
  // wait for this lock in OnQuery/OnLock/OnDraw).
  HostDevice* host = nullptr;
  {
    Impl::TimedLock lock(m, kSitePresent);
    if (!m.ready || !m.host || !m.host->has_swapchain()) return false;
    m.HandleResize();
    host = m.host.get();
  }
  const auto t0 = Clock::now();
  host->WaitFrameSlot();
  const auto t1 = Clock::now();
  const bool acquired = host->AcquireImage();
  const auto t2 = Clock::now();
  Impl::TimedLock lock(m, kSitePresent);
  m.present_frame_wait_us += UsBetween(t0, t1);
  m.present_acquire_us += UsBetween(t1, t2);
  ++m.presents_in_window;
  if (!acquired) return false;
  const bool ok = m.BeginPresent();
  m.present_hold_us += UsBetween(t2, Clock::now());
  return ok;
}

void GameRenderer::EndPresent() {
  Impl& m = *impl_;
  Impl::TimedLock lock(m, kSitePresent);
  if (!m.host || !m.back) return;
  if (!m.present_dump.empty()) {
    // Test (RequestPresentDump): what the player sees, with overlay; waits for the GPU.
    std::vector<uint8_t> rgba;
    std::string error;
    const nvrhi::TextureDesc& d = m.back->getDesc();
    if (ReadbackRgba8(m.dev, m.back, rgba, &error) && WriteImage(m.present_dump, d.width, d.height, rgba, &error))
      m.Log("renderer: present dump -> %s (%ux%u, frame %llu)", m.present_dump.c_str(), d.width, d.height, (unsigned long long)m.frame);
    else
      m.Log("renderer: present dump %s failed: %s", m.present_dump.c_str(), error.c_str());
    m.present_dump.clear();
  }
  const auto t0 = Clock::now();
  m.host->Present();
  m.back = nullptr;
  ++m.total_presents;
  m.present_hold_us += UsBetween(t0, Clock::now());
}

uint32_t GameRenderer::RenderOverlay(const OverlayFrame& frame, OverlayTextureOp* ops, size_t op_count) {
  Impl& m = *impl_;
  Impl::TimedLock lock(m, kSitePresent);
  if (!m.ready || !m.host || !m.back || m.overlay_failed) return 0;
  if (!op_count && !frame.command_count) return 0;  // nothing to do: no GPU work
  if (!m.overlay) {
    std::string error;
    auto pass = std::make_unique<OverlayPass>();
    if (!pass->Init(m.dev, m.opt.api, &error)) {
      m.overlay_failed = true;
      m.Log("renderer: overlay not available: %s", error.c_str());
      return 0;
    }
    m.overlay = std::move(pass);
    m.overlay_cl = m.dev->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
    m.Log("renderer: overlay ready (%s)", HostApiName(m.opt.api));
  }
  auto it = m.back_fbs.find(m.back);
  if (it == m.back_fbs.end())
    it = m.back_fbs.emplace(m.back, m.dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m.back))).first;
  nvrhi::ICommandList* cl = m.overlay_cl;
  cl->open();
  for (size_t i = 0; i < op_count; ++i) {
    OverlayTextureOp& op = ops[i];
    switch (op.kind) {
      case OverlayTextureOp::Kind::kCreate: op.id = m.overlay->CreateTexture(cl, op.width, op.height, op.rgba); break;
      case OverlayTextureOp::Kind::kUpdate: m.overlay->UpdateTexture(cl, op.id, op.rgba); break;
      case OverlayTextureOp::Kind::kDestroy: m.overlay->DestroyTexture(op.id); break;
    }
    op.done = op.kind != OverlayTextureOp::Kind::kCreate || op.id != 0;
  }
  const uint32_t draws = m.overlay->Draw(cl, it->second, frame);
  cl->close();
  m.dev->executeCommandList(cl);
  if (draws) {
    ++m.overlay_frames;
    m.overlay_draws += draws;
  }
  return draws;
}

void GameRenderer::RequestPresentDump(const std::string& path) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  impl_->present_dump = path;
}

void GameRenderer::SetGammaRamp(bool on) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.gamma_ramp == on) return;
  impl_->opt.gamma_ramp = on;
  impl_->Log("renderer: gamma ramp %s (settings menu)", on ? "on" : "off");
}

void GameRenderer::SetSmaa(bool on) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.smaa == on) return;
  impl_->opt.smaa = on;
  impl_->Log("renderer: SMAA %s (settings menu, from the next present)", on ? "on" : "off");
}

void GameRenderer::SetTextureReplaceSource(TextureReplaceSource* source) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  impl_->replace_source = source;
  impl_->Log("renderer: texture replacement %s", source ? "registered (button glyphs)" : "unregistered");
}

void GameRenderer::SetScale(float scale, bool scale_auto) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  Impl& m = *impl_;
  m.opt.scale = std::clamp(scale, 0.5f, 4.0f);
  m.opt.scale_auto = scale_auto;
  m.wanted_scale = scale_auto && m.host && m.host->has_swapchain() ? Impl::AutoScale(m.host->swapchain_height()) : m.opt.scale;
  m.Log("renderer: render scale requested %s%.3f (settings menu, from the next swap)", scale_auto ? "auto = " : "",
        m.wanted_scale);
}

float GameRenderer::Wide() const { return impl_ ? impl_->wide_published.load() : 1.0f; }

void GameRenderer::Shutdown() {
  if (!impl_) return;
  impl_->prewarm_stop = true;
  if (impl_->prewarm.joinable()) impl_->prewarm.join();
  std::lock_guard lock(impl_->mutex);
  Impl& m = *impl_;
  if (!m.host || m.shut_down) return;
  m.shut_down = true;
  m.ready = false;
  m.failed = true;  // no new devices after shutdown
  if (m.cl_open) m.Flush();
  m.host->WaitIdle();
  m.host->SavePipelineCache();
  m.Log("renderer: end. %s", m.ShutdownSummary().c_str());
  if (m.overlay) m.Log("renderer: overlay: %llu frames with overlay, %llu draws, %zu textures", (unsigned long long)m.overlay_frames,
                       (unsigned long long)m.overlay_draws, m.overlay->texture_count());
  if (m.replace_source)
    m.Log("renderer: texture replacement: %llu textures checked, %llu bindings replaced, %llu images uploaded",
          (unsigned long long)m.replace_classified, (unsigned long long)m.replace_binds, (unsigned long long)m.replace_uploads);
}

std::string GameRenderer::Summary() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->ShutdownSummary();
}

void GameRenderer::OnDeviceCreated(uint32_t device) {
  std::lock_guard lock(impl_->mutex);
  impl_->Log("renderer: game device %08X", device);
  impl_->EnsureReady();
}

void GameRenderer::OnDraw(const DrawRecord& r) {
  Impl::TimedLock lock(*impl_, kSiteDraw);
  if (!impl_->EnsureReady()) return;
  impl_->Draw(r);
}

void GameRenderer::OnClear(const ClearRecord& r) {
  Impl::TimedLock lock(*impl_, kSiteClear);
  Impl& m = *impl_;
  if (!m.EnsureReady()) return;
  const GuestDeviceView d = m.View(r.device);
  if (!d.valid()) return;
  if (r.has_rect && !m.in_tiling) {
    // Partial areas: only full surfaces are cleared (partial rectangles are counted).
    Target* t = m.GetTarget(d.u32(F::render_targets, 0), false);
    if (t && (r.rect[0] > 0 || r.rect[1] > 0 || uint32_t(r.rect[2]) < t->surface_w || uint32_t(r.rect[3]) < t->surface_h)) {
      ++m.skip_reasons["partial clear"];
      return;
    }
  }
  m.ClearTargets(d, r.flags, r.color, r.z, r.stencil);
}

void GameRenderer::OnResolve(const ResolveRecord& r) {
  Impl::TimedLock lock(*impl_, kSiteResolve);
  Impl& m = *impl_;
  if (!m.EnsureReady()) return;
  const GuestDeviceView d = m.View(r.device);
  if (!d.valid()) return;
  const res::ResolveFlags flags{r.flags};
  if (flags.DepthStencil()) {
    // Depth into an R32F copy (shadow map, flags 0x14 measured). FCP_RENDER_DEPTH_RESOLVE=0: skipped as before.
    Target* src = m.opt.depth_resolve ? m.GetTarget(d.u32(F::depth_stencil), true) : nullptr;
    Resolved* dst = src && r.dest_texture ? m.ResolveTexture(r.dest_texture) : nullptr;
    if (!dst || dst->format != nvrhi::Format::R32_FLOAT) {
      ++m.skip_reasons["resolve depth"];
    } else {
      int32_t x0 = 0, y0 = 0, x1 = int32_t(src->surface_w), y1 = int32_t(src->surface_h);
      if (r.has_rect) x0 = r.rect[0], y0 = r.rect[1], x1 = r.rect[2], y1 = r.rect[3];
      const int32_t px = r.has_point ? r.point[0] : x0, py = r.has_point ? r.point[1] : y0;
      m.ResolveRect(src, dst, x0, y0, x1, y1, px, py);
    }
  } else if (r.dest_texture) {
    Target* src = m.GetTarget(d.u32(F::render_targets, flags.RenderTargetIndex()), false);
    if (Resolved* dst = m.ResolveTexture(r.dest_texture)) {
      int32_t x0 = 0, y0 = 0, x1 = src ? int32_t(src->surface_w) : 0, y1 = src ? int32_t(src->surface_h) : 0;
      if (r.has_rect) x0 = r.rect[0], y0 = r.rect[1], x1 = r.rect[2], y1 = r.rect[3];
      int32_t px = r.has_point ? r.point[0] : x0, py = r.has_point ? r.point[1] : y0;
      // While tiling, the engine resolves each tile with destination point (0,0) (measured: (0,0)-(768,720) and
      // (640,0)-(1280,720), both point (0,0)); the tile belongs at its position in the frame.
      if (m.in_tiling && r.has_rect && r.has_point && m.tile_w) px += x0, py += y0;
      m.ResolveRect(src, dst, x0, y0, x1, y1, px, py);
    }
  }
  if (flags.ClearRenderTarget() || flags.ClearDepthStencil()) {
    const float color[4] = {r.clear_color[0], r.clear_color[1], r.clear_color[2], r.clear_color[3]};
    const uint32_t cf = (flags.ClearRenderTarget() ? (1u << flags.RenderTargetIndex()) : 0) | (flags.ClearDepthStencil() ? 0x30 : 0);
    m.ClearTargets(d, cf, color, r.clear_z, r.clear_stencil);
  }
}

void GameRenderer::OnTiling(const TilingRecord& r) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  Impl& m = *impl_;
  if (!m.EnsureReady()) return;
  Counters::Get().tilings++;
  if (r.begin) {
    uint32_t w = 0, h = 0;
    for (uint32_t i = 0; i < r.rect_count && i < 16; ++i) {
      const uint8_t* p = m.memory.Virtual(r.rects + 16 * i);
      if (!p) break;
      w = std::max(w, LoadBE32(p + 8));
      h = std::max(h, LoadBE32(p + 12));
    }
    m.in_tiling = true;
    m.tile_w = w;
    m.tile_h = h;
    if (r.flags) ++m.skip_reasons["BeginTiling with flags"];
    return;
  }
  // EndTiling: the whole frame into the destination texture (the original does one resolve per tile).
  const GuestDeviceView d = m.View(r.device);
  if (d.valid() && r.dest_texture) {
    Target* src = m.GetTarget(d.u32(F::render_targets, 0), false);
    uint32_t dw = 0, dh = 0;
    if (Resolved* dst = m.ResolveTexture(r.dest_texture, &dw, &dh)) {
      const int32_t w = int32_t(m.tile_w ? m.tile_w : dw), h = int32_t(m.tile_h ? m.tile_h : dh);
      m.ResolveRect(src, dst, 0, 0, std::min(w, int32_t(dw)), std::min(h, int32_t(dh)), 0, 0);
    }
  }
  m.in_tiling = false;
  m.tile_w = m.tile_h = 0;
}

void GameRenderer::OnPredication(const PredicationRecord& r) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  impl_->predication = r.mask;  // meaningless natively: every draw runs once over the whole frame
}

uint32_t GameRenderer::OnQuery(const QueryRecord& r, uint32_t* samples) {
  Impl::TimedLock lock(*impl_, kSiteQuery);
  Impl& m = *impl_;
  Counters::Get().queries++;
  if (m.opt.query_mode == 2 && m.ready) {
    switch (r.op) {
      case kQueryIssueBegin: return m.QueryBegin(r.query);
      case kQueryIssueEnd: return m.QueryEnd(r.query);
      case kQueryGetData: {
        uint32_t s = 0;
        const uint32_t st = m.QueryGetData(r.query, &s);
        if (samples) *samples = s;
        return st;
      }
      case kQueryRelease: m.queries.erase(r.query); return kQueryReady;
      default: break;
    }
  }
  switch (r.op) {
    case kSurveyBegin: m.survey_id = int32_t(r.flags); ++m.frame_surveys; break;
    case kSurveyEnd: m.survey_id = -1; break;
    case kConditionalBegin: m.conditional_id = int32_t(r.flags); break;
    case kConditionalEnd: m.conditional_id = -1; break;
    default: break;
  }
  if (m.Tracing() && r.op >= kSurveyBegin)
    m.Log("renderer: T f%llu %s %u", (unsigned long long)m.frame,
          r.op == kSurveyBegin ? "SurveyBegin" : r.op == kSurveyEnd ? "SurveyEnd" : r.op == kConditionalBegin ? "CondBegin" : "CondEnd",
          r.flags);
  return kQueryUnsupported;
}

void GameRenderer::OnShaderCreated(const ShaderCreatedRecord& r) {
  if (!r.container_data || !r.object) return;
  const auto t0 = Clock::now();
  const ShaderEntry* e = impl_->shaders.OnShaderCreated(r.object, r.container_data, r.max_size);
  Counters::Get().shaders_created++;
  std::lock_guard lock(impl_->mutex);
  if (e)
    impl_->Log("renderer: Shader %s %08X -> %016llX %s %.1f ms%s%s", r.kind == kVertexShader ? "VS" : "PS", r.object,
               (unsigned long long)e->container_hash, ShaderSourceName(e->source), MsSince(t0),
               e->source == ShaderSource::kFailed ? " error: " : "", e->source == ShaderSource::kFailed ? e->error.c_str() : "");
}

void GameRenderer::OnShaderReleased(uint32_t, uint32_t object) { impl_->shaders.OnShaderReleased(object); }

void GameRenderer::OnLock(const LockRecord& r) {
  Impl::TimedLock lock(*impl_, kSiteLock);
  Impl& m = *impl_;
  if (!m.ready || !m.opt.lock_readback) return;
  for (int i = 0; i < 4; ++i)
    if (m.resolve_objects.count(r.call.r[i]) && m.WritebackResolved(r.call.r[i])) return;
}

void GameRenderer::OnSwap(const SwapRecord& r) {
  Impl& m = *impl_;
  uint64_t wait_for = 0;
  HostDevice* host = nullptr;
  {
    Impl::TimedLock lock(m, kSiteSwap);
    if (!m.ready) return;
    m.Swap(r);
    // Back pressure (split present only): fence after this swap; the wait, without the lock, is for the swap
    // frames_ahead before it.
    if (m.opt.present_unlocked && m.opt.frames_ahead && m.host) {
      const size_t n = std::min<size_t>(m.opt.frames_ahead, m.swap_fences.size());
      uint64_t& slot = m.swap_fences[m.total_swaps % n];
      wait_for = slot;
      slot = m.host->SignalFence();
      host = m.host.get();
    }
  }
  if (host && wait_for) {
    const auto t0 = Clock::now();
    host->WaitFence(wait_for);
    m.swap_wait_us += UsBetween(t0, Clock::now());  // only the render thread writes and reads (swap)
    ++m.swap_waits;
  }
}

}  // namespace fcp::nr
