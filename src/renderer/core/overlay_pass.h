// Draws an OverlayFrame (renderer/overlay.h) with NVRHI into a framebuffer. Internal to GameRenderer; all
// calls happen under the renderer lock on the present thread. This project's own code.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

#include <nvrhi/nvrhi.h>

#include "renderer/host_device.h"
#include "renderer/overlay.h"

namespace fcp::nr {

class OverlayPass {
 public:
  // Shaders (DXC at runtime), sampler, binding layout. false: no overlay (error).
  bool Init(nvrhi::IDevice* device, HostApi api, std::string* error);
  bool ok() const { return ok_; }

  // Textures (RGBA8, rows top first). Returns: ID > 0, 0 on failure.
  uint64_t CreateTexture(nvrhi::ICommandList* cl, uint32_t width, uint32_t height, const uint8_t* rgba);
  void UpdateTexture(nvrhi::ICommandList* cl, uint64_t id, const uint8_t* rgba);
  void DestroyTexture(uint64_t id);

  // Draws into fb (color target 0, framebuffer size). Returns the number of draws.
  uint32_t Draw(nvrhi::ICommandList* cl, nvrhi::IFramebuffer* fb, const OverlayFrame& frame);

  size_t texture_count() const { return textures_.size(); }

 private:
  struct Tex {
    nvrhi::TextureHandle texture;
    nvrhi::BindingSetHandle set;
    uint32_t width = 0, height = 0;
  };
  bool EnsureBuffers(size_t vertices, size_t indices);
  nvrhi::IGraphicsPipeline* Pipeline(nvrhi::IFramebuffer* fb);

  bool ok_ = false;
  nvrhi::IDevice* dev_ = nullptr;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::InputLayoutHandle input_;
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::SamplerHandle sampler_;
  nvrhi::BufferHandle vb_, ib_;
  size_t vb_capacity_ = 0, ib_capacity_ = 0;
  std::map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> pipelines_;
  std::unordered_map<uint64_t, Tex> textures_;
  uint64_t next_id_ = 1;
};

}  // namespace fcp::nr
