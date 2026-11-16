// HDR state of the monitor a window is on, as DXGI and Windows report it ([display] hdr = auto, the log line
// "renderer: display HDR ..."). Read only; nothing is changed. Works the same for Vulkan and D3D12 (DXGI is only
// asked). Adapted from FCI src/app/output.cpp QueryHdr (Far Cry Instincts Recomp, MIT, same author).
#pragma once

#include <cstdint>
#include <string>

namespace fcp::nr {

struct DisplayHdrInfo {
  bool found = false;     // a DXGI output shows the window's monitor
  bool enabled = false;   // Windows HDR is on for it (colour space G2084 / P2020)
  float max_nits = 0, min_nits = 0, max_full_frame_nits = 0;
  float sdr_white_nits = 0;  // Windows' "SDR content brightness", 0 when unknown
  uint32_t bits = 0;
  std::string device;  // \\.\DISPLAYn
};

// hwnd: the game window (null: the primary monitor).
DisplayHdrInfo QueryDisplayHdr(void* hwnd);

}  // namespace fcp::nr
