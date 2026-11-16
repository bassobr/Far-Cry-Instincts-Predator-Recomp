// Screen space ambient occlusion over the opaque 3D scene ([graphics] ao = off | ssao | gtao). Internal to
// GameRenderer; all calls happen under the renderer lock on the game's render thread, into the command list the
// scene's draws go to (also the deferred list: only states, push constants and draws).
//
// GameRenderer decides where it goes in (game_renderer.cpp, "Ambient occlusion"): once per frame, after the opaque
// part of the 3D view and before the first blended draw, resolve or clear of that target. This pass reads the scene's
// depth (multisampled or not), makes view depth from the game camera's projection (z / w of PerspectiveFovRH, undone
// with the viewport's z mapping), computes the occlusion at half or full resolution, blurs it depth aware (two
// separable 5 tap passes) and multiplies it into the scene's colour (alpha untouched), per sample on MSAA targets.
// Sky (cleared depth), the first person view (closer than 5 cm) and far surfaces (fade 35..90 m) get none.
//
// The kernels (view reconstruction, normal from the nearer neighbours, 16 tap SSAO, GTAO at XeGTAO's "high"
// preset, depth aware blur and upsampling) are adapted from Far Cry Instincts Recomp (MIT, same author,
// src/gpu/ao.cpp), which adapted them from SCDA-XBERecomp (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa). GTAO after
// Jimenez et al. 2016 / Intel XeGTAO (MIT). Changed here: projected depth (z / w) instead of FCI's W-buffer, NVRHI
// (Vulkan and Direct3D 12) instead of Direct3D 11, per-sample application on multisampled targets.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>

#include <nvrhi/nvrhi.h>

#include "renderer/host_device.h"

namespace fcp::nr {

class AoPass {
 public:
  // Shaders (DXC at runtime), layout. false: no AO (error).
  bool Init(nvrhi::IDevice* device, HostApi api, std::string* error);
  bool ok() const { return ok_; }

  struct Params {
    int mode = 0;                       // 1 SSAO, 2 GTAO
    nvrhi::ITexture* depth = nullptr;   // the scene's depth (D32S8), multisampled when samples > 1
    nvrhi::ITexture* color = nullptr;   // the scene's colour (render target, same size and samples)
    uint32_t samples = 1;
    uint32_t width = 0, height = 0;     // host size of both
    int32_t left = 0, top = 0, right = 0, bottom = 0;  // the 3D view (viewport: ndc -1..1), host pixels, clipped
    float view_x = 0, view_y = 0, view_w = 0, view_h = 0;  // the viewport unclipped (ndc), host pixels
    float x_scale = 1, y_scale = 1;     // projection: view x = ndc x * depth / x_scale (P00, P11)
    float k0 = 0, k1 = 0;               // view depth = k0 / (stored depth + k1)
    float far_plane = 1000;             // view depth at or beyond which a pixel is sky
    float strength = 1;                 // exponent on the occlusion (0: none)
    bool half = false;                  // occlusion at half resolution
    int debug = 0;                      // FCP_AO_DEBUG: 1 the occlusion instead of the scene, 2 view depth, 3 right half only
  };
  // Draws the occlusion into p.color. Leaves the GraphicsState to the caller to set again.
  bool Run(nvrhi::ICommandList* cl, const Params& p);
  uint32_t target_w() const { return w_; }
  uint32_t target_h() const { return h_; }

 private:
  enum Ps { kLinearize, kLinearizeMs, kSsao, kGtao, kBlur, kApply, kApplyMs, kDepthView, kDepthViewMs, kPsCount };
  bool EnsureTargets(uint32_t w, uint32_t h);
  nvrhi::IGraphicsPipeline* Pipeline(Ps ps, nvrhi::IFramebuffer* fb, bool multiply);
  nvrhi::IBindingSet* Set(nvrhi::ITexture* t0, nvrhi::ITexture* t1, nvrhi::ITexture* t2);
  void Pass(nvrhi::ICommandList* cl, Ps ps, nvrhi::IFramebuffer* fb, bool multiply, nvrhi::IBindingSet* set,
            const nvrhi::Rect& scissor, uint32_t vw, uint32_t vh, const void* constants);

  bool ok_ = false;
  nvrhi::IDevice* dev_ = nullptr;
  nvrhi::ShaderHandle vs_, ps_[kPsCount];
  nvrhi::BindingLayoutHandle layout_;
  nvrhi::TextureHandle lin_, ao_[2], dummy_, dummy_ms_;
  nvrhi::FramebufferHandle lin_fb_, ao_fb_[2];
  uint32_t w_ = 0, h_ = 0;
  std::map<std::tuple<int, nvrhi::Format, uint32_t, bool>, nvrhi::GraphicsPipelineHandle> pipelines_;
  struct SetEntry {
    nvrhi::TextureHandle keep[3];
    nvrhi::BindingSetHandle set;
  };
  std::map<std::tuple<nvrhi::ITexture*, nvrhi::ITexture*, nvrhi::ITexture*>, SetEntry> sets_;
  std::map<nvrhi::ITexture*, std::pair<nvrhi::TextureHandle, nvrhi::FramebufferHandle>> color_fbs_;
};

}  // namespace fcp::nr
