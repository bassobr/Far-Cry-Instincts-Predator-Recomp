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
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
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
#include "deferred_list.h"
#include "display_hdr.h"
#include "hdr_pass.h"
#include "overlay_pass.h"
#include "pipeline_pool.h"
#include "smaa_pass.h"
#include "colour_blind.h"
#include "ao_pass.h"
#include "texture_pack.h"

#ifdef _WIN32
#include <windows.h>  // VirtualQuery (FCP_RENDER_WATCH_VERIFY diagnostics)
#endif
#include "renderer/builtin_shaders.h"
#include "renderer/diagnostics.h"
#include "renderer/guest_device.h"
#include "renderer/present_pacer.h"
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
  // Write generation (every framebuffer use by a draw, every clear) and the generation `single` was resolved at:
  // equal = single is current, a further resolve from this target needs no new MSAA resolve.
  uint64_t writes = 0, single_at = ~0ull;
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
  uint64_t seen_epoch = 0;     // write epoch the ranges were last read at (opt.watch_epoch)
  bool volatile_data = false;  // changes constantly: hash every frame, do not watch
  uint64_t last_change_frame = ~0ull;
  int replace_kind = -1;       // TextureReplaceSource::Classify (button glyphs), -1: never replaced
  // Texture packs (core/texture_pack.h), only while dumping or replacing: the texture's name (0: not taken part).
  uint64_t pack_key = 0;
  pack::Kind pack_kind = pack::Kind::kNone;
  uint64_t dump_at = 0;        // write the dump from this frame on (unchanged until then); 0: none
  uint32_t rep_rev = 0;        // TexturePack::Revision the replacement was looked up at (0: never)
  uint32_t rep_gen = 0;        // generation of the replacement file (0: none)
  uint32_t rep_slot = 0;       // view of the uploaded replacement (0: none yet)
  Table rep_table = Table::kTex2D;
  bool rep_failed = false;     // did not load or does not fit: the game's texture
  float rep_inv_x = 1.0f, rep_inv_y = 1.0f;  // guest texels per replacement texel (kk_GuestSize)
  uint64_t used_frame = 0;     // last frame that bound it (slot recycling)
  uint64_t retry_frame = 0;    // failed only because its table was full: try again from this frame on
};

struct GuestBuf {
  nvrhi::BufferHandle buf;
  uint32_t slot = 0;
  uint64_t hash = 0;
  uint64_t checked_frame = ~0ull;
  uint64_t used_frame = 0;  // last frame that bound it (slot recycling)
  bool watched = false, volatile_data = false;
  uint64_t last_change_frame = ~0ull;
  uint64_t seen_epoch = 0;  // write epoch the data was last read at (opt.watch_epoch)
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
  // Diagnostics (statistics line `renderer: resolves per target`): resolves into it and binds of it since the last
  // line (GitHub issue #2: does a shadow map stop being refreshed or bound?).
  uint32_t interval_resolves = 0, interval_binds = 0;
  bool depth = false;  // written by a depth resolve (shadow map): [graphics] shadow_filter applies to it
  // CPU writes into the target's guest memory after the last resolve (GitHub issue #4): the range is watched from
  // each resolve on; seen_epoch is the write epoch of the resolve or of our own write-back.
  uint32_t bytes = 0;
  uint64_t seen_epoch = 0;
  bool watched = false;
  uint64_t cpu_written_frame = ~0ull;  // last frame a bind found CPU writes (diagnostics)
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
  // The struct is 100 bytes of fields padded to 104; the padding used to be uninitialised stack bytes that went
  // into Key() and the list file, so one pipeline got many keys (created again whenever the stack garbage changed,
  // the list grew by duplicates). Now an explicit field, not part of Key(): the variant word (kVariantMark | guest
  // MSAA level in bits 0-1 | bit 2 screen-sized target), from which the shipped list derives `pad` (the host sample
  // count) for the player's MSAA setting. Older lists had garbage there; it is cleared on load (StartPrewarm).
  uint32_t reserved = 0;
  uint64_t Key() const {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(this);
    uint64_t h = 0x50495045ull;
    for (size_t i = 0; i < sizeof(PipeRec) / 4 - 1; ++i) h = Mix(h, w[i]);
    return h;
  }
  // Key plus variant: one entry per variant in the lists (two variants may be the same pipeline at one setting).
  uint64_t FullKey() const { return Mix(Key(), reserved); }
};
static_assert(sizeof(PipeRec) % 8 == 0);
static_assert(sizeof(PipeRec) == 104, "PipeRec: file format of the pipeline list, no implicit padding");
constexpr uint64_t kSolidPsKey = 0x501D;
constexpr uint32_t kPipeListMagic = 0x31504346;  // "FCP1"
constexpr uint32_t kVariantMark = 0x5A000000u, kVariantMarkMask = 0xFF000000u;
bool VariantValid(uint32_t v) { return (v & kVariantMarkMask) == kVariantMark && (v & 0x00FFFFF8u) == 0; }

// Shipped pipeline list (config/predator/pipelines.fcpl, built by tools/pipelines_build.ps1; format described
// there): only shader container hashes and render state, the same file for Vulkan and D3D12.
constexpr uint32_t kShippedMagic = 0x4C504346;  // "FCPL"
constexpr uint32_t kShippedVersion = 1, kShippedHeader = 40, kShippedRecord = 112, kShippedName = 32, kShippedMaxGroups = 32;
struct ShippedRec {
  PipeRec rec;
  uint32_t groups = 0, zero = 0;
};
static_assert(sizeof(ShippedRec) == kShippedRecord);

uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// Reads and checks the shipped list (magic, version, sizes, length, checksum, every record). False: *error.
bool ReadShippedList(const std::string& path, std::vector<ShippedRec>& recs, std::vector<std::string>& groups, std::string* error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return *error = "not found", false;
  std::vector<uint8_t> b;
  uint8_t chunk[65536];
  for (size_t n; (n = std::fread(chunk, 1, sizeof(chunk), f)) > 0;) b.insert(b.end(), chunk, chunk + n);
  std::fclose(f);
  auto u32 = [&](size_t at) {
    uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
  };
  if (b.size() < kShippedHeader) return *error = "too short", false;
  if (u32(0) != kShippedMagic || u32(4) != kShippedVersion || u32(8) != kShippedHeader || u32(12) != kShippedRecord ||
      u32(24) != kShippedName || u32(20) > kShippedMaxGroups)
    return *error = "not a pipeline list version 1", false;
  const uint64_t count = u32(16), gcount = u32(20);
  if (b.size() != kShippedHeader + gcount * kShippedName + count * kShippedRecord) return *error = "wrong length", false;
  uint64_t sum;
  std::memcpy(&sum, b.data() + 32, 8);
  if (Fnv1a64(b.data() + kShippedHeader, b.size() - kShippedHeader) != sum) return *error = "checksum mismatch", false;
  for (uint64_t g = 0; g < gcount; ++g) {
    const char* s = reinterpret_cast<const char*>(b.data() + kShippedHeader + g * kShippedName);
    groups.emplace_back(s, strnlen(s, kShippedName));
  }
  const uint8_t* at = b.data() + kShippedHeader + gcount * kShippedName;
  recs.resize(size_t(count));
  for (size_t i = 0; i < count; ++i, at += kShippedRecord) {
    std::memcpy(&recs[i], at, kShippedRecord);
    if (!VariantValid(recs[i].rec.reserved) || recs[i].rec.pad != 0) return *error = "record " + std::to_string(i) + " invalid", false;
  }
  return true;
}

