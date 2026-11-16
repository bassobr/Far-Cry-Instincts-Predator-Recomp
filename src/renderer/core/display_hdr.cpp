// QueryDisplayHdr (core/display_hdr.h). Adapted from FCI src/app/output.cpp (Far Cry Instincts Recomp, MIT, same
// author): DXGI output of the monitor (IDXGIOutput6::GetDesc1), Windows' SDR white level (DisplayConfig).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cwchar>
#include <vector>

#include "display_hdr.h"

using Microsoft::WRL::ComPtr;

namespace fcp::nr {

DisplayHdrInfo QueryDisplayHdr(void* hwnd) {
  DisplayHdrInfo info;
  HMONITOR monitor = hwnd ? MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTOPRIMARY)
                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  ComPtr<IDXGIFactory1> factory;
  if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; !info.found && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
      ComPtr<IDXGIOutput> out;
      for (UINT i = 0; !info.found && adapter->EnumOutputs(i, &out) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput6> out6;
        DXGI_OUTPUT_DESC1 d{};
        if (FAILED(out.As(&out6)) || FAILED(out6->GetDesc1(&d)) || d.Monitor != monitor) continue;
        info.found = true;
        info.enabled = d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
        info.max_nits = d.MaxLuminance, info.min_nits = d.MinLuminance;
        info.max_full_frame_nits = d.MaxFullFrameLuminance;
        info.bits = d.BitsPerColor;
        for (const wchar_t* c = d.DeviceName; *c; ++c) info.device += *c < 128 ? char(*c) : '?';
      }
    }
  }
  // Windows' SDR content brightness (the "SDR content brightness" slider): 1000 is 80 nits.
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  UINT32 paths_n = 0, modes_n = 0;
  if (!GetMonitorInfoW(monitor, &mi) || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths_n, &modes_n) != ERROR_SUCCESS)
    return info;
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(paths_n);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modes_n);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths_n, paths.data(), &modes_n, modes.data(), nullptr) != ERROR_SUCCESS)
    return info;
  for (UINT32 i = 0; i < paths_n; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = paths[i].sourceInfo.adapterId;
    source.header.id = paths[i].sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || std::wcscmp(source.viewGdiDeviceName, mi.szDevice)) continue;
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    white.header.size = sizeof(white);
    white.header.adapterId = paths[i].targetInfo.adapterId;
    white.header.id = paths[i].targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) info.sdr_white_nits = float(white.SDRWhiteLevel) * 0.08f;
    break;
  }
  return info;
}

}  // namespace fcp::nr
