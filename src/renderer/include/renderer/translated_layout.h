// The single pipeline layout of all translated shaders (tools/shaders/include/fcshaders/abi.h), on NVRHI:
//   set/space 0: b0 VS constants, b1 PS constants, b2 DrawConstants, b3 ScaleConstants (volatile constant buffers)
//   1 Texture2D[], 2 Texture3D[], 3 TextureCube[], 4 Texture2DArray[], 5 SamplerState[], 6 ByteAddressBuffer[]
// Structure after King Kong Recompiled (game_renderer.cpp, GameRenderer::Initialize, MIT): under Vulkan each
// table is its own descriptor set, under D3D12 the entry's slot is the register space. Slot 0 of each
// table is a fallback (black 1x1 texture, point sampler, small zero buffer) for unset fetches.
// Own code; the arrangement of the layouts is taken from King Kong.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <nvrhi/nvrhi.h>

#include "fcshaders/abi.h"

namespace fcp::nr {

struct ShaderEntry;

enum class Table : uint32_t { kTex2D = 0, kTex3D, kTexCube, kTex2DArray, kSamplers, kBuffers, kCount };

struct TableCapacities {
  std::array<uint32_t, size_t(Table::kCount)> slots{4096, 64, 256, 64, 256, 4096};
  // Versions per volatile constant buffer (NVRHI maxVersions, Vulkan only; D3D12 uses an upload ring).
  // All versions of the lists the GPU has not finished yet are in use (up to three frames). Measured up to
  // 2,378 draw constants per frame (26_shantynight); 4,096 was not enough (NVRHI then waited for the GPU).
  // 8,192 = 32 MB per 4 KB buffer. FCP_RENDER_CB_VERSIONS=4096: old setting.
  // Lower bound: a list not yet submitted (kMaxDrawsPerList) must always fit.
  uint32_t constant_versions = 8192;
};

// Fixed states outside the shaders (part of the pipeline key, kk/backend/pipeline_cache.h).
struct FixedState {
  nvrhi::PrimitiveType primitive = nvrhi::PrimitiveType::TriangleList;
  nvrhi::BlendState::RenderTarget blend;
  nvrhi::DepthStencilState depth;
  nvrhi::RasterState raster;
};

class TranslatedLayout {
 public:
  bool Init(nvrhi::IDevice* device, const TableCapacities& capacities, std::string* error);
  nvrhi::IDevice* device() const { return device_; }
  const nvrhi::BindingLayoutVector& layouts() const { return layouts_; }

  // Shader object from translated code (DXIL or SPIR-V, entry point "main").
  nvrhi::ShaderHandle CreateShader(const ShaderEntry& entry) const;
  nvrhi::GraphicsPipelineHandle CreatePipeline(nvrhi::IShader* vs, nvrhi::IShader* ps, const nvrhi::FramebufferInfo& fb,
                                               const FixedState& state) const;

  // Slot in a table (0 stays the fallback). 0 = table full.
  uint32_t AddTexture(Table table, nvrhi::ITexture* texture);
  // With the view's component mapping (fetch swizzle) and view dimension.
  uint32_t AddTextureView(Table table, nvrhi::ITexture* texture, nvrhi::ComponentMapping mapping,
                          nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown);
  // Rewrite an occupied slot (same table).
  void WriteTextureView(Table table, uint32_t slot, nvrhi::ITexture* texture, nvrhi::ComponentMapping mapping,
                        nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown);
  uint32_t AddSampler(nvrhi::ISampler* sampler);
  uint32_t AddBuffer(nvrhi::IBuffer* buffer);
  uint32_t used(Table table) const { return next_[size_t(table)]; }
  uint32_t constant_versions() const { return capacities_.constant_versions; }

  // Write constants into the volatile buffers and set the bindings.
  void WriteConstants(nvrhi::ICommandList* cl, const void* vs_constants4k, const void* ps_constants4k,
                      const fcshaders::DrawConstants& draw) const;
  // Individually (the draw path writes only what changed since the last draw of the same command list).
  void WriteVertexConstants(nvrhi::ICommandList* cl, const void* constants4k) const { cl->writeBuffer(vs_constants_, constants4k, 4096); }
  void WritePixelConstants(nvrhi::ICommandList* cl, const void* constants4k) const { cl->writeBuffer(ps_constants_, constants4k, 4096); }
  void WriteDrawConstants(nvrhi::ICommandList* cl, const fcshaders::DrawConstants& d) const { cl->writeBuffer(draw_constants_, &d, sizeof(d)); }
  void WriteScaleConstants(nvrhi::ICommandList* cl, const fcshaders::ScaleConstants& s) const { cl->writeBuffer(scale_constants_, &s, sizeof(s)); }
  void Bind(nvrhi::GraphicsState& state) const;

  // NVRHI already flips Vulkan viewports to the D3D convention (+Y up); the translated VS are additionally built
  // with -fvk-invert-y. One of the two flips is undone via ndcScale/ndcOffset (as in King Kong,
  // game_renderer.cpp). Call after filling ndcScale/ndcOffset from the guest viewport.
  static void AdjustNdcForApi(nvrhi::GraphicsAPI api, fcshaders::DrawConstants& dc) {
    if (api == nvrhi::GraphicsAPI::VULKAN) dc.ndcScale[1] = -dc.ndcScale[1], dc.ndcOffset[1] = -dc.ndcOffset[1];
  }

 private:
  nvrhi::IDevice* device_ = nullptr;
  nvrhi::BindingLayoutVector layouts_;
  nvrhi::BindingSetHandle constants_set_;
  nvrhi::BufferHandle vs_constants_, ps_constants_, draw_constants_, scale_constants_;
  std::array<nvrhi::DescriptorTableHandle, size_t(Table::kCount)> tables_;
  std::array<uint32_t, size_t(Table::kCount)> next_{};
  TableCapacities capacities_;
  nvrhi::TextureHandle dummy_2d_, dummy_3d_, dummy_cube_, dummy_array_;
  nvrhi::SamplerHandle dummy_sampler_;
  nvrhi::BufferHandle dummy_buffer_;
};

}  // namespace fcp::nr