// Lower-case group name (level name of the StartLevel command, "menu" before the first level), [a-z0-9_-] only.
std::string GroupName(const std::string& level) {
  std::string g;
  for (char c : level) {
    const char l = char(std::tolower(static_cast<unsigned char>(c)));
    if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '_' || l == '-') g += l;
  }
  return g.empty() ? std::string("menu") : g.substr(0, 31);
}

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
  // Present timing and display pacing (present_pacer.h); after host, so it stops before the host goes away.
  PresentPacer pacer;
  uint64_t present_swap = 0;  // guest swap shown by the present in progress (BeginPresent)
  nvrhi::IDevice* dev = nullptr;
  nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::VULKAN;
  TranslatedLayout tl;
  bool ready = false, failed = false, shut_down = false;
  void* hwnd = nullptr;
  uint32_t win_w = 0, win_h = 0;
  bool resize_pending = false;
  uint32_t resize_w = 0, resize_h = 0;
  Clock::time_point lost_retry{};  // next try to make an out-of-date swapchain anew (HandleResize)
  bool vsync_pending = false;  // SetVsync: the host's present mode changes at the next present (opt.vsync)

  nvrhi::CommandListHandle cl, present_cl;
  bool cl_open = false;
  uint32_t cl_draws = 0;
  uint64_t cl_epoch = 0;

  std::unordered_map<const ShaderEntry*, nvrhi::ShaderHandle> shader_handles;
  nvrhi::ShaderHandle solid_ps, blit_vs, blit_ps, blit_gamma_ps;
  // [graphics] colour_blind: shader made on first use; the matrix of the mode in force (colour_mode)
  nvrhi::ShaderHandle blit_colour_ps;
  bool blit_colour_failed = false;
  int colour_mode = 0;
  float colour_matrix[3][4] = {};
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

  // --- Slot recycling (FCP_RENDER_SLOT_RECYCLE=0: off, the old behaviour) ------------------------------------
  // Every guest texture (per fetch constant) and every vertex buffer holds a slot in a bindless table for good;
  // the tables are finite (TableCapacities). A long session in one level can stream in new textures and geometry
  // until a table is full; from then on new textures were drawn with the black fallback and new vertex streams read
  // zeros (suspected cause of GitHub issue #1). With recycling, entries not bound for a while are evicted when a table
  // fills up; their slots return to the table three frames later (like `retired`), when no command list on the GPU
  // references them any more.
  bool slot_recycle = true;
  bool table_full = false;  // set by CreateTexture when a table refused a slot
  bool evict_now = false;   // a table refused a slot: evict at the next swap
  struct SlotRelease {
    uint64_t frame;
    Table table;
    uint32_t slot;
  };
  std::vector<SlotRelease> slot_releases;
  std::vector<std::pair<uint64_t, nvrhi::BufferHandle>> retired_buffers;
  uint64_t evicted_textures = 0, evicted_buffers = 0, evict_passes = 0;
  uint64_t stale_resolve_binds = 0, stale_resolve_logged = 0;  // fetch at a resolve target's base, other size
  uint64_t a2m_draws = 0, a2m_logged = 0;  // draws with RB_COLORCONTROL alpha-to-mask (FCP_RENDER_ALPHA_TO_MASK)
  // GitHub issue #1: device allocations. Vulkan allows maxMemoryAllocationCount of them (4096 on some drivers; NVIDIA
  // 621 on Windows reports 4294967295, measured) and
  // NVRHI makes one per texture and buffer; past the limit (or out of memory) a creation fails and was silent (black
  // texture, zero vertex stream). Now each failure is counted and logged, the entry is tried again later, and unused
  // entries are evicted (also index buffers) once the estimated count passes 3/4 of the limit.
  bool alloc_failed = false;     // set by CreateTexture when the device refused the texture
  uint64_t buf_retry_frame = 0;  // after a refused vertex buffer: no new ones before this frame
  bool alloc_pressure = false;   // evict regardless of the tables' fill (allocation failure or estimate high)
  uint64_t alloc_fail_tex = 0, alloc_fail_buf = 0, alloc_fail_idx = 0, evicted_index = 0, alloc_passes = 0;
  std::vector<nvrhi::BufferHandle> test_allocs;  // FCP_RENDER_TEST_ALLOCS: extra allocations held for good (diagnostics)
  // Device allocations of this renderer, estimated: own ones per buffer and per texture unless pooled, targets with a
  // resolve copy, the fixed objects, the test allocations.
  size_t EstimatedAllocations() const {
    return buffers.size() + index_buffers.size() + (host && host->pooled_textures() ? textures.size() / 32 : textures.size()) +
           2 * targets.size() + 2 * resolved.size() + test_allocs.size() + 64;
  }
  bool AllocationsPressed() const {
    const uint32_t limit = alloc_limit_override ? alloc_limit_override : (host ? host->max_allocations() : 0);
    return limit && EstimatedAllocations() * 4 >= uint64_t(limit) * 3;
  }
  uint32_t alloc_limit_override = 0;  // FCP_RENDER_ALLOC_LIMIT (diagnostics: evict as if the limit were this)
  // FCP_RENDER_TEST_ALLOC_FAIL: refuse guest resources as a full device would (diagnostics)
  bool SimulatedRefusal() const { return opt.test_alloc_fail && EstimatedAllocations() >= opt.test_alloc_fail; }
  void AllocFailureLog(const char* what, uint64_t n, const char* detail) {
    if (n <= 16 || n % 1000 == 0)
      Log("renderer: %s refused by the device (%s; failure %llu; estimated allocations %zu of %u): drawn without it, %s",
          what, detail, (unsigned long long)n, EstimatedAllocations(), host ? host->max_allocations() : 0,
          opt.alloc_evict && slot_recycle ? "unused resources are evicted and it is tried again"
                                          : "no eviction (FCP_RENDER_ALLOC_EVICT=0 or FCP_RENDER_SLOT_RECYCLE=0)");
  }
  static constexpr uint64_t kEvictAge = 600;      // frames without a binding before an entry may be evicted
  static constexpr uint64_t kEvictAgeShort = 60;  // when a table stays nearly full after a normal pass

  void ReleaseSlot(Table table, uint32_t slot) {
    if (slot) slot_releases.push_back({frame, table, slot});
  }
  static bool Pressed(uint32_t used, uint32_t capacity, uint32_t num, uint32_t den) { return uint64_t(used) * den >= uint64_t(capacity) * num; }
  bool TablePressed(Table t, uint32_t num, uint32_t den) const { return Pressed(tl.used(t), tl.capacity(t), num, den); }

  // At a swap: evict entries not bound for `age` frames from tables at least 3/4 full (all = every table and the index
  // buffers: allocation pressure).
  size_t EvictPass(uint64_t age, bool all = false) {
    bool tex_pressed[4] = {};
    bool any_tex = false;
    for (uint32_t i = 0; i < 4; ++i) any_tex |= tex_pressed[i] = all || TablePressed(Table(i), 3, 4);
    const bool buf_pressed = all || TablePressed(Table::kBuffers, 3, 4);
    if (all) {
      for (auto it = index_buffers.begin(); it != index_buffers.end();) {
        if (it->second.used_frame + age < frame) {
          if (it->second.buf) retired_buffers.emplace_back(frame, it->second.buf);
          it = index_buffers.erase(it);
          ++evicted_index;
        } else {
          ++it;
        }
      }
    }
    size_t n = 0;
    if (any_tex) {
      for (auto it = textures.begin(); it != textures.end();) {
        GuestTex& t = it->second;
        if (t.slot && uint32_t(t.table) < 4 && tex_pressed[uint32_t(t.table)] && t.used_frame + age < frame) {
          ReleaseSlot(t.table, t.slot & 0x7FFFFFFFu);
          DropReplacement(t);
          if (t.tex) retired.emplace_back(frame, t.tex);
          it = textures.erase(it);
          ++evicted_textures, ++n;
        } else {
          ++it;
        }
      }
    }
    if (buf_pressed) {
      for (auto it = buffers.begin(); it != buffers.end();) {
        GuestBuf& b = it->second;
        if (b.slot && b.used_frame + age < frame) {
          ReleaseSlot(Table::kBuffers, b.slot);
          if (b.buf) retired_buffers.emplace_back(frame, b.buf);
          it = buffers.erase(it);
          ++evicted_buffers, ++n;
        } else {
          ++it;
        }
      }
    }
    return n;
  }

  void RecycleSlots() {
    // Slots released at least three frames ago go back to their tables.
    slot_releases.erase(std::remove_if(slot_releases.begin(), slot_releases.end(), [&](const SlotRelease& r) {
                          if (r.frame + 3 >= frame) return false;
                          tl.FreeSlot(r.table, r.slot);
                          return true;
                        }), slot_releases.end());
    retired_buffers.erase(std::remove_if(retired_buffers.begin(), retired_buffers.end(), [&](const auto& e) { return e.first + 3 < frame; }),
                          retired_buffers.end());
    if (!slot_recycle) return;
    if (frame % 120 == 0 && opt.alloc_evict && AllocationsPressed()) alloc_pressure = true;
    bool pressed = evict_now || alloc_pressure;
    if (!pressed && frame % 120 == 0)
      for (uint32_t i = 0; i < uint32_t(Table::kCount); ++i)
        if (Table(i) != Table::kSamplers && TablePressed(Table(i), 3, 4)) pressed = true;
    if (!pressed) return;
    evict_now = false;
    if (alloc_pressure) {
      // Device allocations near the limit (or one failed): unused entries of every kind go, whatever the tables say.
      alloc_pressure = false;
      const size_t before = EstimatedAllocations(), before_idx = index_buffers.size();
      const uint64_t before_tex = evicted_textures, before_buf = evicted_buffers;
      EvictPass(kEvictAge, true);
      if (AllocationsPressed()) EvictPass(kEvictAgeShort, true);
      ++alloc_passes;
      if (alloc_passes <= 20 || alloc_passes % 100 == 0)
        Log("renderer: allocation pass %llu at frame %llu: evicted %llu textures, %llu vertex buffers, %zu index buffers; "
            "estimated allocations %zu -> %zu of %u", (unsigned long long)alloc_passes, (unsigned long long)frame,
            (unsigned long long)(evicted_textures - before_tex), (unsigned long long)(evicted_buffers - before_buf),
            before_idx - index_buffers.size(), before, EstimatedAllocations(), host ? host->max_allocations() : 0);
      return;
    }
    const size_t before_tex = textures.size(), before_buf = buffers.size();
    size_t n = EvictPass(kEvictAge);
    bool nearly_full = false;
    for (uint32_t i = 0; i < uint32_t(Table::kCount); ++i)
      if (Table(i) != Table::kSamplers && TablePressed(Table(i), 19, 20)) nearly_full = true;
    // Released slots only count after three frames: judge by what this pass will give back.
    if (nearly_full && n * 20 < size_t(tl.capacity(Table::kTex2D))) n += EvictPass(kEvictAgeShort);
    ++evict_passes;
    if (evict_passes <= 20 || evict_passes % 100 == 0)
      Log("renderer: slot recycling pass %llu at frame %llu: evicted %zu textures (%zu -> %zu) and %zu buffers (%zu -> %zu); "
          "2D %u/%u, buffers %u/%u",
          (unsigned long long)evict_passes, (unsigned long long)frame, before_tex - textures.size(), before_tex, textures.size(),
          before_buf - buffers.size(), before_buf, buffers.size(), tl.used(Table::kTex2D), tl.capacity(Table::kTex2D),
          tl.used(Table::kBuffers), tl.capacity(Table::kBuffers));
  }

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

  // Texture packs (core/texture_pack.h; [graphics] texture_dump / texture_replace). Null while both are off: then
  // nothing below runs (one pointer test per conversion and per binding of a converted texture).
  std::unique_ptr<pack::TexturePack> tpack;
  // Uploaded replacements, shared by every fetch constant of the same texture (each has its own view).
  struct PackTexKey {
    uint64_t key;
    uint32_t gen;
    bool operator==(const PackTexKey&) const = default;
  };
  struct PackTexKeyHash {
    size_t operator()(const PackTexKey& k) const { return size_t(Mix(k.key, k.gen)); }
  };
  struct PackTex {
    nvrhi::TextureHandle tex;
    std::shared_ptr<const res::HostTextureData> data;  // until uploaded
    size_t next = 0;                                   // next subresource to upload
    bool done = false;
    uint32_t views = 0;  // GuestTex entries with a view of it
    uint32_t width = 0, height = 0;
    bool cube = false;
    uint64_t used_frame = 0;
  };
  std::unordered_map<PackTexKey, PackTex, PackTexKeyHash> pack_tex;
  static constexpr uint64_t kDumpSettle = 20;  // frames a texture stays unchanged before it is dumped
  uint64_t pack_upload_budget = 16u << 20;     // bytes of replacement uploads per frame (FCP_TEXTURE_UPLOAD_MB)
  uint64_t pack_budget_frame = ~0ull, pack_budget_used = 0;
  uint64_t pack_dump_frame = ~0ull, pack_dump_bytes = 0;  // re-conversions for dumps in this frame
  uint64_t pack_keys = 0, pack_skipped = 0, pack_volatile = 0, pack_dumps = 0, pack_binds = 0, pack_uploads = 0;
  uint64_t pack_upload_bytes = 0, pack_key_us = 0, pack_dump_us = 0, pack_upload_us = 0, pack_frames_upload_capped = 0;
  uint64_t pack_logged_binds = 0, pack_logged_dumps = 0;

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
  // --- Ambient occlusion ([graphics] ao; core/ao_pass.h) ------------------------------------------------------
  // Where it goes in (measured in the frame traces, docs: ambient occlusion report): the 3D world goes with depth
  // test into one screen-sized colour target and its depth (the tiling target 1280x736). The occlusion is
  // multiplied in once per 3D view, after its opaque draws (depth test and depth write on, no blending) and before
  // the first draw into that colour target that blends, the first resolve or clear of that target or its depth, or a
  // draw that writes its depth with another colour target. Projection from the camera hook (SetCameraProjection),
  // depth mapping from the viewport of the opaque draws. Left out (log, FCP_TRACE_AO) when something does not fit:
  // no projection yet, the projection's shape not the view's, a projection that is not PerspectiveFovRH.
  std::unique_ptr<AoPass> ao;
  bool ao_failed = false;
  struct AoView {
    bool pending = false;
    Target* color = nullptr;
    Target* depth = nullptr;
    float vport[6] = {};      // PA_CL_VPORT of the opaque draws: x scale, x offset, y scale, y offset, z scale, z offset
    uint32_t vte = 0, clip = 0, opaque = 0, kept = 0;
    uint32_t window_offset = 0;
    bool half_pixel = false;  // PA_SU_VTX_CNTL pixel centre: +0.5 (ComputeViewport)
    uint32_t depth_control = 0;  // RB_DEPTHCONTROL of the first opaque draw (which way the depth runs)
  } ao_view;
  std::vector<std::array<float, 4>> ao_done;  // views (guest viewport) that got their occlusion this frame
  std::mutex proj_mutex;                      // camera hook (any thread) -> render thread; never held with another
  float proj_m[16] = {};
  float proj_far = 0;
  uint64_t proj_count = 0;
  uint64_t ao_applied = 0, ao_frames = 0, ao_skipped_shape = 0, ao_skipped_proj = 0, ao_dropped = 0;
  uint64_t ao_applied_at_swap = 0, ao_swaps = 0, ao_opaque_after = 0;
  std::set<std::string> ao_traced;
  nvrhi::TimerQueryHandle ao_timer[4];
  uint32_t ao_timer_next = 0, ao_timer_pending = 0;
  double ao_ms_sum = 0;
  uint32_t ao_ms_n = 0;
  bool overlay_failed = false;
  nvrhi::CommandListHandle overlay_cl;
  uint64_t overlay_frames = 0, overlay_draws = 0;
  std::string present_dump;  // RequestPresentDump: at the next EndPresent
  // --- HDR10 output ([display] hdr; core/hdr_pass.h) ------------------------------------------------------------
  // With an HDR10 swapchain the SDR present path draws into hdr_sdr (RGBA8, the swapchain's size) instead of the
  // swapchain; the HDR10 pass maps it into the swapchain, the overlay follows at paper white. Off: nothing of this.
  DisplayHdrInfo display_hdr;
  bool hdr_on = false;        // the swapchain is HDR10 and the pass is ready
  bool hdr_pending = false;   // SetHdr: switch at the next present
  std::unique_ptr<HdrPass> hdr_pass;
  bool hdr_pass_failed = false;
  HdrLevels hdr_levels;
  nvrhi::TextureHandle hdr_sdr;
  nvrhi::FramebufferHandle hdr_sdr_fb;
  float hdr_rect[4] = {};     // the picture inside the swapchain image (x, y, w, h) of the latest present
  uint64_t hdr_frames = 0;

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

  // Native query begin/end: into the deferred list while it records (keeps the order), else directly.
  void QueryOp(nvrhi::ICommandList* c, bool begin, uint32_t slot) {
    if (dlist && c == dlist.Get() && dlist->recording()) {
      dlist->Defer([this, begin, slot](nvrhi::ICommandList* r) { begin ? occ->Begin(r, slot) : occ->End(r, slot); });
      return;
    }
    begin ? occ->Begin(c, slot) : occ->End(c, slot);
  }
  void BeginSegment(nvrhi::ICommandList* c, HostQuery& q) {
    if (api == nvrhi::GraphicsAPI::VULKAN) c->clearState();  // query not inside a render pass
    const uint32_t slot = next_slot++ % occ->capacity();
    QueryOp(c, true, slot);
    q.slots.push_back(slot);
    active_segment = true;
  }
  void EndSegment(nvrhi::ICommandList* c, HostQuery& q) {
    if (!active_segment || q.slots.empty()) return;
    if (api == nvrhi::GraphicsAPI::VULKAN) c->clearState();
    QueryOp(c, false, q.slots.back());
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
  // Factor per axis; with Hor+ screen-sized targets are cur_wide times wider in x as well, with Vert+ cur_tall times
  // taller in y (anamorphic). cur_sx/cur_sy: FactorsFor (scale mode: the previous computation).
  float cur_sx = 1.0f, cur_sy = 1.0f;
  void ScaleFor(uint32_t w, uint32_t h, float& sx, float& sy) const {
    if (w == h && w >= 1024) {
      sx = sy = opt.shadow_scale;
    } else if (IsScreenTarget(w, h)) {
      sx = cur_sx;
      sy = cur_sy;
    } else {
      sx = sy = 1.0f;
    }
  }
  bool IsScreenTarget(uint32_t w, uint32_t h) const {
    return w >= opt.scale_min_width && (w > h || (opt.split_views && IsSplitHalf(w, h)));
  }
  // Split screen with 2 players side by side: each view renders into its own 640x720 target (tiling 640x736), then is
  // resolved into its half of the 1280x720 picture (measured, m20_shantytown). As a screen target it gets the screen's
  // factors, like the quarters (640x360) of 3 and 4 players and the halves of the horizontal split; else it stayed at
  // the console's size and was stretched into the wide picture. The camera widens it as well (fcp/camera_fov.cpp).
  static bool IsSplitHalf(uint32_t w, uint32_t h) { return w == 640 && (h == 720 || h == 736); }
  // The targets of a split screen's views: halves (640x720, 1280x360), quarters (640x360); 368 and 736 are the
  // tiling heights of 360 and 720.
  static bool IsSplitTarget(uint32_t w, uint32_t h) {
    return (w == 640 && (h == 720 || h == 736 || h == 360 || h == 368)) || (w == 1280 && (h == 360 || h == 368));
  }
  static uint32_t SplitViewHeight(uint32_t h) { return h == 368 ? 360 : h == 736 ? 720 : h; }

  // --- Hor+ -------------------------------------------------------------------------------------------------
  // Anamorphic: screen-sized targets are cur_wide times wider on the host, the guest coordinates stay (1280 wide).
  // The camera (fcp/camera_fov.cpp) draws the world that much wider into these 1280 guest pixels; the host stretches
  // it back. A picture with a 3D view is shown in the wide shape, all others (menus, videos, loading screens) at 16:9
  // (showing them undoes the stretch). Over a 3D picture, 2D draws after its post processing (HUD, texts, menus over
  // the game) go into the 16:9 middle (narrower host viewport), unless they cover the whole width (post processing,
  // fades, bars).
  // Vert+ is the same in y: with a picture narrower than 16:9 the targets are cur_tall times taller, the camera sees
  // more above and below, 2D after the post processing goes into the 16:9 band in the middle (narrower in y).
  float cur_wide = 1.0f, wanted_wide = 1.0f;
  float cur_tall = 1.0f, wanted_tall = 1.0f;
  std::atomic<float> wide_published{1.0f};  // for the camera hook (Wide())
  std::atomic<float> tall_published{1.0f};  // for the camera hook (Tall())
  uint32_t out_w = 0, out_h = 0;            // swapchain size (native resolution), 0: none
  bool Shaped() const { return cur_wide != 1.0f || cur_tall != 1.0f; }
  bool frame_3d = false;      // since the last swap: a draw with depth test into a screen-sized target
  bool post_3d = false;       // after that: a resolve from the 3D view's target (post processing begins)
  uint32_t view_surface = 0;  // colour target of the 3D view
  bool front_wide = false;    // the shown picture has a 3D view (shown in the wide / tall shape)
  float front_aspect = 16.0f / 9.0f;  // its shape (fixed resolution: the targets may have another one)
  uint64_t wide_frames = 0, narrow_draws = 0, full_draws = 0, unknown_boxes = 0;
  std::set<uint64_t> wide_traced;  // FCP_TRACE_WIDE: kinds already logged
  // HUD at the edges and screen masks (HorPlusNarrow, Draw). Decision for the draw HorPlusNarrow last returned true for:
  struct HudDecision {
    bool known = false;  // box read
    bool mask = false;   // screen mask: narrowed whole, not cut to the band, sides filled
    int ax = 0, ay = 0;  // anchor: -1 left/top, 0 band, +1 right/bottom
    float box[4] = {};   // guest pixels x0, y0, x1, y1
    int view = -1;       // split screen: the view the draw belongs to (-1: the screen)
    float vr[4] = {};    // that view's rectangle (guest pixels), else the whole target
  } hud;
  struct MaskSpan {
    float box[4];
    uint32_t count;
    int view = -1;  // split screen: the view it covers (index into views_*; -1: the screen)
  };
  // Split screen (GameRendererOptions::split_views): the views of this and the last frame, in guest pixels of the
  // screen target (x0, y0, x1, y1), from the viewports of the 3D draws that are a part of the target. With one view
  // (single player) none is listed and everything works about the screen as before.
  struct SplitView {
    float r[4];
  };
  SplitView views_now[4] = {}, views_last[4] = {};
  int n_views_now = 0, n_views_last = 0;
  uint64_t stat_split_frames = 0, stat_view_hud = 0, stat_view_masks = 0, stat_window_offset = 0;
  std::set<uint64_t> views_logged;
  MaskSpan masks_now[8] = {}, masks_last[8] = {};  // screen masks of this and the last frame (nesting, HUD anchors)
  int n_masks_now = 0, n_masks_last = 0;
  struct HudRow {
    float top, bottom;
  };
  HudRow rows_now[16] = {}, rows_last[16] = {};  // rows of the message column (hints) of this and the last frame
  int n_rows_now = 0, n_rows_last = 0;
  bool hud_cover = false;  // this frame: a quad of the HUD covered the screen (pause menu's dimming): the rest stays
  uint64_t hud_vs_now = 0, hud_vs_last = 0;  // vertex shader of the band's 2D draws (this and the last frame)
  std::set<uint64_t> masks_logged;           // kinds of screen masks already in the log
  uint64_t stat_masks = 0, stat_fills = 0, stat_anchored = 0, stat_covered = 0;
  // Aspect of the 3D picture -> factor over 16:9 (1: off). Up to 1 % above 16:9 counts as 16:9.
  float PictureAspect(uint32_t swap_w, uint32_t swap_h) const {
    return opt.aspect > 0.0f ? opt.aspect : (swap_w && swap_h ? float(swap_w) / float(swap_h) : 16.0f / 9.0f);
  }
  float WideFor(uint32_t swap_w, uint32_t swap_h) const {
    const float w = PictureAspect(swap_w, swap_h) / (16.0f / 9.0f);
    return w <= 1.01f ? 1.0f : std::min(w, 4.0f);
  }
  // ... and under 16:9 (Vert+, 1: off). Up to 1 % under 16:9 counts as 16:9; at most square (portrait: bars).
  float TallFor(uint32_t swap_w, uint32_t swap_h) const {
    if (!opt.vert_plus) return 1.0f;
    const float t = (16.0f / 9.0f) / PictureAspect(swap_w, swap_h);
    return t <= 1.01f ? 1.0f : std::min(t, 16.0f / 9.0f);
  }
  uint32_t screen_samples = 0;  // [graphics] msaa = 2|4|8, limited to the device (0: like the console)
  // scale = auto: the output's height (with Vert+ the output's width: the band of 16:9 in the middle).
  static float AutoScale(uint32_t swap_h, float tall = 1.0f) {
    if (tall == 1.0f) return swap_h >= 720 ? float(swap_h) / 720.0f : 1.0f;
    return std::max(1.0f, float(swap_h) / (720.0f * tall));
  }
  // Host factors of the 1280x720 guest screen targets. Scale mode: scale x wide / tall (exactly the previous values
  // with both 1). Native: the size of the picture in the output (bars included in the fit). Fixed: res_w x res_h.
  void FactorsFor(float s, float wide, float tall, float& sx, float& sy) const {
    if (opt.resolution_mode == 1 && out_w && out_h) {
      const float p = (16.0f / 9.0f) * wide / tall;
      float pw = float(out_w), ph = float(out_h);
      if (pw / ph > p) pw = std::round(ph * p);
      else ph = std::round(pw / p);
      sx = pw / 1280.0f, sy = ph / 720.0f;
    } else if (opt.resolution_mode == 2 && opt.res_w && opt.res_h) {
      sx = float(opt.res_w) / 1280.0f, sy = float(opt.res_h) / 720.0f;
    } else {
      sx = wide == 1.0f ? s : s * wide;
      sy = tall == 1.0f ? s : s * tall;
      return;
    }
    sx = std::clamp(sx, 0.25f, 8.0f), sy = std::clamp(sy, 0.25f, 8.0f);
  }
  // Pixel density of the 16:9 band (HUD, texts; the button glyphs are drawn for it).
  float HudScale() const {
    if (opt.resolution_mode == 0) return cur_scale;
    return std::min(cur_sx / cur_wide, cur_sy / cur_tall);
  }
  fcshaders::ScaleConstants scale_c{}, last_scale_c{};
  uint64_t scale_epoch = 0;
  float tex_inv_x = 1.0f, tex_inv_y = 1.0f;  // from Texture(): guest/host of the last returned texture
  bool tex_depth = false;                    // from Texture(): the last returned texture is a depth resolve

  // Scales a rectangle edge (guest pixels -> host pixels).
  static int32_t ScaleEdge(int32_t v, float s) { return s == 1.0f ? v : int32_t(std::lround(double(v) * s)); }

  // New factor (scale = auto after a resize): recreate the targets (EDRAM content only lives within a frame), carry
  // resolve textures with content over to the new size (textures resolved once are preserved that way).
  void ApplyScale(float s, float wide, float tall) {
    float nsx = 1.0f, nsy = 1.0f;
    FactorsFor(s, wide, tall, nsx, nsy);
    if (s == cur_scale && wide == cur_wide && tall == cur_tall && nsx == cur_sx && nsy == cur_sy) return;
    if (!dev) {  // no device yet: the first targets get these values
      cur_scale = s, cur_wide = wide, cur_tall = tall, cur_sx = nsx, cur_sy = nsy;
      wide_published.store(wide), tall_published.store(tall);
      return;
    }
    Flush();
    dev->waitForIdle();
    Log("renderer: render scale %.3f -> %.3f, Hor+ %.4f -> %.4f, Vert+ %.4f -> %.4f, screen targets %ux%u -> %ux%u (targets "
        "recreated, %zu resolve textures carried over)",
        cur_scale, s, cur_wide, wide, cur_tall, tall, HostSize(1280, cur_sx), HostSize(720, cur_sy), HostSize(1280, nsx),
        HostSize(720, nsy), resolved.size());
    cur_scale = s;
    cur_wide = wide;
    cur_tall = tall;
    cur_sx = nsx, cur_sy = nsy;
    wide_published.store(wide);
    tall_published.store(tall);
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
  uint64_t fast_const_skips = 0, fast_const_converts = 0, const_span_bytes = 0;
  uint64_t msaa_resolves = 0, msaa_resolves_skipped = 0;
  const bool defer_all = std::getenv("FCP_RENDER_DEFER_ALL") && std::string(std::getenv("FCP_RENDER_DEFER_ALL")) == "1";
  const bool msaa_resolve_once = std::getenv("FCP_RENDER_MSAA_RESOLVE_ONCE") == nullptr ||
                                 std::string(std::getenv("FCP_RENDER_MSAA_RESOLVE_ONCE")) != "0";
  const bool const_span = std::getenv("FCP_RENDER_CONST_SPAN") == nullptr || std::string(std::getenv("FCP_RENDER_CONST_SPAN")) != "0";
  fcshaders::DrawConstants last_dc{};
  uint64_t constants_epoch = 0, constant_uploads = 0;
  // Versions of the volatile constant buffers (VS/PS/Draw) per frame and maximum in the statistics interval.
  std::array<uint32_t, 3> cb_frame{}, cb_max{};
  std::vector<uint32_t> scratch_indices;
  std::vector<res::VertexElement> elements;
  nvrhi::GraphicsState draw_gs;  // reused by every draw (Draw)

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
    if (opt.pipeline_list.empty() || !listed_keys.insert(r.FullKey()).second) return;
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

  // FCP_PIPELINE_COLLECT: every record a draw uses, once per group, into <folder>/<group>.bin (FCP1 format).
  std::mutex collect_mutex;
  std::string collect_group = "menu";
  std::set<uint64_t> collect_seen;
  void CollectRecord(const PipeRec& r) {
    std::lock_guard lock(collect_mutex);
    uint64_t gk = r.FullKey();
    for (char c : collect_group) gk = Mix(gk, uint8_t(c));
    if (!collect_seen.insert(gk).second) return;
    const std::string path = opt.pipeline_collect + "/" + collect_group + ".bin";
    if (FILE* f = std::fopen(path.c_str(), "ab")) {
      std::fseek(f, 0, SEEK_END);
      if (std::ftell(f) == 0) {
        const uint32_t head[2] = {kPipeListMagic, uint32_t(sizeof(PipeRec))};
        std::fwrite(head, 4, 2, f);
      }
      std::fwrite(&r, sizeof(r), 1, f);
      std::fclose(f);
    }
  }

  // --- Prewarm of the local and the shipped list (pipeline_async): a feeder thread resolves the shaders of the
  // records (outside the renderer lock; may wait for a translation) and submits them to the pool's background queue.
  // Records whose shader is not known yet (the game's run-time shaders before their first translation) wait in
  // `waiting` until NotifyShader reports that hash. A draw that needs a queued or running prewarm job waits for that
  // job (AsyncPipeline: Promote), it does not create the pipeline a second time.
  std::mutex feed_mutex;  // order: renderer mutex may be held when taking it, never the other way round
  std::condition_variable feed_cv;
  std::deque<ShippedRec> feed;
  std::unordered_map<uint64_t, std::vector<ShippedRec>> waiting;
  std::atomic<size_t> waiting_count{0};
  std::vector<std::string> group_names;  // of the shipped list (bit i = group i)
  uint64_t prewarm_failed = 0;
  std::atomic<uint64_t> prewarm_parked{0};
  std::atomic<bool> feed_idle{false};
  uint32_t loading_mask = 0;  // group of the level being loaded (feeder: moved ahead too)

  // Host sample count (pad) of a shipped record for the current MSAA setting, as GetTarget would create its target.
  uint32_t PadForVariant(uint32_t variant) const {
    const uint32_t gm = variant & 3;
    uint32_t s = opt.msaa && gm ? (1u << std::min(gm, 2u)) : 1u;
    if (s > 1 && screen_samples && (variant & 4)) s = screen_samples;
    return s > 1 ? s : 0;
  }

  void NotifyShader(uint64_t hash) {
    if (waiting_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard lock(feed_mutex);
    auto it = waiting.find(hash);
    if (it == waiting.end()) return;
    for (ShippedRec& r : it->second) feed.push_back(r);
    waiting_count -= it->second.size();
    waiting.erase(it);
    feed_idle = false;
    feed_cv.notify_one();
  }

  void Park(uint64_t hash, const ShippedRec& r) {
    {
      std::lock_guard lock(feed_mutex);
      waiting[hash].push_back(r);
      ++waiting_count;
    }
    // The shader may have arrived between the lookup and parking: look again.
    if (shaders.FindByHash(hash)) NotifyShader(hash);
  }

  void Feeder() {
    const auto t0 = Clock::now();
    bool logged = false;
    for (;;) {
      std::vector<ShippedRec> batch;
      {
        std::unique_lock lock(feed_mutex);
        if (feed.empty()) feed_idle = true;
        if (feed.empty() && !logged) {
          logged = true;
          lock.unlock();
          std::lock_guard rl(mutex);
          Log("renderer: prewarm: %llu pipelines queued in %.0f ms, %zu waiting for their shaders, %llu without shaders",
              (unsigned long long)prewarm_jobs, MsSince(t0), waiting_count.load(), (unsigned long long)prewarm_failed);
          continue;
        }
        feed_cv.wait(lock, [&] { return prewarm_stop.load() || !feed.empty(); });
        if (prewarm_stop) return;
        feed_idle = false;
        while (!feed.empty() && batch.size() < 64) batch.push_back(feed.front()), feed.pop_front();
      }
      std::vector<const ShaderEntry*> vs(batch.size()), ps(batch.size());
      for (size_t i = 0; i < batch.size(); ++i) {
        if (prewarm_stop) return;
        vs[i] = shaders.FindByHash(batch[i].rec.vs);
        ps[i] = batch[i].rec.ps && vs[i] ? shaders.FindByHash(batch[i].rec.ps) : nullptr;
      }
      std::vector<std::pair<uint64_t, ShippedRec>> park;
      {
        std::lock_guard lock(mutex);
        if (!ready || shut_down) return;
        for (size_t i = 0; i < batch.size(); ++i) {
          const PipeRec& r = batch[i].rec;
          const uint64_t key = r.Key();
          if (pipelines.count(key) || pending.count(key)) continue;
          if (!vs[i]) { park.emplace_back(r.vs, batch[i]); continue; }
          if (r.ps && !ps[i]) { park.emplace_back(r.ps, batch[i]); continue; }
          nvrhi::IShader* vs_h = Handle(vs[i]);
          nvrhi::IShader* ps_h = r.ps ? Handle(ps[i]) : nullptr;
          if (!vs_h || (r.ps && !ps_h)) {
            ++prewarm_failed;
            continue;
          }
          Pending p;
          p.job = MakeJob(r, vs_h, ps_h);
          p.job->groups = batch[i].groups;
          p.rec = r;
          p.listed = true;
          auto it = pending.emplace(key, std::move(p)).first;
          pool.Submit(it->second.job, false);
          ++prewarm_jobs;
        }
        if (loading_mask) pool.PrioritizeBackground(loading_mask);
      }
      prewarm_parked += park.size();
      for (auto& [h, r] : park) Park(h, r);
    }
  }
  // --- Pipelines on worker threads (opt.pipeline_async, FCP_RENDER_PIPELINE_ASYNC) ----------------------------
  // A draw whose pipeline is missing hands it to the pool and records the rest of the command list into dlist
  // (deferred_list.h); Flush waits for the list's pipelines and replays: same commands, same order, same image, but
  // the pipelines of one list are made in parallel. The pipeline list is prewarmed by the same pool (background).
  struct Pending {
    PipelineJobPtr job;
    nvrhi::GraphicsPipelineHandle placeholder;  // PendingPipeline for the GraphicsState of deferred draws
    PipeRec rec{};
    bool listed = false;  // from the pipeline list (prewarm): not appended again
    bool solid = false;
  };
  PipelinePool pool;
  nvrhi::RefCountPtr<DeferredCommandList> dlist;
  // King Kong's slab workers for large textures (>= 256 KB: subresources split into slabs, same bytes); null = serial.
  // FCP_RENDER_TEXTURE_WORKERS=0: serial as before.
  std::unique_ptr<res::TextureConversionWorkers> tex_workers;
  std::unordered_map<uint64_t, Pending> pending;
  std::vector<uint64_t> list_jobs;  // keys the deferred list waits for
  uint64_t async_lists = 0, async_jobs = 0, async_wait_us = 0, async_create_us = 0, async_replayed_ops = 0;
  uint64_t async_replayed_bytes = 0, async_materialize_early = 0, prewarm_jobs = 0, prewarm_done = 0, async_late = 0;
  Clock::time_point prewarm_t0{};
  bool prewarm_logged = false;

  PipelineJobPtr MakeJob(const PipeRec& r, nvrhi::IShader* vs_h, nvrhi::IShader* ps_h) {
    auto job = std::make_shared<PipelineJob>();
    job->key = r.Key();
    job->make = [this, r, vs = nvrhi::ShaderHandle(vs_h), ps = nvrhi::ShaderHandle(ps_h)]() {
      return dev->createGraphicsPipeline(PipelineDescFrom(r, vs, ps, tl.layouts()), FramebufferInfoFrom(r));
    };
    return job;
  }

  // A finished job into the memory cache (render thread, under the lock). Returns the pipeline (null: failed).
  nvrhi::IGraphicsPipeline* Adopt(uint64_t key) {
    auto it = pending.find(key);
    if (it == pending.end()) return nullptr;
    Pending& p = it->second;
    nvrhi::GraphicsPipelineHandle h = p.job->result;
    async_create_us += uint64_t(p.job->create_ms * 1000.0);
    if (p.listed) {
      ++prewarm_done;
    } else {
      Counters& cn = Counters::Get();
      if (h) cn.pipelines_created++, ++frame_new_pipelines;
      if (h && !p.solid) AppendPipelineRecord(p.rec);
    }
    pending.erase(it);
    return pipelines.emplace(key, h).first->second;
  }
  // Finished background jobs into the memory cache (at every swap).
  void AdoptFinished() {
    for (auto it = pending.begin(); it != pending.end();) {
      const uint64_t key = it->first;
      const bool done = it->second.job->state.load() == PipelineJob::kDone;
      const bool waited = std::find(list_jobs.begin(), list_jobs.end(), key) != list_jobs.end();
      ++it;
      if (done && !waited) Adopt(key);
    }
    if (prewarm_jobs && !prewarm_logged && prewarm_done >= prewarm_jobs && feed_idle) {
      prewarm_logged = true;
      Log("renderer: prewarm done: %llu pipelines in %.0f ms on %u threads (%zu still waiting for their shaders)",
          (unsigned long long)prewarm_done, MsSince(prewarm_t0), pool.threads(), waiting_count.load());
    }
  }
  // Draw path: the pipeline for `key` from the pool (null: not async). Done jobs are adopted; otherwise the rest of
  // the list is recorded and the placeholder returned.
  nvrhi::IGraphicsPipeline* AsyncPipeline(uint64_t key, const PipeRec& rec, nvrhi::IShader* vs_h, nvrhi::IShader* ps_h, bool solid) {
    auto it = pending.find(key);
    if (it == pending.end()) {
      Pending p;
      p.job = MakeJob(rec, vs_h, ps_h);
      p.rec = rec;
      p.solid = solid;
      it = pending.emplace(key, std::move(p)).first;
      pool.Submit(it->second.job, true);
      ++async_jobs;
    } else {
      pool.Promote(it->second.job);
    }
    Pending& p = it->second;
    if (p.job->state.load() == PipelineJob::kDone && std::find(list_jobs.begin(), list_jobs.end(), key) == list_jobs.end()) return Adopt(key);
    if (!p.placeholder)
      p.placeholder = new PendingPipeline(p.job, PipelineDescFrom(rec, vs_h, ps_h, tl.layouts()), FramebufferInfoFrom(rec));
    if (std::find(list_jobs.begin(), list_jobs.end(), key) == list_jobs.end()) list_jobs.push_back(key);
    if (!dlist->recording()) {
      CL();  // the real list is open before recording starts
      dlist->Begin(cl);
      ++async_lists;
    }
    return p.placeholder;
  }
  // Waits for the deferred list's pipelines and replays it onto the real list (Flush, or a call that cannot be
  // recorded). The wait is the render thread's share of the pipeline time (counted as pipeline creation).
  void Materialize() {
    if (!dlist || !dlist->recording()) return;
    const auto t0 = Clock::now();
    // FCP_RENDER_PIPELINE_MAX_WAIT_MS (not the default): draws whose pipeline is not ready by then are left out of
    // this list (changes the image for a frame or two); the job goes on and is used once it is done.
    const auto deadline = opt.pipeline_max_wait_ms < 0 ? Clock::time_point::max()
                                                      : t0 + std::chrono::microseconds(int64_t(opt.pipeline_max_wait_ms * 1000.0));
    std::vector<uint64_t> keys, late;
    keys.swap(list_jobs);
    for (uint64_t key : keys) {
      auto it = pending.find(key);
      if (it == pending.end()) continue;
      Pending& p = it->second;
      const bool done = pool.WaitUntil(p.job, deadline);
      if (auto* pp = static_cast<PendingPipeline*>(p.placeholder.Get())) pp->Resolve(done ? p.job->result.Get() : nullptr);
      if (!done) late.push_back(key);
    }
    const uint64_t waited = uint64_t(MsSince(t0) * 1000.0);
    async_wait_us += waited;
    Counters::Get().pipeline_create_us += waited;
    async_replayed_ops += dlist->ops();
    async_replayed_bytes += dlist->bytes();
    async_late += late.size();
    dlist->Replay();
    for (uint64_t key : keys)
      if (std::find(late.begin(), late.end(), key) == late.end()) Adopt(key);
  }

  // Reads the list and creates every pipeline whose shaders are in the pack or disk cache, on a thread (the driver
  // compiles in parallel with the game; the draw path picks it up as soon as it is in the memory cache).
  void StartPrewarm() {
    if (!opt.pipeline_prewarm) return;
    std::vector<PipeRec> recs;
    if (!opt.pipeline_list.empty())
    if (FILE* f = std::fopen(opt.pipeline_list.c_str(), "rb")) {
      uint32_t head[2] = {};
      if (std::fread(head, 4, 2, f) == 2 && head[0] == kPipeListMagic && head[1] == sizeof(PipeRec)) {
        PipeRec r;
        while (std::fread(&r, sizeof(r), 1, f) == 1) recs.push_back(r);
      }
      std::fclose(f);
    }
    // Normalise lists written with undefined padding (PipeRec::reserved) and drop the duplicates that caused;
    // the file is rewritten once when it had any.
    const size_t read = recs.size();
    {
      std::vector<PipeRec> unique;
      unique.reserve(recs.size());
      for (PipeRec r : recs) {
        if (!VariantValid(r.reserved)) r.reserved = 0;
        if (listed_keys.insert(r.FullKey()).second) unique.push_back(r);
      }
      recs.swap(unique);
    }
    if (recs.size() != read && !opt.pipeline_list.empty()) {
      if (FILE* f = std::fopen(opt.pipeline_list.c_str(), "wb")) {
        const uint32_t head[2] = {kPipeListMagic, uint32_t(sizeof(PipeRec))};
        std::fwrite(head, 4, 2, f);
        if (!recs.empty()) std::fwrite(recs.data(), sizeof(PipeRec), recs.size(), f);
        std::fclose(f);
      }
      Log("renderer: pipeline list: %zu records, %zu after removing duplicates (undefined padding of older versions)", read, recs.size());
    }
    // Shipped list: the sample count follows the player's MSAA setting (variant word); keys go into listed_keys so
    // the local list only collects what the shipped one lacks.
    std::vector<ShippedRec> shipped;
    if (!opt.pipeline_shipped.empty()) {
      std::string error;
      const auto t0 = Clock::now();
      if (ReadShippedList(opt.pipeline_shipped, shipped, group_names, &error)) {
        for (ShippedRec& s : shipped) s.rec.pad = PadForVariant(s.rec.reserved);
        Log("renderer: shipped pipeline list %s: %zu records, %zu groups, checksum ok (%.1f ms)", opt.pipeline_shipped.c_str(),
            shipped.size(), group_names.size(), MsSince(t0));
      } else {
        shipped.clear();
        group_names.clear();
        Log("renderer: shipped pipeline list %s not used: %s", opt.pipeline_shipped.c_str(), error.c_str());
      }
    }
    // Order: the menu group (title, menus, videos, loading screens), then the player's own list, then the shipped
    // records by their first level in campaign order (the file is sorted that way). OnLevelLoading moves a level ahead.
    uint32_t menu_bit = 0;
    for (size_t g = 0; g < group_names.size(); ++g)
      if (group_names[g] == "menu") menu_bit = 1u << g;
    std::vector<ShippedRec> order;
    order.reserve(recs.size() + shipped.size());
    std::unordered_map<uint64_t, size_t> at;  // Key -> index in order
    auto add = [&](const ShippedRec& s) {
      auto [it, inserted] = at.emplace(s.rec.Key(), order.size());
      if (inserted) order.push_back(s);
      else order[it->second].groups |= s.groups;
    };
    for (const ShippedRec& s : shipped)
      if (s.groups & menu_bit) add(s);
    for (const PipeRec& r : recs) add(ShippedRec{r, 0, 0});
    for (const ShippedRec& s : shipped)
      if (!(s.groups & menu_bit)) add(s);
    for (const ShippedRec& s : shipped) listed_keys.insert(s.rec.FullKey());
    if (order.empty()) return;
    if (opt.pipeline_async) {
      // The pool's background queue (draws of the running list go first), filled by the feeder thread.
      prewarm_t0 = Clock::now();
      Log("renderer: prewarm: %zu pipelines (local list %zu, shipped %zu) on %u threads at %s priority", order.size(), recs.size(),
          shipped.size(), pool.threads(), opt.prewarm_low_priority ? "below normal" : "normal");
      {
        std::lock_guard lock(feed_mutex);
        feed.assign(order.begin(), order.end());
      }
      prewarm = std::thread([this] { Feeder(); });
      return;
    }
    recs.clear();
    for (const ShippedRec& s : order) recs.push_back(s.rec);
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
    if (window) {
      // [display] hdr: what the monitor reports (read only), then whether the swapchain becomes HDR10.
      display_hdr = QueryDisplayHdr(window);
      LogDisplayHdr("start");
      ho.hdr = WantHdr();
    }
    const auto t0 = Clock::now();
    host = CreateHostDevice(ho, error);
    if (!host) return false;
    if (host->has_swapchain()) {
      PacerOptions po;
      po.stats_seconds = opt.present_stats_seconds;
      po.target_fps = opt.pace_fps;
      po.queued = opt.pace_queued;
      po.hold = opt.pace_hold;
      po.early_ms = opt.pace_early_ms;
      po.spin_ms = opt.pace_spin_ms;
      po.trace = opt.pace_trace;
      po.single = opt.pace_single;
      pacer.Start(host.get(), po);
    }
    dev = host->device();
    api = dev->getGraphicsAPI();
    Log("renderer: device %s (%s), swapchain %s %ux%u, %.0f ms", host->description().c_str(), HostApiName(opt.api),
        host->has_swapchain() ? "yes" : "no", host->swapchain_width(), host->swapchain_height(), MsSince(t0));
    const bool shaped = host->has_swapchain() || opt.aspect > 0.0f;
    if (host->has_swapchain()) out_w = host->swapchain_width(), out_h = host->swapchain_height();
    cur_wide = wanted_wide = shaped ? WideFor(host->swapchain_width(), host->swapchain_height()) : 1.0f;
    cur_tall = wanted_tall = shaped ? TallFor(host->swapchain_width(), host->swapchain_height()) : 1.0f;
    cur_scale = wanted_scale = opt.scale_auto && host->has_swapchain() ? AutoScale(host->swapchain_height(), cur_tall) : opt.scale;
    FactorsFor(cur_scale, cur_wide, cur_tall, cur_sx, cur_sy);
    wide_published.store(cur_wide);
    tall_published.store(cur_tall);
    Log("renderer: Hor+ %s, Vert+ %s: aspect %s, factor %.4f / %.4f (screen targets 1280x720 guest -> %ux%u host, %s)%s",
        cur_wide != 1.0f ? "on" : "off", cur_tall != 1.0f ? "on" : opt.vert_plus ? "off" : "off (FCP_RENDER_VERTPLUS=0)",
        opt.aspect > 0.0f ? "fixed" : "auto (output)", cur_wide, cur_tall, HostSize(1280, cur_sx), HostSize(720, cur_sy),
        opt.resolution_mode == 1 ? "resolution native" : opt.resolution_mode == 2 ? "resolution fixed" : "from the scale",
        Shaped() ? "" : " - 16:9: bars as before");
    Log("renderer: HUD layout %s (hud_aspect %.4f), screen masks %s, split screen views %s", opt.hud_aspect <= 0.0f ? "auto (the picture's edges)"
        : std::fabs(opt.hud_aspect - 16.0f / 9.0f) < 1e-3f ? "16:9 (band, as before)" : "box", opt.hud_aspect,
        opt.screen_masks ? "on" : "off (FCP_RENDER_SCREEN_MASK=0)", opt.split_views ? "on" : "off (FCP_RENDER_SPLIT_VIEW=0)");
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
    Log("renderer: graphics: scale %s%.3f (screen targets from guest width %u), shadows %.3f, anisotropy %s, MSAA %s, SMAA %s, "
        "AO %s (resolution %s, strength %.2f%s)",
        opt.scale_auto ? "auto = " : "", cur_scale, opt.scale_min_width, opt.shadow_scale, opt.aniso > 1 ? an : "as the game",
        ms, opt.smaa ? "on" : "off", opt.ao == 2 ? "GTAO" : opt.ao ? "SSAO" : "off",
        opt.ao_resolution == 1 ? "full" : opt.ao_resolution == 2 ? "half" : "auto", opt.ao_strength,
        opt.ao_debug ? ", FCP_AO_DEBUG" : "");
    if (!InitObjects(error)) return false;
    if (host->has_swapchain()) SettleHdr(ho.hdr, "start");
    return true;
  }

  // --- HDR10 output ----------------------------------------------------------------------------------------
  bool WantHdr() const { return opt.hdr == 1 || (opt.hdr == 2 && display_hdr.enabled); }

  void LogDisplayHdr(const char* when) {
    const DisplayHdrInfo& d = display_hdr;
    Log("renderer: display HDR %s (%s: DXGI output %s, %u bits, peak %.0f nits, full frame %.0f, min %.4f, Windows SDR "
        "white %.0f nits); [display] hdr = %s",
        d.enabled ? "on" : "off", when, d.found ? d.device.c_str() : "not found", d.bits, d.max_nits, d.max_full_frame_nits,
        d.min_nits, d.sdr_white_nits, opt.hdr == 1 ? "on" : opt.hdr == 2 ? "auto" : "off");
  }

  void UpdateHdrLevels() {
    hdr_levels = HdrLevelsFor(opt.hdr_paper_white, opt.hdr_peak, opt.hdr_boost, display_hdr.sdr_white_nits, display_hdr.max_nits,
                              opt.hdr_gamma);
    hdr_levels.test_pattern = opt.hdr_test_pattern ? 1u : 0u;
  }

  // After the swapchain was made or switched: the pass for HDR10, else back to SDR; one log line either way.
  void SettleHdr(bool wanted, const char* when) {
    if (host->hdr_active() && !hdr_pass && !hdr_pass_failed) {
      auto pass = std::make_unique<HdrPass>();
      std::string e;
      if (pass->Init(dev, opt.api, &e)) hdr_pass = std::move(pass);
      else hdr_pass_failed = true, Log("renderer: HDR10 pass not available: %s", e.c_str());
    }
    if (host->hdr_active() && !hdr_pass) {
      back_fbs.clear();
      host->SetHdr(false);
    }
    hdr_on = host->hdr_active() && hdr_pass;
    UpdateHdrLevels();
    if (hdr_on) {
      Log("renderer: HDR10 output (%s): swapchain R10G10B10A2, PQ / BT.2020; paper white %.0f nits, peak %.0f nits, "
          "highlight gain x%.2f, gamma %.2f%s%s",
          when, hdr_levels.paper_white, hdr_levels.peak, hdr_levels.gain, hdr_levels.gamma,
          display_hdr.enabled ? "" : " - Windows HDR is off for this monitor: Windows shows the picture converted to SDR",
          hdr_levels.test_pattern ? ", FCP_HDR_TEST_PATTERN" : "");
    } else if (wanted) {
      Log("renderer: HDR10 not available (%s): %s; SDR as before", when,
          !host->hdr_note().empty() ? host->hdr_note().c_str() : hdr_pass_failed ? "no HDR10 pass" : "?");
    } else {
      Log("renderer: output SDR (%s)%s", when,
          opt.hdr == 2 ? ", [display] hdr = auto and Windows HDR off for this monitor" : "");
    }
  }

  bool InitObjects(std::string* error) {
    TableCapacities caps;
    // Lower bound: one list (at most kMaxDrawsPerList draws plus blits) must never use up all versions, otherwise a
    // draw would get the previous one's constants (NVRHI error "insufficient").
    if (opt.constant_versions) caps.constant_versions = std::max(opt.constant_versions, 2 * kMaxDrawsPerList + 64);
    for (size_t i = 0; i < caps.slots.size(); ++i)
      if (opt.table_slots[i]) caps.slots[i] = std::max(opt.table_slots[i], 16u);
    if (!tl.Init(dev, caps, error)) return false;
    slot_recycle = opt.slot_recycle;
    if (!slot_recycle) Log("renderer: FCP_RENDER_SLOT_RECYCLE=0: bindless slots are never reused (old behaviour)");
    alloc_limit_override = opt.alloc_limit;
    if (opt.test_allocs) {
      // Diagnostics: the allocation count of a long session from the start (one small buffer = one allocation).
      nvrhi::BufferDesc d;
      d.byteSize = 256;
      d.initialState = nvrhi::ResourceStates::ShaderResource;
      d.keepInitialState = true;
      d.debugName = "test allocation";
      uint32_t failed = 0;
      for (uint32_t i = 0; i < opt.test_allocs; ++i) {
        nvrhi::BufferHandle b = dev->createBuffer(d);
        if (b) test_allocs.push_back(b);
        else ++failed;
      }
      Log("renderer: FCP_RENDER_TEST_ALLOCS=%u: %zu extra allocations held, %u refused (device limit %u)", opt.test_allocs,
          test_allocs.size(), failed, host->max_allocations());
    }
    Log("renderer: device allocations: limit %u%s, guest textures %s, eviction at 3/4 of it %s", host->max_allocations(),
        host->max_allocations() ? "" : " (none)", host->pooled_textures() ? "pooled" : "one allocation each",
        !opt.alloc_evict ? "off (FCP_RENDER_ALLOC_EVICT=0)" : !slot_recycle ? "off (FCP_RENDER_SLOT_RECYCLE=0)" : "on");
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
    if (opt.pipeline_async) {
      const unsigned threads = opt.pipeline_threads ? opt.pipeline_threads : AutoPipelineThreads();
      pool.SetLowPriorityBackground(opt.prewarm_low_priority);
      pool.Start(threads);
      dlist = new DeferredCommandList(dev, [this] { ++async_materialize_early, Materialize(); });
      Log("renderer: pipelines on %u worker threads (FCP_RENDER_PIPELINE_ASYNC=0: on the render thread)", threads);
    }
    if (const char* tw = std::getenv("FCP_RENDER_TEXTURE_WORKERS"); !tw || std::string(tw) != "0")
      tex_workers = std::make_unique<res::TextureConversionWorkers>(std::min(3u, AutoPipelineThreads()));
    if (!opt.texture_dump_dir.empty() || !opt.texture_replace_dir.empty()) {
      pack::Options po;
      po.dump = !opt.texture_dump_dir.empty();
      po.replace = !opt.texture_replace_dir.empty();
      po.dump_dir = std::filesystem::path(std::u8string(opt.texture_dump_dir.begin(), opt.texture_dump_dir.end()));
      po.replace_dir = std::filesystem::path(std::u8string(opt.texture_replace_dir.begin(), opt.texture_replace_dir.end()));
      po.log = opt.log;
      pack_upload_budget = uint64_t(std::max<uint32_t>(opt.texture_upload_mb, 1)) << 20;
      tpack = std::make_unique<pack::TexturePack>(po);
      Log("renderer: texture pack: dump %s, replace %s, uploads up to %u MB per frame", po.dump ? "on" : "off",
          po.replace ? "on" : "off", unsigned(pack_upload_budget >> 20));
    }
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
      // Diagnostics FCP_RENDER_DEFER_ALL=1: every list goes through the deferred list (tests the replay).
      if (defer_all && dlist) dlist->Begin(cl), ++async_lists;
    }
    if (dlist && dlist->recording()) return dlist.Get();
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
    Materialize();
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
  // Write epochs (opt.watch_epoch, GitHub issue #4). The dirty bits above are cleared by the first entry that checks a
  // page; every other entry over the same page (the same texture under another fetch constant: 657 entries on 504 base
  // addresses in the reporter's log; neighbouring buffers sharing a 4 KB page) then misses the write and keeps stale
  // content until the next write. Instead each write gets a number (epoch) stored per page and per block of 64 pages;
  // an entry remembers the epoch it last read its pages at and is dirty if any page has a newer one. Nothing is
  // cleared, so every entry sees every write.
  // Order in a check: epoch snapshot, compare, re-watch, read, remember the snapshot. The SDK runs OnGuestWrite under
  // its watch lock before the faulting write goes ahead, so a write after the re-watch gets an epoch above the
  // snapshot, and one before it is in the data read.
  std::atomic<uint64_t> write_epoch{1};
  std::unique_ptr<std::atomic<uint64_t>[]> page_epoch{new std::atomic<uint64_t>[0x20000]()};
  std::unique_ptr<std::atomic<uint64_t>[]> block_epoch{new std::atomic<uint64_t>[0x20000 / 64]()};
  static void StoreMax(std::atomic<uint64_t>& a, uint64_t v) {
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (cur < v && !a.compare_exchange_weak(cur, v, std::memory_order_release, std::memory_order_relaxed)) {
    }
  }
  uint64_t EpochNow() const { return write_epoch.load(std::memory_order_acquire); }
  // True if a page of the range was written after epoch `seen`.
  bool WrittenSince(uint32_t physical, uint32_t bytes, uint64_t seen) const {
    if (!bytes) return false;
    const uint32_t p0 = (physical & 0x1FFFFFFFu) >> 12, p1 = std::min<uint64_t>((uint64_t(physical & 0x1FFFFFFFu) + bytes - 1) >> 12, 0x1FFFF);
    for (uint32_t b = p0 >> 6; b <= (p1 >> 6); ++b) {
      if (block_epoch[b].load(std::memory_order_acquire) <= seen) continue;
      const uint32_t first = std::max(p0, b << 6), last = std::min(p1, (b << 6) | 63);
      for (uint32_t p = first; p <= last; ++p)
        if (page_epoch[p].load(std::memory_order_acquire) > seen) return true;
    }
    return false;
  }
  // Old bookkeeping without clearing (verification of the old path).
  bool PeekDirtyIn(const std::atomic<uint64_t>* bits, uint32_t physical, uint32_t bytes) const {
    if (!bytes) return false;
    const uint32_t p0 = (physical & 0x1FFFFFFFu) >> 12, p1 = std::min<uint64_t>((uint64_t(physical & 0x1FFFFFFFu) + bytes - 1) >> 12, 0x1FFFF);
    for (uint32_t p = p0; p <= p1; ++p)
      if (bits[p >> 6].load(std::memory_order_relaxed) & (1ull << (p & 63))) return true;
    return false;
  }

  // Verification (opt.watch_verify): an entry the watch skipped whose content changed anyway. kind 0 texture,
  // 1 vertex buffer, 2 index buffer.
  uint64_t verify_checks[3] = {}, verify_missed[3] = {}, verify_races[3] = {}, verify_entries[3] = {};
  std::unordered_set<uint64_t> verify_seen;  // (kind, address, size) already logged
  // Safety net (opt.watch_rehash): changes found by hashing small entries the watch reported no write for.
  uint64_t rehash_caught[3] = {}, rehash_entries[3] = {};
  std::unordered_set<uint64_t> rehash_seen;
  uint32_t rehash_logged = 0;
  void RehashCaught(int kind, uint32_t physical, uint32_t bytes) {
    ++rehash_caught[kind];
    const uint64_t key = (uint64_t(kind) << 62) ^ (uint64_t(physical) << 30) ^ bytes;
    if (rehash_seen.size() < 65536 && rehash_seen.insert(key).second) {
      ++rehash_entries[kind];
      static const char* const names[3] = {"texture", "vertex buffer", "index buffer"};
      if (rehash_logged++ < 16)
        Log("renderer: write watch safety net: %s %08X+%X changed without a reported write (frame %llu), updated", names[kind],
            physical, bytes, (unsigned long long)frame);
    }
  }
  uint32_t verify_logged = 0, verify_dumps = 0;
  uint64_t verify_seen_epoch = 0;  // epoch of the entry being verified (log)
  bool verify_dump_now = false;  // dump this frame (first misses of new entries, FCP_DUMP_FRAMES set)
  void VerifyMiss(int kind, uint32_t physical, uint32_t bytes, const char* what) {
    ++verify_missed[kind];
    const uint64_t key = (uint64_t(kind) << 62) ^ (uint64_t(physical) << 30) ^ bytes;
    if (!verify_seen.insert(key).second) return;  // each entry logged and counted once
    ++verify_entries[kind];
    if (verify_dumps < 24) verify_dump_now = true, ++verify_dumps;  // frame dump (with FCP_DUMP_FRAMES)
    if (verify_logged++ < 64) {
      static const char* const names[3] = {"texture", "vertex buffer", "index buffer"};
      // Pages written around the range since the entry last read it (a write reported one page off?).
      const uint64_t seen_hint = verify_seen_epoch;
      const bool before = physical >= 0x1000 && WrittenSince(physical - 0x1000, 0x1000, seen_hint);
      const bool after = WrittenSince(((physical + bytes + 0xFFFu) & ~0xFFFu), 0x1000, seen_hint);
      // Host protection of the first page in the three physical views (A0/C0/E0): which view lets writes through?
      char prot[96] = "";
#ifdef _WIN32
      {
        const uint32_t p = physical & 0x1FFFF000u;
        const uint32_t views[3] = {0xA0000000u + p, 0xC0000000u + p, p >= 0x1000 ? 0xE0000000u + p - 0x1000u : 0};
        size_t at = 0;
        for (int v = 0; v < 3; ++v) {
          const uint8_t* host = views[v] ? memory.Virtual(views[v]) : nullptr;
          MEMORY_BASIC_INFORMATION mbi{};
          const bool ok = host && VirtualQuery(host, &mbi, sizeof(mbi)) == sizeof(mbi);
          at += size_t(std::snprintf(prot + at, sizeof(prot) - at, "%s%s=%lX", v ? " " : "", v == 0 ? "A0" : v == 1 ? "C0" : "E0",
                                     ok ? (unsigned long)mbi.Protect : 0ul));
        }
      }
#endif
      Log("renderer: write watch missed a change: %s %08X+%X %s (frame %llu, %s bookkeeping; page before written %s, "
          "page after %s; epoch now %llu, entry %llu; host protection %s)", names[kind], physical, bytes, what,
          (unsigned long long)frame, opt.watch_epoch ? "epoch" : "shared bits", before ? "yes" : "no", after ? "yes" : "no",
          (unsigned long long)EpochNow(), (unsigned long long)seen_hint, prot);
    }
  }

  // Resolve targets and CPU writes (GitHub issue #4). A resolve target's host texture stands for its guest memory
  // until the next resolve; on the console a CPU write there (the game putting another image into that memory)
  // replaces the content. Watched from every resolve on; a bind after a CPU write takes the guest data instead
  // (opt.resolve_cpu_writes), until the next resolve. Only pages wholly inside the range count (a neighbour sharing
  // the first or last page does not).
  uint64_t resolve_cpu_binds = 0, resolve_cpu_events = 0, front_cpu_swaps = 0, front_cpu_shown = 0;
  uint32_t resolve_cpu_logged = 0, front_cpu_logged = 0;
  void WatchResolved(Resolved& r) {
    if (!Watching() || !r.bytes) return;
    r.seen_epoch = EpochNow();
    watch_fn(watch_ctx, r.base, r.bytes);
    r.watched = true;
  }
  bool ResolvedWrittenByCpu(const Resolved& r) const {
    if (!r.watched || !r.bytes || !Watching()) return false;
    const uint32_t first = (r.base + 0xFFFu) & ~0xFFFu, end = (r.base + r.bytes) & ~0xFFFu;
    return end > first && WrittenSince(first, end - first, r.seen_epoch);
  }

  void MarkDirty(uint32_t physical, uint32_t bytes) {
    if (!bytes) return;
    const uint32_t p0 = (physical & 0x1FFFFFFFu) >> 12, p1 = std::min<uint64_t>((uint64_t(physical & 0x1FFFFFFFu) + bytes - 1) >> 12, 0x1FFFF);
    const uint64_t e = write_epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (uint32_t p = p0; p <= p1; ++p) {
      dirty_pages[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_relaxed);
      dirty_pages_idx[p >> 6].fetch_or(1ull << (p & 63), std::memory_order_relaxed);
      StoreMax(page_epoch[p], e);
    }
    for (uint32_t b = p0 >> 6; b <= (p1 >> 6); ++b) StoreMax(block_epoch[b], e);
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

  // Pooled texture memory (FCP_RENDER_VK_TEXTURE_POOL) is not zeroed like a dedicated allocation: subresources the
  // conversion leaves unwritten get zeros (as in King Kong's ZeroUnwrittenSubresources, task 304).
  void ZeroUnwritten(nvrhi::ICommandList* c, nvrhi::ITexture* tex, const res::HostTextureData& data) {
    const nvrhi::TextureDesc& d = tex->getDesc();
    const bool volume = d.dimension == nvrhi::TextureDimension::Texture3D;
    const uint32_t layers = volume ? 1u : std::max(d.arraySize, 1u);
    if (data.subresources.size() >= size_t(d.mipLevels) * layers) return;
    const nvrhi::FormatInfo& f = nvrhi::getFormatInfo(d.format);
    const uint32_t block = std::max<uint32_t>(f.blockSize, 1);
    std::vector<uint8_t> zeros;
    for (uint32_t layer = 0; layer < layers; ++layer)
      for (uint32_t level = 0; level < d.mipLevels; ++level) {
        bool written = false;
        for (const res::HostSubresource& s : data.subresources) written = written || (s.level == level && s.layer == layer);
        if (written) continue;
        const uint32_t w = std::max(d.width >> level, 1u), h = std::max(d.height >> level, 1u);
        const uint32_t depth = volume ? std::max(d.depth >> level, 1u) : 1u;
        const size_t row = size_t((w + block - 1) / block) * f.bytesPerBlock;
        const size_t slice = row * ((h + block - 1) / block);
        if (zeros.size() < slice * depth) zeros.resize(slice * depth, 0);
        c->writeTexture(tex, layer, level, zeros.data(), row, slice);
        ++zeroed_subresources;
      }
  }
  uint64_t zeroed_subresources = 0;

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
      d.allowPlacedMemory = true;  // Vulkan: a range of a pooled block (host_vulkan, FCP_RENDER_VK_TEXTURE_POOL)
      nvrhi::TextureHandle nt = SimulatedRefusal() ? nullptr : dev->createTexture(d);
      if (!nt) return alloc_failed = true, false;
      if (t.tex) retired.emplace_back(frame, t.tex);
      const auto mapping = res::ToNvrhiMapping(p.view_swizzle);
      if (rewrite && t.slot && t.table == table) {
        tl.WriteTextureView(table, t.slot & 0x7FFFFFFFu, nt, mapping, d.dimension);
      } else {
        const uint32_t slot = tl.AddTextureView(table, nt, mapping, d.dimension);
        if (!slot) return table_full = true, false;
        // A rewrite into another table (new dimension): the old slot is no longer referenced by this entry.
        if (t.slot && slot_recycle) ReleaseSlot(t.table, t.slot & 0x7FFFFFFFu);
        t.slot = slot | (stacked ? 0x80000000u : 0u);
      }
      t.tex = nt;
      t.table = table;
    }
    for (const res::HostSubresource& s : data.subresources)
      c->writeTexture(t.tex, s.layer, s.level, data.bytes.data() + s.offset, s.row_pitch, s.depth_pitch);
    if (!same && host && host->pooled_textures()) ZeroUnwritten(c, t.tex, data);
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
    const std::shared_ptr<const ReplacementImage> img = replace_source->Current(t.replace_kind, HudScale());
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

  // --- Texture packs (core/texture_pack.h) ------------------------------------------------------------------
  // After every conversion (new or rewritten texture): the name, a dump in kDumpSettle frames, a new look-up of the
  // replacement. Textures the game keeps rewriting (video) take no part.
  void PackConverted(GuestTex& t, const res::TextureFetch& f, const res::HostTextureData& data) {
    const auto t0 = Clock::now();
    DropReplacement(t);
    t.pack_key = 0, t.pack_kind = pack::Kind::kNone, t.dump_at = 0;
    t.rep_rev = 0, t.rep_gen = 0, t.rep_failed = false;
    if (t.volatile_data) {
      ++pack_volatile;
      return;
    }
    const char* why = "";
    const pack::Kind kind = pack::Classify(data.plan, &why);
    if (kind == pack::Kind::kNone) {
      ++pack_skipped;
      Once(std::string("packskip") + why, "renderer: texture pack: textures not dumped or replaced: %s", why);
      return;
    }
    t.pack_kind = kind;
    t.pack_key = pack::Key(f, phys, data, kind);
    if (t.pack_key) ++pack_keys;
    if (t.pack_key && tpack->Dumping() && tpack->WantsDump(t.pack_key)) t.dump_at = frame + kDumpSettle;
    pack_key_us += uint64_t(MsSince(t0) * 1000);
  }

  // The texture's replacement view (and its share of the uploaded texture) goes.
  void DropReplacement(GuestTex& t) {
    if (!t.rep_slot) return;
    ReleaseSlot(t.rep_table, t.rep_slot);
    t.rep_slot = 0;
    auto it = pack_tex.find(PackTexKey{t.pack_key, t.rep_gen});
    if (it != pack_tex.end() && it->second.views && --it->second.views == 0) {
      if (it->second.tex) retired.emplace_back(frame, it->second.tex);
      pack_tex.erase(it);
    }
  }

  // Dump due: converted again from guest memory (unchanged since kDumpSettle frames) and handed to the pack's worker.
  // At most 32 MB of conversions per frame; the rest follows in the next frames.
  void PackDump(GuestTex& t, const uint32_t words[6]) {
    if (pack_dump_frame != frame) pack_dump_frame = frame, pack_dump_bytes = 0;
    if (pack_dump_bytes >= (32u << 20)) return;
    const auto t0 = Clock::now();
    t.dump_at = 0;
    if (!tpack->WantsDump(t.pack_key)) return;
    const res::TextureFetch f = res::TextureFetch::FromWords(words);
    res::HostTextureData data, file;
    if (!res::ConvertTexture(f, phys, data, nullptr, res::TextureOptions(), tex_workers.get()) ||
        pack::Key(f, phys, data, t.pack_kind) != t.pack_key || !pack::DumpData(data, t.pack_kind, file))
      return;
    pack_dump_bytes += data.bytes.size();
    const std::string name = pack::DumpName(t.pack_key, f, data.plan, t.pack_kind);
    const std::string info = pack::DumpInfo(t.pack_key, f, data.plan, t.pack_kind);
    if (tpack->Dump(t.pack_key, name, info, std::move(file))) {
      ++pack_dumps;
      if (pack_logged_dumps++ < 20) Log("renderer: texture pack: dump %s", name.c_str());
    } else if (tpack->WantsDump(t.pack_key)) {
      t.dump_at = frame + 30;  // the worker's queue is full
    }
    pack_dump_us += uint64_t(MsSince(t0) * 1000);
  }

  // Binding of a texture with a pack name: dump when due; the replacement's view once it is loaded and uploaded, else
  // `slot` (the game's texture). Uploads are limited to pack_upload_budget bytes per frame and may take several frames.
  uint32_t PackSlot(GuestTex& t, const uint32_t words[6], uint32_t slot, nvrhi::ICommandList* c) {
    if (t.dump_at && frame >= t.dump_at) PackDump(t, words);
    if (!tpack->Replacing()) return slot;
    const uint32_t rev = tpack->Revision();
    if (t.rep_rev != rev) {
      t.rep_rev = rev;
      const uint32_t gen = tpack->FileGen(t.pack_key);
      if (gen != t.rep_gen) {
        DropReplacement(t);
        t.rep_gen = gen;
        t.rep_failed = false;
      }
    }
    if (!t.rep_gen || t.rep_failed) return slot;
    if (!t.rep_slot && !PackPrepare(t, c)) return slot;
    tex_inv_x = t.rep_inv_x, tex_inv_y = t.rep_inv_y;
    ++pack_binds;
    return t.rep_slot;
  }

  bool PackPrepare(GuestTex& t, nvrhi::ICommandList* c) {
    PackTex& r = pack_tex[PackTexKey{t.pack_key, t.rep_gen}];
    r.used_frame = frame;
    const bool cube = t.plan.dimension == res::Dimension::kCube;
    if (!r.tex) {
      if (!r.data) {
        std::shared_ptr<const res::HostTextureData> d;
        switch (tpack->Find(t.pack_key, t.rep_gen, d)) {
          case pack::TexturePack::Lookup::kNone: t.rep_failed = true; return false;
          case pack::TexturePack::Lookup::kLoading: return false;
          case pack::TexturePack::Lookup::kReady: r.data = std::move(d); break;
        }
      }
      const res::HostTexturePlan& p = r.data->plan;
      if ((p.dimension == res::Dimension::kCube) != cube) {
        char key[24];
        std::snprintf(key, sizeof key, "%016llx", (unsigned long long)t.pack_key);
        Once(std::string("packshape") + key,
             cube ? "renderer: texture pack: %s is a cube map, the replacement is not (a cube DDS is needed)"
                  : "renderer: texture pack: %s is a 2D texture, the replacement is a cube map",
             key);
        t.rep_failed = true;
        r.data.reset();
        return false;
      }
      nvrhi::TextureDesc d;
      d.width = p.width;
      d.height = p.height;
      d.mipLevels = p.levels;
      d.format = res::ToNvrhi(p.format);
      d.dimension = cube ? nvrhi::TextureDimension::TextureCube : nvrhi::TextureDimension::Texture2D;
      d.arraySize = cube ? 6 : 1;
      d.initialState = nvrhi::ResourceStates::ShaderResource;
      d.keepInitialState = true;
      d.debugName = "texture pack replacement";
      r.tex = dev->createTexture(d);
      if (!r.tex) {
        t.rep_failed = true;
        r.data.reset();
        return false;
      }
      r.width = p.width, r.height = p.height, r.cube = cube;
    }
    if (!r.done) {
      if (pack_budget_frame != frame) pack_budget_frame = frame, pack_budget_used = 0;
      const auto t0 = Clock::now();
      const std::vector<res::HostSubresource>& subs = r.data->subresources;
      while (r.next < subs.size()) {
        const res::HostSubresource& s = subs[r.next];
        if (pack_budget_used && pack_budget_used + s.size > pack_upload_budget) break;
        c->writeTexture(r.tex, s.layer, s.level, r.data->bytes.data() + s.offset, s.row_pitch, s.depth_pitch);
        pack_budget_used += s.size, pack_upload_bytes += s.size;
        ++r.next;
      }
      pack_upload_us += uint64_t(MsSince(t0) * 1000);
      if (r.next < subs.size()) {
        ++pack_frames_upload_capped;
        return false;
      }
      c->setTextureState(r.tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
      r.done = true;
      r.data.reset();
      ++pack_uploads;
    }
    const Table table = cube ? Table::kTexCube : Table::kTex2D;
    const uint16_t swz = pack::ReplacementSwizzle(t.pack_kind, t.plan.view_swizzle);
    const uint32_t s = tl.AddTextureView(table, r.tex, res::ToNvrhiMapping(swz),
                                         cube ? nvrhi::TextureDimension::TextureCube : nvrhi::TextureDimension::Texture2D);
    if (!s) {
      Once("packfull", "renderer: texture pack: texture table full (%s), the game's texture is drawn meanwhile",
           slot_recycle ? "evicting unused textures" : "slot recycling off");
      if (slot_recycle) evict_now = true;
      return false;
    }
    t.rep_slot = s;
    t.rep_table = table;
    ++r.views;
    t.rep_inv_x = float(t.plan.width) / float(r.width);
    t.rep_inv_y = float(t.plan.height) / float(r.height);
    if (pack_logged_binds++ < 40)
      Log("renderer: texture pack: %016llx drawn with its replacement (%ux%u for %ux%u%s)", (unsigned long long)t.pack_key,
          r.width, r.height, t.plan.width, t.plan.height, cube ? ", cube" : "");
    return true;
  }

  // At a swap: the folder watch, and uploaded replacements no view uses any more (half uploaded ones of evicted
  // textures) after kEvictAge frames.
  void PackFrame() {
    tpack->Frame();
    if (frame % 120) return;
    for (auto it = pack_tex.begin(); it != pack_tex.end();) {
      if (!it->second.views && it->second.used_frame + kEvictAge < frame) {
        if (it->second.tex) retired.emplace_back(frame, it->second.tex);
        it = pack_tex.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::string PackStats() const {
    char line[512];
    std::snprintf(line, sizeof line,
                  "%llu textures named (%.1f ms), %llu not taking part, %llu rewritten constantly; %llu dumps handed over "
                  "(%.1f ms); %llu replacements uploaded (%llu MB, %.1f ms, %llu times over the frame budget), %llu "
                  "bindings replaced, %zu replacement textures",
                  (unsigned long long)pack_keys, pack_key_us / 1000.0, (unsigned long long)pack_skipped,
                  (unsigned long long)pack_volatile, (unsigned long long)pack_dumps, pack_dump_us / 1000.0,
                  (unsigned long long)pack_uploads, (unsigned long long)(pack_upload_bytes >> 20), pack_upload_us / 1000.0,
                  (unsigned long long)pack_frames_upload_capped, (unsigned long long)pack_binds, pack_tex.size());
    return line;
  }

  uint32_t Texture(const uint32_t words[6], nvrhi::ICommandList* c) {
    GuestTex* g = nullptr;
    const uint32_t slot = TextureOf(words, c, g);
    if (g && g->replace_kind >= 0 && replace_source && slot) {
      // button glyphs first; where the prompt module keeps the game's page (Xbox style), a texture pack may replace it
      const uint32_t glyphs = ReplacedSlot(*g, slot, c);
      if (glyphs != slot) return glyphs;
    }
    if (g && g->pack_key && slot) return PackSlot(*g, words, slot, c);
    return slot;
  }

  uint32_t TextureOf(const uint32_t words[6], nvrhi::ICommandList* c, GuestTex*& out) {
    const res::TextureFetch f = res::TextureFetch::FromWords(words);
    tex_inv_x = tex_inv_y = 1.0f;
    tex_depth = false;
    if (f.Type() != 2) return 0;
    auto rit = f.BaseAddress() ? resolved.find(f.BaseAddress()) : resolved.end();
    if (rit != resolved.end() && ResolvedWrittenByCpu(rit->second)) {
      // The CPU wrote into the target's memory after its last resolve: the guest data is the newer content.
      Resolved& r = rit->second;
      ++resolve_cpu_binds;
      if (r.cpu_written_frame == ~0ull || r.cpu_written_frame + 1 < frame) {
        ++resolve_cpu_events;
        if (resolve_cpu_logged++ < 16)
          Log("renderer: resolve target base %08X %ux%u written by the CPU after its resolve (frame %llu, now %llu): fetch "
              "%ux%u format %u %s", r.base, r.gw, r.gh, (unsigned long long)r.frame, (unsigned long long)frame, f.Width(),
              f.Height(), unsigned(f.Format()), opt.resolve_cpu_writes ? "drawn from guest memory" : "resolve target kept (FCP_RENDER_RESOLVE_CPU_WRITES=1: guest memory)");
      }
      r.cpu_written_frame = frame;
      if (opt.resolve_cpu_writes) rit = resolved.end();
    }
    if (rit != resolved.end()) {
      Resolved& r = rit->second;
      if (f.Width() != r.gw || f.Height() != r.gh) {
        // Diagnostics (issue #1): a fetch at the base of a resolve target with another size, i.e. probably a guest
        // texture that now lives where a resolve target once was.
        ++stale_resolve_binds;
        if (stale_resolve_logged++ < 16)
          Log("renderer: fetch at resolve target base %08X with another size: fetch %ux%u format %u, target %ux%u "
              "(resolved in frame %llu, now frame %llu)", f.BaseAddress(), f.Width(), f.Height(), unsigned(f.Format()),
              r.gw, r.gh, (unsigned long long)r.frame, (unsigned long long)frame);
      }
      tex_inv_x = 1.0f / r.sx, tex_inv_y = 1.0f / r.sy;  // guest texels per host texel (kk_GuestSize in the shader)
      ++r.interval_binds;
      tex_depth = r.depth;
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
    t.used_frame = frame;
    if (t.failed) {
      // Failed only because the table was full (slot recycling): convert again once slots may be free.
      if (!t.retry_frame || frame < t.retry_frame) return 0;
      textures.erase(it);
      return TextureOf(words, c, out);
    }
    out = &t;
    if (!inserted && (t.checked_frame == frame || !opt.texture_check)) return t.slot;
    const auto t0 = Clock::now();
    if (inserted) t.ranges = res::GetTextureRanges(f);
    const bool watch = Watching() && !t.volatile_data;
    if (watch) {
      // Watched: only hash when the SDK has reported a write to a page. Clear the bits (or take the epoch) and
      // re-watch first, then read: a write after that falls into the next check.
      const uint64_t epoch = EpochNow();
      bool dirty = opt.watch_epoch ? WrittenSince(t.ranges.base, t.ranges.base_bytes, t.seen_epoch) ||
                                         WrittenSince(t.ranges.mip, t.ranges.mip_bytes, t.seen_epoch)
                                   : TakeDirty(t.ranges.base, t.ranges.base_bytes) | TakeDirty(t.ranges.mip, t.ranges.mip_bytes);
      if (!inserted && t.watched && !dirty && uint64_t(t.ranges.base_bytes) + t.ranges.mip_bytes <= opt.watch_rehash &&
          HashRanges(t.ranges) != t.hash) {
        dirty = true;  // safety net: changed without a reported write
        RehashCaught(0, t.ranges.base, t.ranges.base_bytes);
      }
      if (!inserted && t.watched && !dirty) {
        if (opt.watch_verify) {
          ++verify_checks[0];
          if (HashRanges(t.ranges) != t.hash) {
            const bool late = opt.watch_epoch ? WrittenSince(t.ranges.base, t.ranges.base_bytes, t.seen_epoch) ||
                                                    WrittenSince(t.ranges.mip, t.ranges.mip_bytes, t.seen_epoch)
                                              : PeekDirtyIn(dirty_pages.get(), t.ranges.base, t.ranges.base_bytes) ||
                                                    PeekDirtyIn(dirty_pages.get(), t.ranges.mip, t.ranges.mip_bytes);
            char what[96];
            std::snprintf(what, sizeof(what), "(mip %08X+%X, format %u %ux%u)", t.ranges.mip, t.ranges.mip_bytes,
                          unsigned(f.Format()), f.Width(), f.Height());
            if (late) ++verify_races[0];  // a write reported meanwhile: the next check takes it
            else verify_seen_epoch = t.seen_epoch, VerifyMiss(0, t.ranges.base, t.ranges.base_bytes, what);
          }
        }
        t.checked_frame = frame;
        ++watch_skips;
        tex_check_us += uint64_t(MsSince(t0) * 1000);
        return t.slot;
      }
      if (t.ranges.base_bytes) watch_fn(watch_ctx, t.ranges.base, t.ranges.base_bytes);
      if (t.ranges.mip_bytes) watch_fn(watch_ctx, t.ranges.mip, t.ranges.mip_bytes);
      t.watched = true;
      t.seen_epoch = epoch;
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
    table_full = false;
    alloc_failed = false;
    if (!res::ConvertTexture(f, phys, data, &why, res::TextureOptions(), tex_workers.get()) || !CreateTexture(t, data, c, !inserted)) {
      if (alloc_failed) {
        // The device refused the texture (allocation limit or out of memory): logged, unused resources evicted at the
        // next swap, the texture tried again after a second (FCP_RENDER_ALLOC_EVICT=0: given up as before).
        char detail[96];
        std::snprintf(detail, sizeof(detail), "%ux%u, %u levels, format %u", data.plan.width, data.plan.height,
                      data.plan.levels, unsigned(f.Format()));
        AllocFailureLog("texture", ++alloc_fail_tex, detail);
        if (opt.alloc_evict && slot_recycle) {
          alloc_pressure = true;
          if (inserted) t.failed = true, t.retry_frame = frame + 60;
          return 0;
        }
      }
      if (table_full) {
        // No free slot: logged once; with slot recycling the next eviction pass frees slots, try again then.
        Once("tablefull", "renderer: texture table full (%s), textures without a slot are drawn with the black fallback",
             slot_recycle ? "evicting unused textures" : "slot recycling off");
        if (slot_recycle) {
          evict_now = true;
          if (inserted) t.failed = true, t.retry_frame = frame + 30;
        } else if (inserted) {
          t.failed = true;
        }
        return 0;
      }
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
    if (tpack) PackConverted(t, f, data);
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
    if (frame < buf_retry_frame && !buffers.count(key)) return 0;  // a creation failed just now (see below)
    auto [it, inserted] = buffers.try_emplace(key);
    GuestBuf& b = it->second;
    b.used_frame = frame;
    if (!inserted && !b.slot && b.buf && slot_recycle) {
      // Created while the table was full: try for a slot again (content is written below as for a change).
      if ((b.slot = tl.AddBuffer(b.buf)) != 0) b.hash = 0, b.checked_frame = ~0ull, b.watched = false;
    }
    if (!inserted && b.checked_frame == frame) return b.slot;
    const auto t0 = Clock::now();
    const bool watch = Watching() && opt.buffer_watch && !b.volatile_data;
    if (watch) {
      const uint64_t epoch = EpochNow();
      bool dirty = opt.watch_epoch ? WrittenSince(physical, bytes, b.seen_epoch) : TakeDirty(physical, bytes);
      if (!inserted && b.watched && !dirty && bytes <= opt.watch_rehash && fcshaders::containerHash(data, bytes) != b.hash) {
        dirty = true;  // safety net: changed without a reported write
        RehashCaught(1, physical, bytes);
      }
      if (!inserted && b.watched && !dirty) {
        if (opt.watch_verify) {
          ++verify_checks[1];
          if (fcshaders::containerHash(data, bytes) != b.hash) {
            const bool late = opt.watch_epoch ? WrittenSince(physical, bytes, b.seen_epoch)
                                              : PeekDirtyIn(dirty_pages.get(), physical, bytes);
            if (late) ++verify_races[1];
            else verify_seen_epoch = b.seen_epoch, VerifyMiss(1, physical, bytes, "");
          }
        }
        b.checked_frame = frame;
        buffer_us += uint64_t(MsSince(t0) * 1000);
        return b.slot;
      }
      watch_fn(watch_ctx, physical, bytes);
      b.watched = true;
      b.seen_epoch = epoch;
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
      b.buf = SimulatedRefusal() ? nullptr : dev->createBuffer(d);
      if (!b.buf) {
        // Refused by the device (GitHub issue #1): logged, the entry dropped so the next use tries again (it used to stay
        // without a buffer and the stream read zeros).
        char detail[48];
        std::snprintf(detail, sizeof(detail), "%u bytes", bytes);
        AllocFailureLog("vertex buffer", ++alloc_fail_buf, detail);
        buffers.erase(it);
        buf_retry_frame = frame + 30;  // no new buffer creations for half a second (one failed call per draw costs)
        if (opt.alloc_evict && slot_recycle) alloc_pressure = true;
        return 0;
      }
      b.slot = tl.AddBuffer(b.buf);
      if (!b.slot) {
        Once("buffull", "renderer: vertex buffer table full (%s), draws without a slot lose that vertex stream",
             slot_recycle ? "evicting unused buffers" : "slot recycling off");
        if (slot_recycle) evict_now = true;
        return 0;
      }
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
    uint64_t used_frame = 0;  // last frame that drew with it (eviction under allocation pressure)
    uint64_t seen_epoch = 0;  // write epoch the indices were last read at (opt.watch_epoch)
    uint8_t head[16] = {};    // first bytes at the last conversion (FCP_RENDER_WATCH_VERIFY log)
    uint64_t converted_frame = 0;
    uint64_t rehash_frame = ~0ull;  // frame of the last safety-net hash (opt.watch_rehash)
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
    e.used_frame = frame;
    if (e.volatile_data) return nullptr;
    const uint64_t epoch = EpochNow();
    bool dirty = opt.watch_epoch ? WrittenSince(ib.physical, n * isize, e.seen_epoch)
                                 : TakeDirtyIn(dirty_pages_idx.get(), ib.physical, n * isize);
    if (!inserted && e.watched && !dirty && e.rehash_frame != frame && n * isize <= opt.watch_rehash) {
      // Safety net, once per frame (the watch alone is checked at every draw): changed without a reported write?
      e.rehash_frame = frame;
      if (fcshaders::containerHash(data, size_t(n) * isize) != e.hash) {
        dirty = true;
        RehashCaught(2, ib.physical, n * isize);
      }
    }
    if (!inserted && e.watched && !dirty) {
      if (opt.watch_verify) {
        ++verify_checks[2];
        if (fcshaders::containerHash(data, size_t(n) * isize) != e.hash) {
          const bool late = opt.watch_epoch ? WrittenSince(ib.physical, n * isize, e.seen_epoch)
                                            : PeekDirtyIn(dirty_pages_idx.get(), ib.physical, n * isize);
          if (late) ++verify_races[2];
          else {
            // First bytes then (last conversion) and now, and the IB object's address: what changed?
            char what[200];
            int at = std::snprintf(what, sizeof(what), "(%s; converted in frame %llu: ", ib.index32 ? "32 bit" : "16 bit",
                                   (unsigned long long)e.converted_frame);
            for (int i = 0; i < 16 && at < 150; ++i) at += std::snprintf(what + at, sizeof(what) - at, "%02X", e.head[i]);
            at += std::snprintf(what + at, sizeof(what) - at, ", now ");
            for (int i = 0; i < 16 && at < 190; ++i) at += std::snprintf(what + at, sizeof(what) - at, "%02X", data[i]);
            std::snprintf(what + at, sizeof(what) - at, ")");
            verify_seen_epoch = e.seen_epoch;
            VerifyMiss(2, ib.physical, n * isize, what);
          }
        }
      }
      return ++idx_hits, e.buf.Get();
    }
    watch_fn(watch_ctx, ib.physical, n * isize);
    e.watched = true;
    e.seen_epoch = epoch;
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
      e.buf = SimulatedRefusal() ? nullptr : dev->createBuffer(d);
      if (!e.buf) {
        // Refused by the device: the ring path draws it (as before); counted and logged (GitHub issue #1).
        char detail[48];
        std::snprintf(detail, sizeof(detail), "%llu bytes", (unsigned long long)d.byteSize);
        AllocFailureLog("index buffer", ++alloc_fail_idx, detail);
        if (opt.alloc_evict && slot_recycle) alloc_pressure = true;
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
    std::memcpy(e.head, data, std::min<size_t>(16, size_t(n) * isize));
    e.converted_frame = frame;
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
    for (int i = 0; i < 4; ++i)
      if (colors[i]) key[i] = colors[i]->tex, ++colors[i]->writes;
    if (depth) key[4] = depth->tex, ++depth->writes;
    if (auto it = framebuffers.find(key); it != framebuffers.end()) return it->second;
    nvrhi::FramebufferDesc d;
    for (int i = 0; i < 4; ++i)
      if (colors[i]) d.addColorAttachment(colors[i]->tex);
    if (depth) d.setDepthAttachment(depth->tex);
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
      if (!frame_3d) frame_3d = true, view_surface = color->surface, view_gw = color->gw, view_gh = color->gh;
      return false;
    }
    // (Not into the view's own target: after a resolve in the middle of the scene more world draws without depth test
    // go there, measured VS 7AFF2B5C in 21_dunea; the HUD goes into its own target 1280x720.)
    // Split screen: the views are 640 wide (or 360 high), the HUD of all of them goes into the screen-sized target at the
    // end of the frame (measured): that one counts as the view's width too.
    const bool split_hud = opt.split_views && IsSplitTarget(view_gw, view_gh) && color->gw >= 2 * 640 && color->gw > color->gh;
    if (!frame_3d || !post_3d || color->surface == view_surface || (color->gw != view_gw && !split_hud) || !(color_mask & 0xF))
      return false;
    float box[4];
    const bool known = DrawBox(r, d, box);
    // Window offset (PA_SC_WINDOW_OFFSET with PA_SU_SC_MODE_CNTL bit 16): the positions are the screen's, the target
    // holds only a part of it (measured in the split screen with 4 players: the views on the right draw their post
    // processing quad at x 639.5..1279.5 into their own 640x360 target with offset -640). Into the target's pixels, so
    // such a quad counts as covering its target. Not for quads in clip space (-1..1).
    if (known && opt.split_views && (d.u32(F::pa_su_sc_mode_cntl) & (1u << 16))) {
      const uint32_t wo = d.u32(F::pa_sc_window_offset);
      const float wx = float(int32_t(wo << 17) >> 17), wy = float(int32_t((wo >> 16) << 17) >> 17);
      const bool clip_space = box[0] >= -1.01f && box[2] <= 1.01f && box[1] >= -1.01f && box[3] <= 1.01f;
      if ((wx != 0.0f || wy != 0.0f) && !clip_space) {
        box[0] += wx, box[2] += wx, box[1] += wy, box[3] += wy;
        ++stat_window_offset;
      }
    }
    bool full = false, mask = false;
    const float w = float(color->gw), h = float(color->gh);
    // Split screen: the view the draw lies in (its box's centre); its rectangle takes the screen's place for the
    // decisions below (whole view, mask over the view, anchors at the view's edges). Else the whole target.
    float vr[4] = {0.0f, 0.0f, w, h};
    // (only boxes in pixels: a quad in clip space -1..1 has its centre at the screen's corner)
    const bool pixels = known && (box[2] - box[0] > 2.5f || box[3] - box[1] > 2.5f || box[0] > 1.5f || box[1] > 1.5f);
    int view = pixels && opt.split_views && ViewOf(box, vr) ? 1 : -1;
    // A screen mask of a view (binoculars of one player, measured 2 players at 21:9: frame 156 vertices about 593..1297
    // for the right view) reaches beyond its view: by its centre, if it is one relative to that view (several quads
    // across the view's width over 0.7 of its height, or over its height and half its width).
    if (view < 0 && pixels && opt.split_views && opt.screen_masks && r.count > 6) {
      float lr[4];
      if (ViewOf(box, lr, false)) {
        const bool la = box[0] <= lr[0] + 3.0f && box[2] >= lr[2] - 3.0f, lt = box[1] <= lr[1] + 3.0f && box[3] >= lr[3] - 3.0f;
        if ((la && box[3] - box[1] >= 0.7f * (lr[3] - lr[1])) || (lt && box[2] - box[0] >= 0.5f * (lr[2] - lr[0])))
          std::copy(lr, lr + 4, vr), view = 1;
      }
    }
    if (known) {
      const bool single = r.count <= 6;
      const bool clip = single && box[0] <= -0.99f && box[2] >= 0.99f && box[2] - box[0] <= 2.5f && box[1] <= -0.99f &&
                        box[3] >= 0.99f && box[3] - box[1] <= 2.5f;
      bool across = box[0] <= 1.0f && box[2] >= w - 1.0f;
      bool tall = box[1] <= 1.0f && box[3] >= h - 1.0f;
      // Split screen: every view renders into its own target (640x720, 640x360/368, measured), and a view on the right
      // or at the bottom draws its full-view quads (post processing) at its place on the screen, e.g. x 639.5..1279.5
      // into its 640 wide target. A single quad of the view's size covers its target wherever its positions lie.
      if (opt.split_views && single && !clip && IsSplitTarget(color->gw, color->gh)) {
        const float view_h = float(SplitViewHeight(color->gh));
        if (box[2] - box[0] >= w - 1.5f) across = true;
        if (box[3] - box[1] >= view_h - 1.5f) tall = true;
        if ((across || tall) && !(box[0] <= 1.0f && box[2] >= w - 1.0f && box[1] <= 1.0f)) ++stat_window_offset;
      }
      const float vw = vr[2] - vr[0], vh = vr[3] - vr[1];
      const bool v_across = box[0] <= vr[0] + 1.0f && box[2] >= vr[2] - 1.0f;
      const bool v_tall = box[1] <= vr[1] + 1.0f && box[3] >= vr[3] - 1.0f;
      full = clip || (across && (single || tall)) || (view >= 0 && v_across && (single || v_tall));
      // Screen mask (binoculars, scopes; measured in 02_native, HUD shader, alpha blend): several quads across the
      // width over at least 0.7 of the height (scope side bars 12 vertices, binocular frame 156, both while coming up),
      // or over the whole height and at least half the width (the scope's circle, 24 vertices, 768x768 about the
      // centre). Not after the HUD covered the screen (the pause menu's panel goes across the width, 0.67 high).
      // Split screen: the same within the view.
      if (opt.screen_masks && !single && !hud_cover &&
          ((v_across && box[3] - box[1] >= 0.7f * vh) || (v_tall && box[2] - box[0] >= 0.5f * vw)))
        mask = true, full = false;
      // The HUD covers the screen (a single quad across it with the band's vertex shader, after the band began): the
      // pause menu's dimming, fades. From here on this frame stays in the band (menus over the game).
      if (across && tall && !clip && single && (vs->container_hash == hud_vs_now || vs->container_hash == hud_vs_last))
        hud_cover = true;
    } else {
      ++unknown_boxes;
    }
    const bool narrow = !full;
    ++(narrow ? narrow_draws : full_draws);
    hud.known = known, hud.mask = mask, hud.ax = hud.ay = 0;
    hud.view = narrow ? view : -1;
    std::copy(vr, vr + 4, hud.vr);
    if (known) std::copy(box, box + 4, hud.box);
    if (narrow) hud_vs_now = vs->container_hash;
    if (narrow && view >= 0) ++(mask ? stat_view_masks : stat_view_hud);
    if (mask) {
      ++stat_masks;
      if (n_masks_now < 8) masks_now[n_masks_now++] = {{box[0], box[1], box[2], box[3]}, r.count, view >= 0 ? ViewKey(vr) : -1};
    } else if (narrow && known) {
      HudAnchor(box, vr, hud.ax, hud.ay);
    }
    if (opt.trace_wide && wide_traced.size() < 4000) {
      const uint64_t key = Mix(Mix(vs->container_hash, ps ? ps->container_hash : 0),
                               (known ? uint64_t(int32_t(box[0]) >> 3 & 0xFFF) << 36 | uint64_t(int32_t(box[2]) >> 3 & 0xFFF) << 24 |
                                            uint64_t(int32_t(box[1]) >> 3 & 0xFFF) << 12 | uint64_t(int32_t(box[3]) >> 3 & 0xFFF)
                                      : 0xFFFFFFFFFFull) ^ uint64_t(narrow) << 62 ^ uint64_t(r.kind) << 60 ^ uint64_t(mask) << 59 ^
                                   uint64_t(hud.ax + 1) << 56 ^ uint64_t(hud.ay + 1) << 54 ^ uint64_t(hud_cover) << 53);
      if (wide_traced.insert(key).second)
        Log("renderer: hor+ f%llu %s%s VS %016llX PS %016llX kind %u prim %u n=%u target %ux%u %08X box %s anchor %c%c", (unsigned long long)frame,
            mask ? "mask" : narrow ? "middle" : "wide", hud_cover ? " (covered)" : "", (unsigned long long)vs->container_hash,
            (unsigned long long)(ps ? ps->container_hash : 0), r.kind, r.primitive, r.count, color->gw, color->gh, color->surface,
            known ? (std::to_string(box[0]) + "," + std::to_string(box[1]) + " .. " + std::to_string(box[2]) + "," + std::to_string(box[3])).c_str()
                  : "unknown",
            "LCR"[hud.ax + 1], "TCB"[hud.ay + 1]);
    }
    return narrow;
  }

  // HUD at the edges: the anchor of a 2D draw of the band (box in guest pixels of a w x h target). Measured in
  // 02_native, 05_mines, 10_dead_marshes (FCP_TRACE_WIDE, guest 1280x720):
  // - left/bottom: radar 113..305 x 462..692 with its blips, health/armour 83..285 x 575..672, stance 237..285;
  // - right/bottom: ammunition and grenades 954..1184 x 480..713;
  // - left/top: the message column (hints, "Press [V] to use sniper scope"): lines from x 96..101, wrapped before
  //   x 460, y 100..350; button glyphs and the text after them are draws of their own (434..469 inside a line);
  // - centre/top: compass 501..777 x 46..115; centre: crosshair, the ring 313..973 x 113..608, prompts at the bottom
  //   middle (479..762 x 580..673; centre/bottom).
  // Rule per draw: a line of the message column starts at 6..10 % of the width in the upper half and ends left of the
  // middle; it marks its rows (this and the next frame, the order of the pieces varies), and every piece left of the
  // middle whose centre lies in such a row (at most 10 % of the height) belongs to it: left/top. Else x: wholly left
  // of 52 % with the centre left of 30 %: left; wholly right of 48 % with the centre right of 70 %: right. y: wholly
  // above 52 % with the centre above 35 %: top; wholly below 48 % with the centre below 60 %: bottom. Else centre.
  // Split screen: the same within the view's rectangle vr (x0, y0, x1, y1; single player the whole target).
  void HudAnchor(const float* box, const float* vr, int& ax, int& ay) {
    const float w = vr[2] - vr[0], h = vr[3] - vr[1];
    const float b[4] = {box[0] - vr[0], box[1] - vr[1], box[2] - vr[0], box[3] - vr[1]};
    const float cx = (b[0] + b[2]) * 0.5f, cy = (b[1] + b[3]) * 0.5f;
    bool row = false;
    if (b[0] >= 0.06f * w && b[0] <= 0.1f * w && b[2] <= 0.52f * w && b[3] <= 0.52f * h) {
      row = true;
      if (n_rows_now < 16) rows_now[n_rows_now++] = {box[1], box[3]};
    } else if (b[0] >= 0.06f * w && b[2] <= 0.52f * w && b[3] - b[1] <= 0.1f * h) {
      const float cy_abs = cy + vr[1];
      auto in = [&](const HudRow& rw) { return cy_abs >= rw.top - 2.0f && cy_abs <= rw.bottom + 2.0f; };
      for (int i = 0; i < n_rows_now && !row; ++i) row = in(rows_now[i]);
      for (int i = 0; i < n_rows_last && !row; ++i) row = in(rows_last[i]);
    }
    ax = (row || (b[2] <= 0.52f * w && cx < 0.3f * w)) ? -1 : (b[0] >= 0.48f * w && cx > 0.7f * w) ? 1 : 0;
    ay = (row || (b[3] <= 0.52f * h && cy < 0.35f * h)) ? -1 : (b[1] >= 0.48f * h && cy > 0.6f * h) ? 1 : 0;
  }
  // How far the HUD's box reaches beyond the band ([graphics] hud_aspect), as a factor over 16:9 (1: the band).
  float HudWide() const {
    if (cur_wide == 1.0f) return 1.0f;
    return opt.hud_aspect <= 0.0f ? cur_wide : std::clamp(opt.hud_aspect / (16.0f / 9.0f), 1.0f, cur_wide);
  }
  float HudTall() const {
    if (cur_tall == 1.0f) return 1.0f;
    return opt.hud_aspect <= 0.0f ? cur_tall : std::clamp((16.0f / 9.0f) / opt.hud_aspect, 1.0f, cur_tall);
  }
  // Screen masks: whether another mask of the last frame lay further out on that side (0 left, 1 top, 2 right,
  // 3 bottom) over this one's rows/columns (the circle inside the scope's bars while it comes up): that one fills it.
  bool OuterMask(const float* m, uint32_t count, int side, int view = -1) const {
    for (int i = 0; i < n_masks_last; ++i) {
      const MaskSpan& o = masks_last[i];
      if (o.count == count || o.view != view) continue;
      const bool covers = side == 0 || side == 2 ? o.box[1] <= m[1] + 1.0f && o.box[3] >= m[3] - 1.0f
                                                 : o.box[0] <= m[0] + 1.0f && o.box[2] >= m[2] - 1.0f;
      if (!covers) continue;
      if (side < 2 ? o.box[side] < m[side] - 1.0f : o.box[side] > m[side] + 1.0f) return true;
    }
    return false;
  }
  // Split screen (measured, m20_shantytown with 2, 3 and 4 players): every view renders and post-processes in its own
  // target (640x720 halves side by side, 640x360 quarters; the lower ones in a 640x368 target with the picture in rows
  // 8..368) and is resolved from it into its place in the screen-sized picture (at 0,0 / 640,0 / 0,360 / 640,360).
  // The HUD of all views follows at the end of the frame in the screen-sized target, each part inside its view. Such a
  // resolve (the source a view target, the rectangle the view's size, the destination screen-sized) joins this
  // frame's views, in guest pixels of the screen (x0, y0, x1, y1).
  void NoteSplitView(const Target* src, const Resolved* dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t px,
                     int32_t py) {
    if (!IsSplitTarget(src->gw, src->gh) || !IsScreenTarget(dst->gw, dst->gh) || dst->gw < 2 * 640) return;
    const int32_t w = x1 - x0, h = y1 - y0;
    if (w != int32_t(src->gw) || h != int32_t(SplitViewHeight(src->gh))) return;
    const float r[4] = {float(px), float(py), float(px + w), float(py + h)};
    if (r[2] > float(dst->gw) + 1.0f || r[3] > float(dst->gh) + 1.0f) return;
    // (each view also resolves its 3D picture to 0,0 of a screen-sized texture for its post processing: with 3 players
    // the upper right quarter's lands inside the left half; a rectangle that overlaps a known view is not a new one)
    for (int i = 0; i < n_views_now; ++i) {
      const float* o = views_now[i].r;
      if (r[0] < o[2] && o[0] < r[2] && r[1] < o[3] && o[1] < r[3]) return;
    }
    if (n_views_now >= 4) return;
    std::copy(r, r + 4, views_now[n_views_now++].r);
    const uint64_t key = uint64_t(px & 0xFFFF) << 48 | uint64_t(py & 0xFFFF) << 32 | uint64_t(w & 0xFFFF) << 16 | uint64_t(h & 0xFFFF);
    if (views_logged.size() < 16 && views_logged.insert(key).second)
      Log("renderer: split screen f%llu: view %d at %d,%d .. %d,%d (resolved from %08X %ux%u rows %d..%d)", (unsigned long long)frame,
          n_views_now, px, py, px + w, py + h, src->surface, src->gw, src->gh, y0, y1);
  }
  // A view's identity for masks of this and the last frame (its top left corner).
  static int ViewKey(const float* vr) {
    return int(std::max(0.0f, vr[0])) << 16 | int(std::max(0.0f, vr[1]));
  }
  // Whether a mask of this or the last frame lies in that view (-1: the screen; single player every mask).
  bool MaskInView(int key) const {
    for (int i = 0; i < n_masks_now; ++i)
      if (masks_now[i].view == key || key < 0 || masks_now[i].view < 0) return true;
    for (int i = 0; i < n_masks_last; ++i)
      if (masks_last[i].view == key || key < 0 || masks_last[i].view < 0) return true;
    return false;
  }
  // The HUD the views share (team score and time, m20_shantytown): with views side by side it sits at the top in the
  // middle (traced, 2 players, guest pixels: score 546..640 x 86..119 with a "0" at 608..624 x 55..83, time 600..678 x
  // 114..163), with four quarters along the line between the upper and the lower views (read from screenshots: score
  // about 400..520, time about 600..680, rows 340..375). Its pieces lie on either side of a line between the views;
  // each narrowed about its own view's centre, they came apart (seen at 21:9 and 16:10). Such a piece stays with the
  // screen's centre: within 100 guest pixels of a line between views that runs the screen's whole height and in the
  // top 160 rows, or, where four views meet, within 260 pixels of that point across and 25 rows above or below it
  // (the quarters' own HUD keeps more distance: ammunition at rows 300..340, counters from row 375).
  static bool SharedHud(const SplitView* list, int n, float cx, float cy) {
    // Two views one above the other across the whole width (the game's horizontal split, measured 1280x360 and
    // 1280x368 targets): the shared HUD sits on the line between them (seen at 16:10: score digits above, bars below).
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        const float* a = list[i].r;
        const float* b = list[j].r;
        if (i == j || a[3] != b[1] || a[0] > 0.5f || b[0] > 0.5f || a[2] - a[0] < 1000.0f || b[2] - b[0] < 1000.0f) continue;
        if (std::fabs(cy - a[3]) <= 25.0f && std::fabs(cx - (a[0] + a[2]) * 0.5f) <= 260.0f) return true;
      }
    for (int i = 0; i < n; ++i)
      for (int j = 0; j < n; ++j) {
        const float* a = list[i].r;
        const float* b = list[j].r;
        if (i == j || a[2] != b[0]) continue;  // a left of b: the line x = a[2]
        const float x = a[2];
        if (std::fabs(cx - x) > 260.0f) continue;
        const bool full_height = a[1] <= 0.5f && b[1] <= 0.5f && (a[3] - a[1] > 400.0f || b[3] - b[1] > 400.0f);
        if (full_height && cy <= 160.0f && std::fabs(cx - x) <= 100.0f) return true;
        // four views: another pair below or above meets at the same x, so a horizontal line crosses there
        for (int k = 0; k < n; ++k) {
          const float* c = list[k].r;
          if (k == i || k == j) continue;
          if ((c[0] == a[0] || c[2] == x) && (c[1] == a[3] || c[3] == a[1])) {
            const float y = c[1] == a[3] ? a[3] : a[1];
            if (std::fabs(cy - y) <= 25.0f) return true;
          }
        }
      }
    return false;
  }
  // The view (this frame's, else the last frame's) whose rectangle holds the centre of a box; false: none (one view).
  // strict = false: only the centre decides (a screen mask reaches far beyond its view; the scissor keeps it there).
  bool ViewOf(const float* box, float out[4], bool strict = true) const {
    const float cx = (box[0] + box[2]) * 0.5f, cy = (box[1] + box[3]) * 0.5f;
    for (int pass = 0; pass < 2; ++pass) {
      const SplitView* list = pass == 0 ? views_now : views_last;
      const int n = pass == 0 ? n_views_now : n_views_last;
      if (n < 2) continue;  // a split screen has two views at least (a lone part view is no split)
      for (int i = 0; i < n; ++i) {
        const float* r = list[i].r;
        // (parts reach a little beyond their view; one that lies across views, such as the score and the time in
        // the middle of the screen, belongs to none: at most a quarter of its size outside)
        const float ox = std::max(r[0] - box[0], 0.0f) + std::max(box[2] - r[2], 0.0f);
        const float oy = std::max(r[1] - box[1], 0.0f) + std::max(box[3] - r[3], 0.0f);
        if (cx >= r[0] && cx < r[2] && cy >= r[1] && cy < r[3]) {
          if (strict && (ox > 0.25f * (box[2] - box[0]) + 2.0f || oy > 0.25f * (box[3] - box[1]) + 2.0f)) return false;
          if (strict && SharedHud(list, n, cx, cy)) return false;
          std::copy(r, r + 4, out);
          return true;
        }
      }
    }
    return false;
  }
  uint32_t view_gw = 0, view_gh = 0;

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
    // Variant word (not in Key): guest MSAA level and screen-sized target, from which the shipped list derives pad.
    rec.reserved = kVariantMark | std::min(size_from->msaa, 2u) | (IsScreenTarget(size_from->gw, size_from->gh) ? 4u : 0u);
    rec.clip = clip_cntl & (1u << 16);
    if (depth && (mode & (1u << 11))) {
      const float ps0 = d.f32(F::pa_su_poly_offset, 0), po = d.f32(F::pa_su_poly_offset, 1);
      std::memcpy(&rec.poly_scale, &ps0, 4);
      std::memcpy(&rec.poly_offset, &po, 4);
    }
    const uint64_t key = rec.Key();
    if (!opt.pipeline_collect.empty() && !solid) CollectRecord(rec);
    nvrhi::IGraphicsPipeline* pipeline = nullptr;
    nvrhi::GraphicsPipelineHandle pipeline_hold;
    if (auto it = pipelines.find(key); it != pipelines.end()) {
      pipeline = it->second;
      Counters::Get().pipelines_cached++;
    } else if (opt.pipeline_async && dlist && !Tracing() && opt.probe_x < 0) {
      // Worker threads; the rest of the list is recorded until Flush (deferred_list.h). Traces/probes read back
      // per draw and take the synchronous path.
      pipeline = AsyncPipeline(key, rec, vs_h, ps_h, solid);
      pipeline_hold = pipeline;  // a placeholder must outlive a Flush in the middle of this draw
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

    // Ambient occlusion: goes in here before the first draw after the opaque part of the 3D view (AoView).
    AoBeforeDraw(d, colors[0], depth, rec.controls[0], rec.masks[0], depth ? depth_control : 0);
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
        // z: shadow filter of the bound texture ([graphics] shadow_filter; only depth resolves, 0 = as the console)
        scale_c.texInvScale[t.slot & 31][2] = tex_depth ? float(opt.shadow_filter) : 0.0f;
      }
    };
    // b3 (render scaling): factor of the target; per fetch slot guest/host of the bound texture (otherwise 1), z the
    // shadow filter (0 = as the console), w unused.
    for (auto& s : scale_c.texInvScale) s[0] = s[1] = 1.0f, s[2] = 0.0f, s[3] = 1.0f;
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
    if (size_from->sx != 1.0f || size_from->sy != 1.0f || Shaped()) {
      vp.viewport.minX = 0.0f, vp.viewport.maxX = float(size_from->width);
      vp.viewport.minY = 0.0f, vp.viewport.maxY = float(size_from->height);
      vp.scissor = nvrhi::Rect(ScaleEdge(vp.scissor.minX, size_from->sx), ScaleEdge(vp.scissor.maxX, size_from->sx),
                               ScaleEdge(vp.scissor.minY, size_from->sy), ScaleEdge(vp.scissor.maxY, size_from->sy));
    }
    // Hor+: 2D over a wide 3D picture after its post processing into the 16:9 middle.
    // Vert+ likewise into the 16:9 band in the middle (narrower in y).
    // Screen masks: narrowed whole, not cut to the band; the sides beyond it filled after the draw (mask_fills).
    // HUD at the edges: edge elements moved to the edges of the HUD's box ([graphics] hud_aspect).
    struct MaskFill {
      int axis;             // 0 x, 1 y
      float scale, offset;  // that axis' ndcScale / ndcOffset (before AdjustNdcForApi), viewport: the whole target
      nvrhi::Rect scissor;
    };
    MaskFill mask_fills[4];
    int n_mask_fills = 0;
    const bool narrowed = Shaped() && HorPlusNarrow(r, d, vs, ps, colors[0], depth, depth_control, color_mask);
    if (narrowed) {
      const nvrhi::Rect whole = vp.scissor;
      const float tw = float(size_from->width), th = float(size_from->height);
      // The band's centre: the screen's, or with a split screen the centre of the draw's view (its own band; the view's
      // rectangle in host pixels hx0..hx1 x hy0..hy1, single player the whole target).
      const float fx = tw / float(size_from->gw), fy = th / float(size_from->gh);
      const bool in_view = hud.view >= 0;
      const float hx0 = in_view ? hud.vr[0] * fx : 0.0f, hx1 = in_view ? hud.vr[2] * fx : tw;
      const float hy0 = in_view ? hud.vr[1] * fy : 0.0f, hy1 = in_view ? hud.vr[3] * fy : th;
      if (cur_wide != 1.0f) {
        const float cx = in_view ? (hx0 + hx1) * 0.5f : tw * 0.5f, k = 1.0f / cur_wide;
        vp.viewport.minX = cx - cx * k;
        vp.viewport.maxX = cx + (tw - cx) * k;
        auto nx = [&](int32_t x) { return int32_t(std::lround(cx + (float(x) - cx) * k)); };
        vp.scissor.minX = nx(vp.scissor.minX);
        vp.scissor.maxX = nx(vp.scissor.maxX);
      }
      if (cur_tall != 1.0f) {
        const float cy = in_view ? (hy0 + hy1) * 0.5f : th * 0.5f, k = 1.0f / cur_tall;
        vp.viewport.minY = cy - cy * k;
        vp.viewport.maxY = cy + (th - cy) * k;
        auto ny = [&](int32_t y) { return int32_t(std::lround(cy + (float(y) - cy) * k)); };
        vp.scissor.minY = ny(vp.scissor.minY);
        vp.scissor.maxY = ny(vp.scissor.maxY);
      }
      if (hud.mask) {
        // Screen mask: the whole draw in the band, also where it reaches beyond it (coming up); each side of the
        // picture beyond its edge is filled with that edge: the draw once more, stretched kStretch times about a point
        // 2 guest pixels inside the edge, which lands in the middle of the strip, scissored to the strip (every host
        // pixel of the strip shows the mask within a fraction of a guest pixel of that point). Vertex shader, constants
        // and textures unchanged: only the ndc of that axis, viewport and scissor. Idea after FCI's screen masks
        // (src/gpu/renderer.cpp, own MIT code of the same author, read only); written anew for this renderer.
        // Split screen: the same within the view (its band, its edges; nothing of it reaches into another view).
        nvrhi::Rect area = whole;
        if (in_view) {
          area.minX = std::max(area.minX, int32_t(std::lround(hx0))), area.maxX = std::min(area.maxX, int32_t(std::lround(hx1)));
          area.minY = std::max(area.minY, int32_t(std::lround(hy0))), area.maxY = std::min(area.maxY, int32_t(std::lround(hy1)));
        }
        vp.scissor = area;
        if (hud.known) {
          constexpr float kStretch = 1024.0f;
          const float vx0 = vp.viewport.minX, vw = vp.viewport.maxX - vp.viewport.minX;
          const float vy0 = vp.viewport.minY, vh = vp.viewport.maxY - vp.viewport.minY;
          // (The box is in the target's guest pixels, measured: the band's viewport puts it there. The vertex shader may
          // transform the positions itself, so the stretch goes onto the clip position: ndc' = A * ndc + B.)
          const float gw = float(size_from->gw), gh = float(size_from->gh);
          auto host_x = [&](float x) { return vx0 + x / gw * vw; };
          auto host_y = [&](float y) { return vy0 + y / gh * vh; };
          const float* b = hud.box;
          const int mview = in_view ? ViewKey(hud.vr) : -1;
          // (single player: hud.vr is the whole target, hx0/hy0 0 and hx1/hy1 the target's size: as before)
          // (in a split screen's view the binoculars end 2 rows above the view's bottom, measured 16:10: -2..718 of 720)
          const float tol = in_view ? 3.0f : 1.0f;
          if (cur_wide != 1.0f && b[0] <= hud.vr[0] + tol && b[2] >= hud.vr[2] - tol)
            for (int side = 0; side < 2; ++side) {
              // (split screen: a mask reaching beyond its view shows only inside it, the game's scissor; its edge there is
              // the view's: measured, the binoculars' frame of the right view at x 593 is transparent outside the view)
              const float edge = in_view ? std::clamp(b[side * 2], hud.vr[0], hud.vr[2]) : b[side * 2], at = host_x(edge);
              if ((side == 0 ? at <= hx0 + 0.25f : at >= hx1 - 0.25f) || OuterMask(b, r.count, side * 2, mview)) continue;
              const float p = side == 0 ? edge + 2.0f : edge - 2.0f, s = side == 0 ? (hx0 + at) * 0.5f : (at + hx1) * 0.5f;
              MaskFill& f = mask_fills[n_mask_fills++];
              f.axis = 0;
              const float A = kStretch * vw / tw, B = 2.0f * s / tw - 1.0f - A * (2.0f * p / gw - 1.0f);
              f.scale = A * vp.ndc_scale[0], f.offset = A * vp.ndc_offset[0] + B;
              const int32_t x0 = side == 0 ? area.minX : std::max(area.minX, int32_t(std::floor(at)));
              const int32_t x1 = side == 0 ? std::min(area.maxX, int32_t(std::ceil(at))) : area.maxX;
              f.scissor = nvrhi::Rect(x0, x1, area.minY, area.maxY);
            }
          if (cur_tall != 1.0f && b[1] <= hud.vr[1] + tol && b[3] >= hud.vr[3] - tol)
            for (int side = 0; side < 2; ++side) {
              const float edge = in_view ? std::clamp(b[side * 2 + 1], hud.vr[1], hud.vr[3]) : b[side * 2 + 1], at = host_y(edge);
              if ((side == 0 ? at <= hy0 + 0.25f : at >= hy1 - 0.25f) || OuterMask(b, r.count, side * 2 + 1, mview)) continue;
              const float p = side == 0 ? edge + 2.0f : edge - 2.0f, s = side == 0 ? (hy0 + at) * 0.5f : (at + hy1) * 0.5f;
              MaskFill& f = mask_fills[n_mask_fills++];
              f.axis = 1;
              const float A = kStretch * vh / th, B = 1.0f - 2.0f * s / th - A * (1.0f - 2.0f * p / gh);
              f.scale = A * vp.ndc_scale[1], f.offset = A * vp.ndc_offset[1] + B;
              const int32_t y0 = side == 0 ? area.minY : std::max(area.minY, int32_t(std::floor(at)));
              const int32_t y1 = side == 0 ? std::min(area.maxY, int32_t(std::ceil(at))) : area.maxY;
              f.scissor = nvrhi::Rect(area.minX, area.maxX, y0, y1);
            }
          // The mask itself: narrowed in the ndc with the whole target as viewport, so the parts beyond the band (the
          // circle reaches 24.5 guest pixels above and below the screen, everything while coming up) are not cut off
          // by the clip volume of a narrowed viewport (measured at 16:10: world showed there). About the band's centre c
          // in the ndc: ndc' = (ndc - c) / k + c (single player c = 0, as before).
          if (cur_wide != 1.0f) {
            vp.viewport.minX = 0.0f, vp.viewport.maxX = tw;
            const float c = in_view ? (hx0 + hx1) / tw - 1.0f : 0.0f;
            vp.ndc_scale[0] /= cur_wide;
            vp.ndc_offset[0] = in_view ? (vp.ndc_offset[0] - c) / cur_wide + c : vp.ndc_offset[0] / cur_wide;
          }
          if (cur_tall != 1.0f) {
            vp.viewport.minY = 0.0f, vp.viewport.maxY = th;
            const float c = in_view ? 1.0f - (hy0 + hy1) / th : 0.0f;
            vp.ndc_scale[1] /= cur_tall;
            vp.ndc_offset[1] = in_view ? (vp.ndc_offset[1] - c) / cur_tall + c : vp.ndc_offset[1] / cur_tall;
          }
          if (masks_logged.size() < 32 &&
              masks_logged.insert(uint64_t(r.count) << 8 | uint64_t(n_mask_fills) | uint64_t(in_view) << 7).second) {
            char where[96] = "";
            if (in_view)
              std::snprintf(where, sizeof(where), ", in the split screen view %.0f,%.0f .. %.0f,%.0f", hud.vr[0], hud.vr[1],
                            hud.vr[2], hud.vr[3]);
            Log("renderer: screen mask f%llu: %u vertices, box %.1f,%.1f .. %.1f,%.1f, sides filled %d (%s %.4f)%s",
                (unsigned long long)frame, r.count, b[0], b[1], b[2], b[3], n_mask_fills, cur_wide != 1.0f ? "Hor+" : "Vert+",
                cur_wide != 1.0f ? cur_wide : cur_tall, where);
          }
        }
      } else if ((hud.ax || hud.ay) && !hud_cover && !MaskInView(in_view ? ViewKey(hud.vr) : -1)) {
        // HUD at the edges: moved by the distance between the band's edge and the HUD box's edge (split screen: those
        // of the draw's view, and only while no mask is up in that view).
        const float kx = HudWide(), ky = HudTall();
        bool moved = false;
        if (hud.ax && kx > 1.0f) {
          const float shift = float(hud.ax) * (hx1 - hx0) * 0.5f * (kx - 1.0f) / cur_wide;
          const int32_t is = int32_t(std::lround(shift));
          vp.viewport.minX += shift, vp.viewport.maxX += shift;
          vp.scissor.minX = std::clamp(vp.scissor.minX + is, 0, int32_t(tw));
          vp.scissor.maxX = std::clamp(vp.scissor.maxX + is, 0, int32_t(tw));
          moved = true;
        }
        if (hud.ay && ky > 1.0f) {
          const float shift = float(hud.ay) * (hy1 - hy0) * 0.5f * (ky - 1.0f) / cur_tall;
          const int32_t is = int32_t(std::lround(shift));
          vp.viewport.minY += shift, vp.viewport.maxY += shift;
          vp.scissor.minY = std::clamp(vp.scissor.minY + is, 0, int32_t(th));
          vp.scissor.maxY = std::clamp(vp.scissor.maxY + is, 0, int32_t(th));
          moved = true;
        }
        if (moved) ++stat_anchored;
      } else if ((hud.ax || hud.ay) && hud_cover) {
        ++stat_covered;
      }
    }
    for (uint32_t k = 0; k < 3; ++k) dc.ndcScale[k] = vp.ndc_scale[k], dc.ndcOffset[k] = vp.ndc_offset[k];
    TranslatedLayout::AdjustNdcForApi(api, dc);
    const uint32_t color_control = d.u32(F::rb_color_control);
    if (color_control & 8) {
      dc.alphaFunc = (color_control & 7) + 1;
      dc.alphaRef = d.f32(F::rb_alpha_ref);
    }
    if (color_control & 0x10) {
      // RB_COLORCONTROL ALPHA_TO_MASK_ENABLE (bit 4): coverage from alpha, not emulated (the draw is drawn without
      // it). Diagnostics only: counted and the first draws logged (none seen at the start of 8 levels, issue #1).
      ++a2m_draws;
      if (a2m_logged++ < 8)
        Log("renderer: alpha-to-mask draw (RB_COLORCONTROL %08X, frame %llu, PS %016llX), not emulated", color_control,
            (unsigned long long)frame, ps ? (unsigned long long)ps->container_hash : 0ull);
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
      // Only the changed span (64-byte steps from both ends) is copied and converted; outside it raw and out
      // already hold these bytes. FCP_RENDER_CONST_SPAN=0: whole block on every change.
      size_t lo = 0, hi = raw.size();
      if (valid) {
        constexpr size_t kStep = 64;
        while (lo < hi && std::memcmp(p + lo, raw.data() + lo, kStep) == 0) lo += kStep;
        if (lo == hi) return ++fast_const_skips, false;
        while (hi > lo + kStep && std::memcmp(p + hi - kStep, raw.data() + hi - kStep, kStep) == 0) hi -= kStep;
        if (!const_span) lo = 0, hi = raw.size();
      }
      std::memcpy(raw.data() + lo, p + lo, hi - lo);
      valid = true;
      for (size_t i = lo / 4; i < hi / 4; ++i) {
        uint32_t u;
        std::memcpy(&u, raw.data() + i * 4, 4);
        u = __builtin_bswap32(u);
        std::memcpy(&out[i], &u, 4);
      }
      ++fast_const_converts, const_span_bytes += hi - lo;
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

    // One GraphicsState for all draws: its constructor zero-fills ~1 KB of fixed-size arrays (viewports, scissors,
    // vertex buffers, bindings); resetting the sizes and the fields set below gives the same state.
    nvrhi::GraphicsState& gs = draw_gs;
    gs.viewport.viewports.resize(0);
    gs.viewport.scissorRects.resize(0);
    gs.bindings.resize(0);
    gs.vertexBuffers.resize(0);
    gs.indexBuffer = nvrhi::IndexBufferBinding();
    gs.indirectParams = gs.indirectCountBuffer = nullptr;
    if (pipeline_hold && !(dlist && dlist->recording())) {
      // The list was flushed during this draw (ring full): the placeholder's job is done, use its pipeline.
      if (auto* pp = dynamic_cast<PendingPipeline*>(pipeline)) {
        pool.Wait(pp->job());
        pipeline = pp->job()->result;
        if (!pipeline) return Skip("pipeline failed");
      }
    }
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
        // Same values as index(i), as straight loops the compiler vectorizes.
        scratch_indices.resize(r.count);
        uint32_t* out = scratch_indices.data();
        if (ib.index32) {
          for (uint32_t i = 0; i < r.count; ++i) {
            uint32_t v;
            std::memcpy(&v, src + size_t(i) * 4, 4);
            out[i] = __builtin_bswap32(v);
          }
        } else {
          for (uint32_t i = 0; i < r.count; ++i) {
            uint16_t v;
            std::memcpy(&v, src + size_t(i) * 2, 2);
            v = __builtin_bswap16(v);
            out[i] = v == 0xFFFF ? 0xFFFFFFFFu : v;
          }
        }
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
    // Screen masks: the sides beyond the mask, each with its stretched edge (above, mask_fills).
    for (int i = 0; i < n_mask_fills; ++i) {
      const MaskFill& f = mask_fills[i];
      fcshaders::DrawConstants fdc = dc;
      for (uint32_t k = 0; k < 3; ++k) fdc.ndcScale[k] = vp.ndc_scale[k], fdc.ndcOffset[k] = vp.ndc_offset[k];
      fdc.ndcScale[f.axis] = f.scale, fdc.ndcOffset[f.axis] = f.offset;
      TranslatedLayout::AdjustNdcForApi(api, fdc);
      nvrhi::Viewport fv = vp.viewport;
      if (f.axis == 0) fv.minX = 0.0f, fv.maxX = float(size_from->width);
      else fv.minY = 0.0f, fv.maxY = float(size_from->height);
      if (f.scissor.maxX <= f.scissor.minX || f.scissor.maxY <= f.scissor.minY) continue;
      tl.WriteDrawConstants(c, fdc), last_dc = fdc, ++constant_uploads, ++cb_frame[2];
      gs.viewport.viewports.resize(0);
      gs.viewport.scissorRects.resize(0);
      gs.viewport.addViewport(fv);
      gs.viewport.addScissorRect(f.scissor);
      c->setGraphicsState(gs);
      if (indexed) c->drawIndexed(args);
      else c->draw(args);
      ++stat_fills, ++cl_draws;
    }
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
          "mask %X depth %08X mode %08X alpha %08X ref %g ndc (%g %g %g %g) guest %ux%u vport (%g %g %g %g) wo %08X host vp "
          "%.1f..%.1f x %.1f..%.1f sc %d,%d-%d,%d %s%s",
          (unsigned long long)frame, r.kind, r.primitive, r.count, (unsigned long long)vs->container_hash,
          (unsigned long long)(ps ? ps->container_hash : 0), solid ? " solid" : "", size_from->width, size_from->height,
          d.u32(F::render_targets, 0), d.u32(F::depth_stencil), rec.controls[0], rec.masks[0], depth ? depth_control : 0, mode,
          color_control, dc.alphaRef, dc.ndcScale[0], dc.ndcOffset[0], dc.ndcScale[1], dc.ndcOffset[1], size_from->gw,
          size_from->gh, d.f32(F::pa_cl_vport, 0), d.f32(F::pa_cl_vport, 1), d.f32(F::pa_cl_vport, 2), d.f32(F::pa_cl_vport, 3),
          d.u32(F::pa_sc_window_offset), vp.viewport.minX, vp.viewport.maxX, vp.viewport.minY, vp.viewport.maxY, vp.scissor.minX,
          vp.scissor.minY, vp.scissor.maxX, vp.scissor.maxY,
          !Shaped() ? "" : narrowed ? (hud.mask ? "MASK" : hud.view >= 0 ? "BAND-VIEW" : "BAND") : "as-is",
          (Shaped() && narrowed && hud.known)
              ? (" box " + std::to_string(int(hud.box[0])) + "," + std::to_string(int(hud.box[1])) + ".." +
                 std::to_string(int(hud.box[2])) + "," + std::to_string(int(hud.box[3])))
                    .c_str()
              : "");
      if (opt.trace_textures && !tex.empty()) Log("renderer: T f%llu textures%s", (unsigned long long)frame, tex.c_str());
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

  // --- Ambient occlusion (AoView) -------------------------------------------------------------------------
  void AoNote(const std::string& kind, const std::string& text) {
    if (!ao_traced.insert(kind).second && !opt.ao_trace) return;
    if (ao_traced.size() > 64 && !opt.ao_trace) return;
    Log("renderer: ao f%llu: %s", (unsigned long long)frame, text.c_str());
  }

  static bool AoOpaque(uint32_t blend_control, uint32_t mask, uint32_t depth_control) {
    return (mask & 7) && !BlendTargetFrom(blend_control, mask).blendEnable && (depth_control & 2) && (depth_control & 4);
  }

  void AoViewport(const GuestDeviceView& d, float out[6]) const {
    const uint32_t vte = d.u32(F::pa_cl_vte_cntl);
    out[0] = (vte & 1) ? d.f32(F::pa_cl_vport, 0) : 1.0f;
    out[1] = (vte & 2) ? d.f32(F::pa_cl_vport, 1) : 0.0f;
    out[2] = (vte & 4) ? d.f32(F::pa_cl_vport, 2) : 1.0f;
    out[3] = (vte & 8) ? d.f32(F::pa_cl_vport, 3) : 0.0f;
    out[4] = (vte & 16) ? d.f32(F::pa_cl_vport, 4) : 1.0f;
    out[5] = (vte & 32) ? d.f32(F::pa_cl_vport, 5) : 0.0f;
  }

  // Before a draw (Draw), with the targets it goes to and its blend, colour mask and depth state.
  void AoBeforeDraw(const GuestDeviceView& d, Target* color, Target* depth, uint32_t blend_control, uint32_t mask,
                    uint32_t depth_control) {
    if (!opt.ao) return;
    AoView& v = ao_view;
    const bool screen = color && depth && IsScreenTarget(color->gw, color->gh) && depth->width == color->width &&
                        depth->height == color->height && depth->host_samples == color->host_samples;
    const bool opaque = screen && AoOpaque(blend_control, mask, depth_control);
    float vport[6];
    AoViewport(d, vport);
    if (v.pending) {
      const bool same = color == v.color && depth == v.depth;
      if (same && opaque) {
        if (std::memcmp(vport, v.vport, sizeof(vport)) == 0) {
          ++v.opaque;
          return;
        }
        AoFlush("an opaque draw with another viewport");  // e.g. the next view of a split screen
      } else if (same) {
        if (!(mask & 0xF)) {  // no colour: depth only, occlusion query boxes
          ++v.kept;
          return;
        }
        AoFlush("the first draw that blends or leaves the depth");
      } else if (depth == v.depth && depth && (depth_control & 6) == 6) {
        AoFlush("a draw into its depth with another colour target");
      } else if (color && color == v.color) {
        AoFlush("a draw into its colour target with another depth");
      } else {
        return;  // other targets (shadow maps, reflections): the view goes on after them
      }
    }
    if (!opaque) return;
    const std::array<float, 4> key{vport[0], vport[1], vport[2], vport[3]};
    if (std::find(ao_done.begin(), ao_done.end(), key) != ao_done.end()) {
      ++ao_opaque_after;  // this view had its occlusion: an opaque draw after it (the weapon, late objects) gets none
      return;
    }
    v = AoView{};
    v.pending = true;
    v.color = color, v.depth = depth;
    std::memcpy(v.vport, vport, sizeof(vport));
    v.vte = d.u32(F::pa_cl_vte_cntl);
    v.clip = d.u32(F::pa_cl_clip_cntl);
    v.window_offset = (d.u32(F::pa_su_sc_mode_cntl) & (1u << 16)) ? d.u32(F::pa_sc_window_offset) : 0;
    v.half_pixel = (d.u32(F::pa_su_vtx_cntl) & 1) == 0;
    v.opaque = 1;
    v.depth_control = depth_control;
  }

  // Before a clear or resolve of `t` (ClearTargets, ResolveRect).
  void AoBeforeUse(const Target* t, const char* what) {
    if (ao_view.pending && t && (t == ao_view.color || t == ao_view.depth)) AoFlush(what);
  }

  void AoFlush(const char* reason) {
    AoView& v = ao_view;
    if (!v.pending) return;
    v.pending = false;
    ao_done.push_back({v.vport[0], v.vport[1], v.vport[2], v.vport[3]});
    if (!opt.ao || ao_failed) return;
    float m[16], far_plane;
    uint64_t count;
    {
      std::lock_guard lock(proj_mutex);
      std::memcpy(m, proj_m, sizeof(m));
      far_plane = proj_far;
      count = proj_count;
    }
    Target& c = *v.color;
    Target& z = *v.depth;
    // PerspectiveFovRH (row-major as the camera holds it, checked against the camera's planes by the hook): [0] x
    // scale, [5] y scale, [10] f / (n - f), [11] -1, [14] n f / (n - f), [15] 0. Anything else: left out.
    if (!count || !(m[0] > 0) || !(m[5] > 0) || m[11] != -1.0f || m[15] != 0.0f || !(m[10] < 0) || !(m[14] < 0) || !(far_plane > 0)) {
      ++ao_skipped_proj;
      AoNote("proj", count ? "left out: the camera's projection is not PerspectiveFovRH" : "left out: no camera projection yet");
      return;
    }
    // The viewport in guest pixels: pixel = scale * ndc + offset (+ window offset, + 0.5 without the half pixel
    // flag: ComputeViewport); y scale negative (y down). Host pixels with the target's factors.
    const float add_x = float(int32_t(v.window_offset << 17) >> 17) + (v.half_pixel ? 0.5f : 0.0f);
    const float add_y = float(int32_t((v.window_offset >> 16) << 17) >> 17) + (v.half_pixel ? 0.5f : 0.0f);
    const float sx = v.vport[0], ox = v.vport[1] + add_x, sy = v.vport[2], oy = v.vport[3] + add_y;
    const float sz = v.vport[4], oz = v.vport[5];
    if (!(sx > 4.0f) || !(sy < -4.0f) || sz == 0.0f) {
      ++ao_skipped_shape;
      AoNote("vport", "left out: viewport " + std::to_string(sx) + "/" + std::to_string(sy) + "/" + std::to_string(sz) +
                          " (not a 3D view with y down)");
      return;
    }
    const float view_x = (ox - sx) * c.sx, view_w = 2.0f * sx * c.sx;
    const float view_y = (oy + sy) * c.sy, view_h = -2.0f * sy * c.sy;
    // The projection must have the view's shape (otherwise it belongs to another camera, e.g. split screen).
    const float shape = view_w / view_h, proj_shape = m[5] / m[0];
    if (std::fabs(shape / proj_shape - 1.0f) > 0.02f) {
      ++ao_skipped_shape;
      AoNote("shape", "left out: projection shape " + std::to_string(proj_shape) + " is not the view's " + std::to_string(shape));
      return;
    }
    if (!ao) {
      auto pass = std::make_unique<AoPass>();
      std::string error;
      if (!pass->Init(dev, opt.api, &error)) {
        ao_failed = true;
        Log("renderer: ambient occlusion not available: %s", error.c_str());
        return;
      }
      ao = std::move(pass);
    }
    AoPass::Params p;
    p.mode = int(opt.ao);
    p.depth = z.tex;
    p.color = c.tex;
    p.samples = c.host_samples;
    p.width = c.width, p.height = c.height;
    p.view_x = view_x, p.view_y = view_y, p.view_w = view_w, p.view_h = view_h;
    p.left = std::max(0, int32_t(std::floor(view_x))), p.top = std::max(0, int32_t(std::floor(view_y)));
    p.right = std::min(int32_t(c.width), int32_t(std::ceil(view_x + view_w)));
    p.bottom = std::min(int32_t(c.height), int32_t(std::ceil(view_y + view_h)));
    p.x_scale = m[0], p.y_scale = m[5];
    // stored depth = sz * ndc z + oz (ComputeViewport, every case); view depth = m14 / (ndc z + m10)
    // (double: the game's near plane is 5 mm, m10 is -1.00001; reversed depth leaves k1 = m10 * -1 - 1, a tiny number)
    // Which way the depth runs: the opaque draws' depth test (GREATER/GEQUAL = reversed: near is large) against the
    // viewport's z mapping. Measured in 02_native: GEQUAL, depth cleared to 0, viewport z scale 1 offset 0, so the
    // reversal is in the game's own matrices (stored = 1 - z / w of the camera's projection). Either way:
    //   not reversed by the matrix: ndc z = (stored - oz) / sz;  reversed by it: ndc z = 1 - (stored - oz) / sz.
    const uint32_t zfunc = (v.depth_control >> 4) & 7;
    const bool greater = zfunc == 4 || zfunc == 6, less = zfunc == 1 || zfunc == 3;
    if (!greater && !less) {
      ++ao_skipped_proj;
      AoNote("zfunc", "left out: depth test " + std::to_string(zfunc) + " (neither less nor greater)");
      return;
    }
    const bool matrix_reversed = greater == (sz > 0);
    if (!matrix_reversed) {
      p.k0 = float(double(m[14]) * sz);
      p.k1 = float(double(m[10]) * sz - double(oz));
    } else {
      p.k0 = float(-double(m[14]) * sz);
      p.k1 = float(-(double(oz) + double(sz) + double(m[10]) * sz));
    }
    p.far_plane = far_plane;
    p.strength = opt.ao_strength;
    p.half = opt.ao_resolution == 2 || (opt.ao_resolution == 0 && view_h > 720.5f);
    p.debug = opt.ao_debug;
    nvrhi::ICommandList* cl = CL();
    const bool timing = opt.ao_timing && ao_timer_pending < 4;
    if (timing) {
      if (!ao_timer[ao_timer_next]) ao_timer[ao_timer_next] = dev->createTimerQuery();
      cl->beginTimerQuery(ao_timer[ao_timer_next]);
    }
    const bool ran = ao->Run(cl, p);
    if (timing) {
      cl->endTimerQuery(ao_timer[ao_timer_next]);
      ao_timer_next = (ao_timer_next + 1) % 4;
      ++ao_timer_pending;
    }
    if (!ran) return;
    ++c.writes;  // the MSAA copy for a later resolve is out of date
    ++ao_applied;
    if (ao_applied <= 3 || opt.ao_trace)
      Log("renderer: ao f%llu: %s into %08X %ux%u (%u samples) before %s, after %u opaque draws (%u depth-only kept); view "
          "%.0f,%.0f %.0fx%.0f, projection x %.4f y %.4f near %.3f far %.1f, viewport z %.4f/%.4f, depth test %u (%s), %s "
          "resolution",
          (unsigned long long)frame, opt.ao == 2 ? "GTAO" : "SSAO", c.surface, c.width, c.height, c.host_samples, reason,
          v.opaque, v.kept, view_x, view_y, view_w, view_h, m[0], m[5], m[14] / m[10], p.far_plane, sz, oz, zfunc,
          matrix_reversed ? "reversed in the game's matrices" : "as the projection", p.half ? "half" : "full");
    else
      AoNote(std::string("before ") + reason, std::string("inserted before ") + reason);
  }

  void AoCollectTiming() {
    while (ao_timer_pending) {
      const uint32_t i = (ao_timer_next + 4 - ao_timer_pending) % 4;
      if (!ao_timer[i] || !dev->pollTimerQuery(ao_timer[i])) break;
      ao_ms_sum += double(dev->getTimerQueryTime(ao_timer[i])) * 1000.0;
      dev->resetTimerQuery(ao_timer[i]);
      --ao_timer_pending;
      if (++ao_ms_n == 300) {
        Log("renderer: ao: %.3f ms GPU per pass (average of 300, %s)", ao_ms_sum / ao_ms_n, opt.ao == 2 ? "GTAO" : "SSAO");
        ao_ms_sum = 0, ao_ms_n = 0;
      }
    }
  }

  // --- Blit (Resolve, Present) --------------------------------------------------------------------------
  nvrhi::IGraphicsPipeline* BlitPipeline(nvrhi::IFramebuffer* fb, bool gamma = false, bool colour = false) {
    const nvrhi::FramebufferInfoEx& info = fb->getFramebufferInfo();
    const uint64_t key = (info.colorFormats.empty() ? 0 : uint64_t(info.colorFormats[0])) | (gamma ? 1ull << 32 : 0) |
                         (colour ? 1ull << 33 : 0);
    if (auto it = blit_pipelines.find(key); it != blit_pipelines.end()) return it->second;
    nvrhi::GraphicsPipelineDesc pd;
    pd.primType = nvrhi::PrimitiveType::TriangleList;
    pd.VS = blit_vs;
    pd.PS = colour ? blit_colour_ps : gamma ? blit_gamma_ps : blit_ps;
    pd.bindingLayouts = tl.layouts();
    pd.renderState.rasterState.setCullNone();
    pd.renderState.rasterState.scissorEnable = false;
    pd.renderState.depthStencilState.depthTestEnable = false;
    pd.renderState.depthStencilState.depthWriteEnable = false;
    nvrhi::GraphicsPipelineHandle p = dev->createGraphicsPipeline(pd, fb->getFramebufferInfo());
    return blit_pipelines.emplace(key, p).first->second;
  }

  // Draws src_slot (rectangle in UV) into fb at viewport.
  // colour: [graphics] colour_blind, the three rows of the colour matrix (kBlitColourPS); null = the blit as before.
  void Blit(nvrhi::ICommandList* c, nvrhi::IFramebuffer* fb, uint32_t src_slot, const float uv[4], const nvrhi::Viewport& viewport,
            uint32_t gamma_lut_slot = 0, const float (*colour)[4] = nullptr) {
    nvrhi::IGraphicsPipeline* p = BlitPipeline(fb, gamma_lut_slot != 0, colour != nullptr);
    if (!p) return;
    fcshaders::DrawConstants dc{};
    dc.textureIndex[0] = src_slot;
    dc.textureIndex[1] = gamma_lut_slot;
    dc.pixelSamplers[0] = linear_clamp;
    std::memcpy(dc.clipPlanes[1], uv, 16);
    if (colour) std::memcpy(dc.clipPlanes[2], colour, 48);
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
    {
      res::TextureFetch pf = f;
      pf.words[1] = (pf.words[1] & 0xFFFu) | (base & 0xFFFFF000u);
      r.bytes = res::GetTextureRanges(pf).base_bytes;
    }
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
    AoBeforeUse(src, "a resolve of it");
    if (!src || !dst || !src->srv_slot) {
      if (src && src->host_samples > 1 && src->depth) ++skip_reasons["resolve from MSAA depth"];
      return;
    }
    if (src->depth && dst->format != nvrhi::Format::R32_FLOAT) return;
    // Hor+: the first resolve from the 3D view's target begins the post processing (HorPlusNarrow).
    if (frame_3d && !src->depth && src->surface == view_surface) post_3d = true;
    // Split screen: a finished view resolved from its own target into its place in the screen-sized picture.
    if (opt.split_views && !src->depth) NoteSplitView(src, dst, x0, y0, x1, y1, px, py);
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
      // MSAA target: first average the samples (like the 360's resolve), then copy as usual. Not again when nothing
      // was drawn into or cleared on the target since the last one (several rectangles from one target):
      // FCP_RENDER_MSAA_RESOLVE_ONCE=0 resolves every time.
      if (!msaa_resolve_once || src->single_at != src->writes) {
        c->resolveTexture(src->single, nvrhi::AllSubresources, src->tex, nvrhi::AllSubresources);
        c->setTextureState(src->single, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        src->single_at = src->writes;
        ++msaa_resolves;
      } else {
        ++msaa_resolves_skipped;
      }
    }
    nvrhi::FramebufferHandle fb = dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(dst->tex));
    if (!src->single) c->setTextureState(src->tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    const float sw = float(src->width), sh = float(src->height);
    const float uv[4] = {x0 / sw, y0 / sh, (x1 - x0) / sw, (y1 - y0) / sh};
    Blit(c, fb, src->srv_slot, uv, nvrhi::Viewport(float(dx0), float(dx1), float(dy0), float(dy1), 0, 1));
    c->setTextureState(dst->tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    dst->frame = frame;
    WatchResolved(*dst);
    ++dst->interval_resolves;
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
  bool WriteGuestImage(Resolved& r, const res::TextureFetch& f, uint32_t base, const uint8_t* rgba, size_t pitch) {
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
    if (r.watched) r.seen_epoch = EpochNow();  // our own write is not a CPU write of the game
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
    if (ao_view.pending) {
      for (uint32_t i = 0; i < 4; ++i)
        if (flags & (1u << i)) AoBeforeUse(GetTarget(d.u32(F::render_targets, i), false), "a clear of it");
      if (flags & 0x30) AoBeforeUse(GetTarget(d.u32(F::depth_stencil), true), "a clear of its depth");
      c = CL();
    }
    // Split screen: every view renders into the same target with the same viewport, beginning with a clear of colour
    // and depth (measured, 4 players: target 640x360, viewport 320/-180 for each). The occlusion is once per view, not
    // once per viewport and frame: such a clear begins a new view (else only the first player got it).
    if (opt.split_views && (flags & 0x30) && (flags & 0xF)) {
      const Target* z = GetTarget(d.u32(F::depth_stencil), true);
      if (z && IsSplitTarget(z->gw, z->gh)) ao_done.clear();
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (!(flags & (1u << i))) continue;
      if (Target* t = GetTarget(d.u32(F::render_targets, i), false))
        ++t->writes, c->clearTextureFloat(t->tex, nvrhi::AllSubresources, nvrhi::Color(color[0], color[1], color[2], color[3]));
    }
    if (flags & 0x30) {
      if (Target* t = GetTarget(d.u32(F::depth_stencil), true))
        ++t->writes, c->clearDepthStencilTexture(t->tex, nvrhi::AllSubresources, (flags & 0x10) != 0, z, (flags & 0x20) != 0, uint8_t(stencil));
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
    // Ambient occlusion: a view still open at the swap had no end before it was shown (nothing to do there).
    if (ao_view.pending) {
      ++ao_dropped;
      AoNote("dropped", "left out: the 3D view's opaque part never ended before the swap");
    }
    if (ao_applied != ao_applied_at_swap) ++ao_frames;
    ao_applied_at_swap = ao_applied;
    ++ao_swaps;
    ao_view = AoView{};
    ao_done.clear();
    if (opt.ao_timing && ao_timer_pending) AoCollectTiming();
    ReadGammaRamp(r.device);
    if (f) {
      front = f->tex;
      front_slot = f->slot;
      front_w = w;
      front_h = h;
      front_gw = f->gw;
      front_gh = f->gh;
      if (ResolvedWrittenByCpu(*f)) {
        // The CPU wrote the front buffer's memory after its last resolve (GitHub issue #4: a loading screen copying
        // one front buffer into the other): on the console that memory is what the display shows.
        ++front_cpu_swaps;
        if (front_cpu_logged++ < 16)
          Log("renderer: front buffer base %08X %ux%u written by the CPU after its resolve (frame %llu, now %llu), %s",
              f->base, f->gw, f->gh, (unsigned long long)f->frame, (unsigned long long)frame,
              opt.resolve_cpu_writes ? "shown from guest memory" : "resolve target shown (FCP_RENDER_RESOLVE_CPU_WRITES=1: guest memory)");
        if (opt.resolve_cpu_writes) {
          const uint8_t* obj = memory.Virtual(r.front_buffer);
          const res::TextureFetch fetch = res::TextureFetch::FromGuest(obj + layout.texture.get("fetch", 16));
          GuestTex* gt = nullptr;
          const uint32_t slot = TextureOf(fetch.words, CL(), gt);
          if (gt && gt->tex && slot && !(slot & 0x80000000u) && gt->table == Table::kTex2D) {
            const nvrhi::TextureDesc& d = gt->tex->getDesc();
            front = gt->tex;
            front_slot = slot;
            front_w = d.width;
            front_h = d.height;
            ++front_cpu_shown;
          }
        }
      }
    }
    if (opt.probe_x >= 0 && Tracing()) ProbeReport();
    if (k_min_dist < 1e30f) {
      Log("renderer: K f%llu nearest instance %.1f m: T (%.1f %.1f %.1f) camera (%.1f %.1f %.1f)", (unsigned long long)frame,
          k_min_dist, k_min_t[0], k_min_t[1], k_min_t[2], k_min_t[3], k_min_t[4], k_min_t[5]);
      k_min_dist = 1e30f;
    }
    Flush();
    if (opt.pipeline_async) AdoptFinished();
    DiagRewatch();
    // Diagnostics FCP_DIAG_RESCALE=<frame>,<factor>: factor change as after a resize (scale = auto).
    if (diag_rescale_frame && frame == diag_rescale_frame) wanted_scale = diag_rescale_to;
    // Hor+: show this picture in the wide shape when it had a 3D view with post processing.
    // Vert+ likewise (the tall shape).
    front_wide = Shaped() && frame_3d && post_3d;
    front_aspect = (16.0f / 9.0f) * cur_wide / cur_tall;
    if (front_wide) ++wide_frames;
    frame_3d = post_3d = false;
    // HUD at the edges, screen masks: this frame's state becomes the last frame's.
    std::copy(masks_now, masks_now + n_masks_now, masks_last), n_masks_last = n_masks_now, n_masks_now = 0;
    if (n_views_now > 1) ++stat_split_frames;
    std::copy(views_now, views_now + n_views_now, views_last), n_views_last = n_views_now, n_views_now = 0;
    std::copy(rows_now, rows_now + n_rows_now, rows_last), n_rows_last = n_rows_now, n_rows_now = 0;
    hud_vs_last = hud_vs_now, hud_vs_now = 0;
    hud_cover = false;
    view_surface = 0;
    {
      float nsx = 1.0f, nsy = 1.0f;
      FactorsFor(wanted_scale, wanted_wide, wanted_tall, nsx, nsy);
      if (wanted_scale != cur_scale || wanted_wide != cur_wide || wanted_tall != cur_tall || nsx != cur_sx || nsy != cur_sy)
        ApplyScale(wanted_scale, wanted_wide, wanted_tall);
    }
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
    RecycleSlots();
    if (tpack) PackFrame();
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
      {
        // Bindless tables: slots in use / capacity, requests refused because a table was full.
        std::string tables;
        static const char* const kNames[] = {"2D", "3D", "cube", "array", "samplers", "buffers"};
        uint64_t refused = 0;
        for (uint32_t i = 0; i < uint32_t(Table::kCount); ++i) {
          char part[64];
          std::snprintf(part, sizeof(part), "%s%s %u/%u", i ? ", " : "", kNames[i], tl.used(Table(i)), tl.capacity(Table(i)));
          tables += part;
          refused += tl.refused(Table(i));
        }
        std::string refused_by;
        for (uint32_t i = 0; i < uint32_t(Table::kCount); ++i)
          if (tl.refused(Table(i))) refused_by += std::string(" ") + kNames[i] + "=" + std::to_string(tl.refused(Table(i)));
        std::unordered_set<uint32_t> bases;
        for (const auto& [k, t] : textures) bases.insert(res::TextureFetch::FromWords(k.w.data()).BaseAddress());
        Log("renderer: tables %s; textures %zu (%zu base addresses), buffers %zu, samplers %zu, refused %llu%s; recycled: "
            "%llu textures, %llu buffers; alpha-to-mask draws %llu; fetches at a resolve base with another size %llu",
            tables.c_str(), textures.size(), bases.size(), buffers.size(), samplers.size(), (unsigned long long)refused,
            refused_by.c_str(), (unsigned long long)evicted_textures, (unsigned long long)evicted_buffers,
            (unsigned long long)a2m_draws, (unsigned long long)stale_resolve_binds);
        Log("renderer: allocations: estimated %zu of %u (%zu textures%s, %zu vertex buffers, %zu index buffers, %zu targets, "
            "%zu resolve targets, %zu test); refused: %llu textures, %llu vertex buffers, %llu index buffers; allocation "
            "passes %llu, evicted index buffers %llu, zeroed subresources (pool) %llu",
            EstimatedAllocations(), host ? host->max_allocations() : 0, textures.size(), host && host->pooled_textures() ? " pooled" : "",
            buffers.size(), index_buffers.size(), targets.size(), resolved.size(), test_allocs.size(),
            (unsigned long long)alloc_fail_tex, (unsigned long long)alloc_fail_buf, (unsigned long long)alloc_fail_idx,
            (unsigned long long)alloc_passes, (unsigned long long)evicted_index, (unsigned long long)zeroed_subresources);
        if (opt.ao || ao_applied)
          Log("renderer: ao %s: in %llu of %llu frames; so far %llu passes, left out %llu projection, %llu shape, %llu without an end; "
              "opaque draws after it %.1f per frame",
              opt.ao == 2 ? "GTAO" : opt.ao ? "SSAO" : "off", (unsigned long long)ao_frames, (unsigned long long)ao_swaps,
              (unsigned long long)ao_applied, (unsigned long long)ao_skipped_proj, (unsigned long long)ao_skipped_shape,
              (unsigned long long)ao_dropped, ao_frames ? double(ao_opaque_after) / double(ao_frames) : 0.0);
        ao_frames = ao_swaps = ao_opaque_after = 0;
      }
      if (st.queued)
        Log("renderer: async shaders: %llu translated on workers (%.0f ms), draws waited %.1f ms", (unsigned long long)st.queued,
            st.translate_ms, st.wait_ms);
      if (opt.pipeline_async && async_jobs)
        Log("renderer: async pipelines: %llu jobs, %llu deferred lists (%llu ops, %llu KB copied, %llu ended early), render "
            "thread waited %.1f ms, creation %.1f ms on %u threads (%llu by waiting threads); prewarm %llu/%llu, pending %zu, "
            "not ready in time %llu",
            (unsigned long long)async_jobs, (unsigned long long)async_lists, (unsigned long long)async_replayed_ops,
            (unsigned long long)(async_replayed_bytes >> 10), (unsigned long long)async_materialize_early, async_wait_us / 1000.0,
            async_create_us / 1000.0, pool.threads(), (unsigned long long)pool.jobs_helped(), (unsigned long long)prewarm_done,
            (unsigned long long)prewarm_jobs, pending.size(), (unsigned long long)async_late);
      if (Watching()) {
        Log("renderer: write watch: %llu checks without writes skipped, %llu hashed, %llu textures changing constantly (hash per frame)",
            (unsigned long long)watch_skips, (unsigned long long)watch_checks, (unsigned long long)watch_volatile);
        if (opt.watch_rehash)
          Log("renderer: write watch safety net (up to %u bytes): changes without a reported write: textures %llu, vertex "
              "buffers %llu, index buffers %llu (different entries %llu/%llu/%llu)",
              opt.watch_rehash, (unsigned long long)rehash_caught[0], (unsigned long long)rehash_caught[1],
              (unsigned long long)rehash_caught[2], (unsigned long long)rehash_entries[0],
              (unsigned long long)rehash_entries[1], (unsigned long long)rehash_entries[2]);
        if (opt.watch_verify)
          Log("renderer: write watch verify (%s): skipped and hashed anyway: textures %llu, vertex buffers %llu, index buffers "
              "%llu; changed without a reported write: textures %llu, vertex buffers %llu, index buffers %llu (different "
              "entries %llu/%llu/%llu; write reported during the check: %llu/%llu/%llu)",
              opt.watch_epoch ? "epoch" : "shared bits", (unsigned long long)verify_checks[0], (unsigned long long)verify_checks[1],
              (unsigned long long)verify_checks[2], (unsigned long long)verify_missed[0], (unsigned long long)verify_missed[1],
              (unsigned long long)verify_missed[2], (unsigned long long)verify_entries[0], (unsigned long long)verify_entries[1],
              (unsigned long long)verify_entries[2], (unsigned long long)verify_races[0], (unsigned long long)verify_races[1],
              (unsigned long long)verify_races[2]);
        if (resolve_cpu_events || front_cpu_swaps)
          Log("renderer: resolve targets written by the CPU after their resolve: %llu times, %llu binds; front buffers %llu "
              "swaps (%llu shown from guest memory) %s",
              (unsigned long long)resolve_cpu_events, (unsigned long long)resolve_cpu_binds,
              (unsigned long long)front_cpu_swaps, (unsigned long long)front_cpu_shown,
              opt.resolve_cpu_writes ? "" : "(resolve targets kept; FCP_RENDER_RESOLVE_CPU_WRITES=1: guest memory)");
      }
      Log("renderer: constant versions per frame at most VS %u PS %u Draw %u (ring %u); raw data unchanged %llu, converted %llu "
          "(%.0f bytes each)",
          cb_max[0], cb_max[1], cb_max[2], tl.constant_versions(), (unsigned long long)fast_const_skips,
          (unsigned long long)fast_const_converts, fast_const_converts ? double(const_span_bytes) / fast_const_converts : 0.0);
      fast_const_skips = fast_const_converts = const_span_bytes = 0;
      Log("renderer: MSAA resolves %llu, not repeated (target unchanged) %llu", (unsigned long long)msaa_resolves,
          (unsigned long long)msaa_resolves_skipped);
      msaa_resolves = msaa_resolves_skipped = 0;
      {
        // Resolve targets refreshed (resolves) and used (binds) in this interval, ordered by base address.
        std::vector<const Resolved*> rs;
        for (const auto& [b, r] : resolved) rs.push_back(&r);
        std::sort(rs.begin(), rs.end(), [](const Resolved* a, const Resolved* b) { return a->base < b->base; });
        std::string line;
        for (const Resolved* r : rs) {
          char item[64];
          std::snprintf(item, sizeof(item), " %08X %ux%u f%u %u/%u", r->base, r->gw, r->gh, unsigned(r->format), r->interval_resolves,
                        r->interval_binds);
          line += item;
        }
        Log("renderer: resolves per target (base size format resolves/binds):%s", line.c_str());
        for (auto& [b, r] : resolved) r.interval_resolves = r.interval_binds = 0;
      }
      if (Shaped()) {
        Log("renderer: %s %.4f: %llu wide frames of %u, 2D centred %llu, full width %llu, box unknown %llu",
            cur_wide != 1.0f ? "Hor+" : "Vert+", cur_wide != 1.0f ? cur_wide : cur_tall, (unsigned long long)wide_frames, opt.stats_every, (unsigned long long)narrow_draws, (unsigned long long)full_draws,
            (unsigned long long)unknown_boxes);
        Log("renderer: HUD layout %s: %llu draws at the edges, %llu kept in the band under a cover (menus); screen masks %llu, "
            "sides filled %llu",
            opt.hud_aspect <= 0.0f ? "auto" : std::fabs(opt.hud_aspect - 16.0f / 9.0f) < 1e-3f ? "16:9" : "box",
            (unsigned long long)stat_anchored, (unsigned long long)stat_covered, (unsigned long long)stat_masks,
            (unsigned long long)stat_fills);
        if (stat_split_frames || stat_view_hud || stat_view_masks || stat_window_offset)
          Log("renderer: split screen: %llu frames with views, %llu HUD draws in a view's band, %llu screen masks in a view, "
              "%llu view quads drawn at their place on the screen%s",
              (unsigned long long)stat_split_frames, (unsigned long long)stat_view_hud, (unsigned long long)stat_view_masks,
              (unsigned long long)stat_window_offset, opt.split_views ? "" : " (FCP_RENDER_SPLIT_VIEW=0)");
        wide_frames = narrow_draws = full_draws = unknown_boxes = 0;
        stat_anchored = stat_covered = stat_masks = stat_fills = 0;
        stat_split_frames = stat_view_hud = stat_view_masks = stat_window_offset = 0;
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
    const bool missed = verify_dump_now;  // FCP_RENDER_WATCH_VERIFY: a frame drawn with stale data
    verify_dump_now = false;
    if (!listed && !every && !missed) return;
    if (missed) Log("renderer: dump frame %llu: drawn with content the write watch missed", (unsigned long long)frame);
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

  // Present dump with an HDR10 swapchain (FCP_DUMP_PRESENT, RequestPresentDump): <name> = the SDR picture the HDR10
  // pass mapped (without overlay), <base>_hdr10.raw = the swapchain image as presented ("FCPHDR10", uint32 width,
  // height, picture x, y, w, h, then one R10G10B10A2 word per pixel, rows top first), <base>_hdr10.txt = the
  // parameters. tools/hdr_check.py decodes and compares them.
  void DumpHdr(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const std::string base = dot == std::string::npos ? path : path.substr(0, dot);
    std::string error;
    std::vector<uint32_t> words;
    const nvrhi::TextureDesc& d = back->getDesc();
    if (!ReadbackRaw32(dev, back, words, &error)) {
      Log("renderer: HDR10 dump %s failed: %s", path.c_str(), error.c_str());
      return;
    }
    if (FILE* f = std::fopen((base + "_hdr10.raw").c_str(), "wb")) {
      const uint32_t head[6] = {d.width, d.height, uint32_t(hdr_rect[0]), uint32_t(hdr_rect[1]), uint32_t(hdr_rect[2] + 0.5f),
                                uint32_t(hdr_rect[3] + 0.5f)};
      std::fwrite("FCPHDR10", 1, 8, f);
      std::fwrite(head, 4, 6, f);
      std::fwrite(words.data(), 4, words.size(), f);
      std::fclose(f);
    }
    if (FILE* f = std::fopen((base + "_hdr10.txt").c_str(), "w")) {
      std::fprintf(f, "paper_white %g\npeak %g\ngain %g\nknee %g\ngamma %g\ntest_pattern %u\napi %s\nframe %llu\n"
                   "display_hdr %d\ndisplay_peak %g\ndisplay_sdr_white %g\n",
                   hdr_levels.paper_white, hdr_levels.peak, hdr_levels.gain, hdr_levels.knee, hdr_levels.gamma, hdr_levels.test_pattern,
                   HostApiName(opt.api), (unsigned long long)frame, display_hdr.enabled ? 1 : 0, display_hdr.max_nits,
                   display_hdr.sdr_white_nits);
      std::fclose(f);
    }
    std::vector<uint8_t> rgba;
    if (hdr_sdr && ReadbackRgba8(dev, hdr_sdr, rgba, &error) && WriteImage(path, hdr_sdr->getDesc().width, hdr_sdr->getDesc().height, rgba, &error))
      Log("renderer: HDR10 dump -> %s_hdr10.raw (%ux%u, frame %llu), SDR picture -> %s", base.c_str(), d.width, d.height,
          (unsigned long long)frame, path.c_str());
    else
      Log("renderer: HDR10 dump -> %s_hdr10.raw, SDR picture failed: %s", base.c_str(), error.c_str());
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
    if (vsync_pending) {
      // settings menu (frame rate, VSync): the swapchain anew with FIFO or without (Vulkan), the sync interval (D3D12)
      vsync_pending = false;
      back_fbs.clear();
      const bool ok = host->SetVsync(opt.vsync);
      Log("renderer: present %s (settings menu)%s", opt.vsync ? "with VSync" : "without waiting for the monitor",
          ok ? "" : ": FAILED");
    }
    // Vulkan: a swapchain the driver reported out of date (exclusive mode change, restore from minimized at the same
    // size: no resize comes) is made anew at the window's size once it has one; at most every 100 ms.
    // FCP_RENDER_RECREATE_LOST=0: only on resize events, as before.
    static const bool recreate_lost = !std::getenv("FCP_RENDER_RECREATE_LOST") ||
                                      std::string(std::getenv("FCP_RENDER_RECREATE_LOST")) != "0";
    if (!resize_pending && recreate_lost && host->swapchain_lost() && Clock::now() >= lost_retry) {
      uint32_t w = 0, h = 0;
      lost_retry = Clock::now() + std::chrono::milliseconds(100);
      if (host->window_client_size(&w, &h) && w && h) {
        Log("renderer: swapchain out of date, made anew at the window's %ux%u", w, h);
        resize_pending = true;
        resize_w = w, resize_h = h;
      }
    }
    if (hdr_pending) {
      // [display] hdr from the settings menu: a new swapchain in the other mode (same size)
      hdr_pending = false;
      display_hdr = QueryDisplayHdr(hwnd);
      LogDisplayHdr("settings menu");
      const bool want = WantHdr();
      if (want != host->hdr_active()) {
        back_fbs.clear();
        host->SetHdr(want);
      }
      SettleHdr(want, "settings menu");
    }
    if (!resize_pending) return;
    resize_pending = false;
    back_fbs.clear();
    if (host->Resize(resize_w, resize_h)) {
      Log("renderer: Swapchain %ux%u", host->swapchain_width(), host->swapchain_height());
      out_w = host->swapchain_width(), out_h = host->swapchain_height();  // native resolution
      wanted_wide = WideFor(host->swapchain_width(), host->swapchain_height());   // Hor+ likewise
      wanted_tall = TallFor(host->swapchain_width(), host->swapchain_height());   // Vert+ likewise
      if (opt.scale_auto) wanted_scale = AutoScale(host->swapchain_height(), wanted_tall);  // applied at the next swap
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

  // [graphics] colour_blind: matrix of the mode and the shader (made on first use). false: the final blit as before.
  bool ColourReady() {
    const int mode = opt.colour_blind;
    if (mode < 1 || mode > 3 || blit_colour_failed) return false;
    if (!blit_colour_ps) {
      std::string e;
      blit_colour_ps = CreateBuiltinShader(dev, opt.api, BuiltinShader::kBlitColourPS, &e);
      if (!blit_colour_ps) {
        Log("renderer: colour blind blit not available: %s", e.c_str());
        blit_colour_failed = true;
        return false;
      }
    }
    if (colour_mode != mode) {
      colour_mode = mode;
      ColourBlindMatrix(mode, colour_matrix);
      Log("renderer: colour blind mode %s: %.4f %.4f %.4f / %.4f %.4f %.4f / %.4f %.4f %.4f", ColourBlindName(mode),
          colour_matrix[0][0], colour_matrix[0][1], colour_matrix[0][2], colour_matrix[1][0], colour_matrix[1][1],
          colour_matrix[1][2], colour_matrix[2][0], colour_matrix[2][1], colour_matrix[2][2]);
    }
    return true;
  }

  // HDR10: the SDR picture at the swapchain's size (recreated on a size change). false: no HDR10 pass this frame.
  bool HdrTarget(uint32_t w, uint32_t h) {
    if (hdr_sdr && hdr_sdr->getDesc().width == w && hdr_sdr->getDesc().height == h) return true;
    nvrhi::TextureDesc d;
    d.width = w;
    d.height = h;
    d.format = nvrhi::Format::RGBA8_UNORM;
    d.isRenderTarget = true;
    d.initialState = nvrhi::ResourceStates::ShaderResource;
    d.keepInitialState = true;
    d.setClearValue(nvrhi::Color(0, 0, 0, 1));
    d.debugName = "hdr10 sdr picture";
    hdr_sdr = dev->createTexture(d);
    hdr_sdr_fb = hdr_sdr ? dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(hdr_sdr)) : nullptr;
    if (!hdr_sdr_fb) {
      Log("renderer: HDR10: SDR picture %ux%u failed", w, h);
      hdr_sdr = nullptr;
      return false;
    }
    Log("renderer: HDR10: SDR picture %ux%u", w, h);
    return true;
  }

  bool BeginPresent() {
    if (!ready || !host || !host->has_swapchain()) return false;
    HandleResize();
    back = host->BeginFrame();
    if (!back) return false;
    auto it = back_fbs.find(back);
    if (it == back_fbs.end()) it = back_fbs.emplace(back, dev->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(back))).first;
    nvrhi::IFramebuffer* fb = it->second;
    nvrhi::IFramebuffer* back_fb = fb;
    // HDR10: the SDR path below draws into hdr_sdr instead of the swapchain, the HDR10 pass then maps it.
    const bool hdr = hdr_on && HdrTarget(back->getDesc().width, back->getDesc().height);
    if (hdr) fb = hdr_sdr_fb;
    present_cl->open();
    present_cl->clearTextureFloat(hdr ? hdr_sdr.Get() : back, nvrhi::AllSubresources, nvrhi::Color(0, 0, 0, 1));
    if (front) {
      // Aspect ratio of the frame, centred (bars instead of distortion).
      const float bw = float(back->getDesc().width), bh = float(back->getDesc().height);
      // Hor+: a picture with a 3D view in the host texture's shape (wide), all others in the guest shape (16:9).
      // Vert+ likewise (tall). A fixed render resolution may have another shape than the picture: the picture's.
      const float aspect = front_wide && opt.resolution_mode == 2 ? front_aspect
                           : front_wide && front_h ? float(front_w) / float(front_h)
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
      const float (*colour)[4] = ColourReady() ? colour_matrix : nullptr;  // [graphics] colour_blind
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
        Blit(present_cl, fb, smaa_out_slot, uv, nvrhi::Viewport(x, x + w, y, y + h, 0, 1), 0, colour);
        ++smaa_frames;
      } else {
        Blit(present_cl, fb, front_slot, uv, nvrhi::Viewport(x, x + w, y, y + h, 0, 1), gamma ? gamma_lut_slot : 0,
             colour);
      }
      hdr_rect[0] = x, hdr_rect[1] = y, hdr_rect[2] = w, hdr_rect[3] = h;
    }
    if (hdr) {
      hdr_pass->Run(present_cl, hdr_sdr, back_fb, hdr_levels);
      ++hdr_frames;
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
  if (options.shader_async) {
    so.translate_threads = options.pipeline_threads ? options.pipeline_threads : AutoPipelineThreads();
    Impl* mi = &m;
    so.on_translated = [mi](const ShaderEntry& e, double ms) {
      mi->Log("renderer: Shader %016llX %s %.1f ms (worker)%s%s", (unsigned long long)e.container_hash, ShaderSourceName(e.source), ms,
              e.source == ShaderSource::kFailed ? " error: " : "", e.source == ShaderSource::kFailed ? e.error.c_str() : "");
      mi->NotifyShader(e.container_hash);  // prewarm records that waited for this shader
    };
  }
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
  impl_->Log("renderer: SDK write watch %s%s%s", watch ? (impl_->opt.texture_watch ? "active for guest textures" : "present, off (FCP_RENDER_TEXTURE_WATCH=0)") : "off",
             watch && impl_->opt.texture_watch ? (impl_->opt.watch_epoch ? ", write epochs per entry" : ", shared dirty bits (FCP_RENDER_WATCH_EPOCH=0)") : "",
             watch && impl_->opt.watch_verify ? ", verifying skipped checks (FCP_RENDER_WATCH_VERIFY)" : "");
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
    m.present_swap = m.total_swaps;
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
  m.present_swap = m.total_swaps;
  return ok;
}

void GameRenderer::EndPresent() {
  Impl& m = *impl_;
  m.pacer.BeforePresent();  // display pacing: holds the present (only), without the renderer lock
  Impl::TimedLock lock(m, kSitePresent);
  if (!m.host || !m.back) return;
  if (!m.present_dump.empty() && m.back->getDesc().format == nvrhi::Format::R10G10B10A2_UNORM) {
    m.DumpHdr(m.present_dump);
    m.present_dump.clear();
  }
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
  m.pacer.AfterPresent(m.present_swap);
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
  // HDR10 swapchain: the overlay at paper white in PQ (no highlight stretch)
  const float hdr[2] = {m.hdr_levels.paper_white / 10000.0f, m.hdr_levels.gamma};
  const bool hdr_back = m.back->getDesc().format == nvrhi::Format::R10G10B10A2_UNORM;
  const uint32_t draws = m.overlay->Draw(cl, it->second, frame, hdr_back ? hdr : nullptr);
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

void GameRenderer::SetHudAspect(float aspect) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.hud_aspect == aspect) return;
  impl_->opt.hud_aspect = aspect;
  impl_->Log("renderer: HUD layout %s (settings menu)", aspect == 0.0f ? "auto (the picture's edges)"
                                                       : std::fabs(aspect - 16.0f / 9.0f) < 1e-3f ? "16:9 (band)" : "box");
}

void GameRenderer::SetSmaa(bool on) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.smaa == on) return;
  impl_->opt.smaa = on;
  impl_->Log("renderer: SMAA %s (settings menu, from the next present)", on ? "on" : "off");
}

void GameRenderer::SetVsync(bool on) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.vsync == on) return;
  impl_->opt.vsync = on;
  impl_->vsync_pending = impl_->host != nullptr;  // before the device: CreateHost takes opt.vsync
  impl_->Log("renderer: VSync %s (settings menu, from the next present)", on ? "on" : "off");
}

void GameRenderer::SetPaceTarget(double fps) {
  {
    Impl::TimedLock lock(*impl_, kSiteOther);
    if (impl_->opt.pace_fps == fps) return;
    impl_->opt.pace_fps = fps;  // a host made later starts the pacer with it
  }
  impl_->pacer.SetTarget(fps);
}

void GameRenderer::SetFramesAhead(uint32_t frames) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.frames_ahead == frames) return;
  impl_->Log("renderer: frames ahead %u -> %u (settings menu, from the next swap)", impl_->opt.frames_ahead, frames);
  impl_->opt.frames_ahead = frames;
}

void GameRenderer::SetColourBlind(int mode) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  mode = mode >= 0 && mode <= 3 ? mode : 0;
  if (impl_->opt.colour_blind == mode) return;
  impl_->opt.colour_blind = mode;
  impl_->Log("renderer: colour blind mode %s (settings menu, from the next present)", ColourBlindName(mode));
}

void GameRenderer::SetHdr(uint32_t mode) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  mode = std::min(mode, 2u);
  if (impl_->opt.hdr == mode) return;
  impl_->opt.hdr = mode;
  impl_->hdr_pending = impl_->host && impl_->host->has_swapchain();
  impl_->Log("renderer: [display] hdr = %s (settings menu, at the next present)", mode == 1 ? "on" : mode == 2 ? "auto" : "off");
}

void GameRenderer::SetHdrLevels(float paper_white, float peak, float boost) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  Impl& m = *impl_;
  if (m.opt.hdr_paper_white == paper_white && m.opt.hdr_peak == peak && m.opt.hdr_boost == boost) return;
  m.opt.hdr_paper_white = paper_white, m.opt.hdr_peak = peak, m.opt.hdr_boost = boost;
  m.UpdateHdrLevels();
  m.Log("renderer: HDR10 paper white %.0f nits, peak %.0f nits, highlight gain x%.2f (settings menu%s)", m.hdr_levels.paper_white,
        m.hdr_levels.peak, m.hdr_levels.gain, m.hdr_on ? "" : "; HDR10 output is off");
}

bool GameRenderer::HdrActive() const {
  Impl::TimedLock lock(*impl_, kSiteOther);
  return impl_->hdr_on;
}

void GameRenderer::SetShadowFilter(uint32_t mode) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  if (impl_->opt.shadow_filter == mode) return;
  impl_->opt.shadow_filter = mode;
  impl_->Log("renderer: shadow filter %s (settings menu, from the next draw)", mode ? "soft" : "console");
}

void GameRenderer::SetAo(uint32_t mode) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  mode = std::min(mode, 2u);
  if (impl_->opt.ao == mode) return;
  impl_->opt.ao = mode;
  impl_->Log("renderer: ambient occlusion %s (settings menu, from the next frame)", mode == 2 ? "GTAO" : mode ? "SSAO" : "off");
}

void GameRenderer::SetCameraProjection(const float m[16], float far_plane) {
  if (!impl_) return;
  std::lock_guard lock(impl_->proj_mutex);
  std::memcpy(impl_->proj_m, m, sizeof(impl_->proj_m));
  impl_->proj_far = far_plane;
  ++impl_->proj_count;
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
  m.wanted_scale = scale_auto && m.host && m.host->has_swapchain() ? Impl::AutoScale(m.host->swapchain_height(), m.wanted_tall)
                                                                   : m.opt.scale;
  m.Log("renderer: render scale requested %s%.3f (settings menu, from the next swap)%s", scale_auto ? "auto = " : "",
        m.wanted_scale, m.opt.resolution_mode ? "; [graphics] resolution is set and decides" : "");
}

void GameRenderer::SetResolution(uint32_t mode, uint32_t w, uint32_t h) {
  Impl::TimedLock lock(*impl_, kSiteOther);
  Impl& m = *impl_;
  m.opt.resolution_mode = mode <= 2 ? mode : 0;
  m.opt.res_w = w, m.opt.res_h = h;
  float sx = 1.0f, sy = 1.0f;
  m.FactorsFor(m.wanted_scale, m.wanted_wide, m.wanted_tall, sx, sy);
  m.Log("renderer: render resolution requested %s -> screen targets %ux%u (settings menu, from the next swap)",
        mode == 1 ? "native" : mode == 2 ? "fixed" : "from the scale", HostSize(1280, sx), HostSize(720, sy));
}

float GameRenderer::Wide() const { return impl_ ? impl_->wide_published.load() : 1.0f; }
float GameRenderer::Tall() const { return impl_ ? impl_->tall_published.load() : 1.0f; }

void GameRenderer::OnLevelLoading(const std::string& level) {
  if (!impl_) return;
  Impl& m = *impl_;
  const std::string g = GroupName(level);
  {
    std::lock_guard lock(m.collect_mutex);
    m.collect_group = g;
  }
  uint32_t mask = 0;
  size_t moved = 0, fed = 0;
  {
    std::lock_guard lock(m.feed_mutex);
    for (size_t i = 0; i < m.group_names.size(); ++i)
      if (m.group_names[i] == g) mask = 1u << i;
    if (mask) {
      auto mid = std::stable_partition(m.feed.begin(), m.feed.end(), [mask](const ShippedRec& s) { return (s.groups & mask) != 0; });
      fed = size_t(mid - m.feed.begin());
    }
  }
  if (!mask || !m.opt.pipeline_async) return;
  std::lock_guard lock(m.mutex);
  m.loading_mask = mask;
  moved = m.pool.PrioritizeBackground(mask);
  m.Log("renderer: prewarm: level %s loading, %zu queued pipelines of it moved ahead (%zu not queued yet), %zu queued in all",
        g.c_str(), moved, fed, m.pool.background_queued());
}

void GameRenderer::Shutdown() {
  if (!impl_) return;
  impl_->prewarm_stop = true;
  {
    std::lock_guard fl(impl_->feed_mutex);
    impl_->feed_cv.notify_all();
  }
  if (impl_->prewarm.joinable()) impl_->prewarm.join();
  std::lock_guard lock(impl_->mutex);
  Impl& m = *impl_;
  if (!m.host || m.shut_down) return;
  m.pacer.Stop();  // no waits on the display from here on
  m.shut_down = true;
  m.ready = false;
  m.failed = true;  // no new devices after shutdown
  if (m.cl_open) m.Flush();
  m.pool.Stop();  // queued prewarm jobs are dropped; running ones finish
  m.pending.clear();
  m.host->WaitIdle();
  m.host->SavePipelineCache();
  m.Log("renderer: end. %s", m.ShutdownSummary().c_str());
  if (m.overlay) m.Log("renderer: overlay: %llu frames with overlay, %llu draws, %zu textures", (unsigned long long)m.overlay_frames,
                       (unsigned long long)m.overlay_draws, m.overlay->texture_count());
  if (m.hdr_frames) m.Log("renderer: HDR10: %llu presents through the HDR10 pass", (unsigned long long)m.hdr_frames);
  if (m.replace_source)
    m.Log("renderer: texture replacement: %llu textures checked, %llu bindings replaced, %llu images uploaded",
          (unsigned long long)m.replace_classified, (unsigned long long)m.replace_binds, (unsigned long long)m.replace_uploads);
  if (m.tpack) {
    m.Log("renderer: texture pack: %s; %s", m.PackStats().c_str(), m.tpack->Summary().c_str());
    m.tpack.reset();  // writes the dumps still queued
  }
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
      dst->depth = true;  // a shadow map: [graphics] shadow_filter applies when a shader fetches it
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
  if (e && !e->pending.load(std::memory_order_acquire)) impl_->NotifyShader(e->container_hash);
  std::lock_guard lock(impl_->mutex);
  if (e && e->pending.load(std::memory_order_acquire))
    impl_->Log("renderer: Shader %s %08X -> %016llX queued for a worker %.1f ms", r.kind == kVertexShader ? "VS" : "PS", r.object,
               (unsigned long long)e->container_hash, MsSince(t0));
  else if (e)
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
  uint64_t swap = 0;
  {
    Impl::TimedLock lock(m, kSiteSwap);
    if (!m.ready) return;
    m.Swap(r);
    swap = m.total_swaps;
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
  m.pacer.AfterSwap(swap);  // display pacing: the game waits for the display, not for its own clock
}

bool GameRenderer::DisplayPacingActive() const { return impl_ && impl_->pacer.active(); }
double GameRenderer::DisplayFrameMs() const { return impl_ ? impl_->pacer.frame_ms() : 0.0; }

}  // namespace fcp::nr
