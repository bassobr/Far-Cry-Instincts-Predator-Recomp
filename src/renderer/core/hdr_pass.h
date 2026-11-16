// HDR10 output ([display] hdr): the last step of the present path when the swapchain is HDR10 (R10G10B10A2,
// SMPTE ST 2084 "PQ" over BT.2020). Internal to GameRenderer; all calls happen under the renderer lock on the present
// thread.
//
// The game renders SDR (8-bit, gamma encoded; its gamma ramp is applied in the blits before). With HDR10 the whole
// SDR path (gamma ramp, SMAA, centring with bars, later the colour blind matrix) writes into an SDR image of the
// swapchain's size instead of the swapchain; this pass then maps it per pixel:
//   SDR code value -> linear light (gamma `gamma`, 2.2 as an SDR PC monitor shows it) -> optional highlight stretch
//   (luminance above `knee` bends up to `gain` x at SDR white, colours keep their hue) -> BT.709 to BT.2020 primaries
//   -> x paper white (nits of SDR white) -> clamp to the peak -> PQ.
// The settings menu and the frame rate display are drawn afterwards straight into the HDR10 image (overlay_pass.cpp,
// ps_hdr: the same mapping without the stretch, so always at paper white).
//
// Origin: the mapping and its parameters (paper white, peak, boost with a knee at 0.5) follow FCI
// src/gpu/present.cpp (Far Cry Instincts Recomp, MIT, same author), which adapted them from SCDA-XBERecomp
// (MIT, Copyright (c) 2026 TGP482, Joshhhuaaa). PQ constants from SMPTE ST 2084, the primaries matrix from ITU-R
// BT.2087. The NVRHI implementation is this project's own code.
#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <nvrhi/nvrhi.h>

#include "renderer/host_device.h"

namespace fcp::nr {

struct HdrLevels {
  float paper_white = 200.0f;  // nits of SDR white (1.0)
  float peak = 1000.0f;        // nits, never below paper_white
  float gain = 1.0f;           // highlight stretch: luminance gain at SDR white (1 = off)
  float knee = 0.5f;           // the stretch starts above this linear luminance
  float gamma = 2.2f;          // SDR code value -> linear light
  uint32_t test_pattern = 0;   // FCP_HDR_TEST_PATTERN: eight bars of known SDR values instead of the picture
};

// HLSL shared by this pass and the overlay's HDR shader: Pq(nits / 10000), k709To2020.
extern const char* const kHdrCommonHlsl;

class HdrPass {
 public:
  // Shaders (DXC at runtime), layout. false: no HDR10 pass (error).
  bool Init(nvrhi::IDevice* device, HostApi api, std::string* error);
  bool ok() const { return ok_; }
  // src: the SDR picture (shader resource, the same size as dst), dst: framebuffer of the HDR10 image.
  bool Run(nvrhi::ICommandList* cl, nvrhi::ITexture* src, nvrhi::IFramebuffer* dst, const HdrLevels& levels);

 private:
  nvrhi::IGraphicsPipeline* Pipeline(nvrhi::IFramebuffer* fb);
  nvrhi::IBindingSet* Set(nvrhi::ITexture* src);

  bool ok_ = false;
  nvrhi::IDevice* dev_ = nullptr;
  nvrhi::ShaderHandle vs_, ps_;
  nvrhi::BindingLayoutHandle layout_;
  std::map<nvrhi::Format, nvrhi::GraphicsPipelineHandle> pipelines_;
  std::map<nvrhi::ITexture*, std::pair<nvrhi::TextureHandle, nvrhi::BindingSetHandle>> sets_;
};

// CPU reference of the shader's mapping (tests): SDR code values 0..1 -> PQ code values 0..1.
void HdrEncodeReference(const float sdr[3], const HdrLevels& levels, float pq[3]);
float PqFromNits(float nits);  // SMPTE ST 2084 inverse EOTF, 0..1
float NitsFromPq(float code);  // SMPTE ST 2084 EOTF

// Paper white, peak and gain from the settings and what the display reports ([display] hdr_paper_white,
// hdr_peak, hdr_boost; 0 = the display's / Windows' value, else 200 / 1000 nits).
HdrLevels HdrLevelsFor(float paper_white, float peak, float boost, float display_white, float display_peak, float gamma);

}  // namespace fcp::nr
