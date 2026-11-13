// SMAA 1x over a finished image ([graphics] smaa). Internal to GameRenderer; all calls happen under the
// renderer lock on the present thread.
//
// Three passes as in SMAA (github.com/iryoku/smaa, MIT; shaders and tables in src/renderer/smaa/, taken
// from FCI): edges from luma, blend weights from edges and the two tables, then the blended image.
// The flow (configuration, entry points, tables) comes from FCI src/gpu/present.cpp (MIT, same author;
// there after SCDA-XBERecomp, MIT); the NVRHI implementation is this project's own code.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>

#include <nvrhi/nvrhi.h>

#include "renderer/host_device.h"

namespace fcp::nr {

class SmaaPass {
 public:
  // Shaders (DXC at runtime), samplers, layout. false: no SMAA (error).
  bool Init(nvrhi::IDevice* device, HostApi api, std::string* error);
  bool ok() const { return ok_; }

  // Reads src (shader resource, w x h from its description) and writes the anti-aliased image to dst (same
  // size, render target). The first call uploads the tables (on cl).
  bool Run(nvrhi::ICommandList* cl, nvrhi::ITexture* src, nvrhi::ITexture* dst);

 private:
  bool EnsureTargets(uint32_t w, uint32_t h);
  nvrhi::IGraphicsPipeline* Pipeline(int pass, nvrhi::IFramebuffer* fb);
  nvrhi::IBindingSet* Set(nvrhi::ITexture* t0, nvrhi::ITexture* t1, nvrhi::ITexture* t2);
  void Pass(nvrhi::ICommandList* cl, int pass, nvrhi::IFramebuffer* fb, nvrhi::IBindingSet* set, const float metrics[4]);

  bool ok_ = false, uploaded_ = false;
  nvrhi::IDevice* dev_ = nullptr;
  nvrhi::ShaderHandle vs_[3], ps_[3];
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::SamplerHandle linear_, point_;
  nvrhi::TextureHandle area_, search_, edges_, weights_;
  nvrhi::FramebufferHandle edges_fb_, weights_fb_;
  uint32_t w_ = 0, h_ = 0;
  std::map<std::pair<int, nvrhi::Format>, nvrhi::GraphicsPipelineHandle> pipelines_;
  // Binding sets per (t0, t1, t2); holds the textures so a pointer never refers to a new texture.
  struct SetEntry {
    nvrhi::TextureHandle keep[3];
    nvrhi::BindingSetHandle set;
  };
  std::map<std::array<nvrhi::ITexture*, 3>, SetEntry> sets_;
  std::map<nvrhi::ITexture*, std::pair<nvrhi::TextureHandle, nvrhi::FramebufferHandle>> dst_fbs_;
};

}  // namespace fcp::nr
