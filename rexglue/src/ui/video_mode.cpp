#include "ui/video_mode.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <rex/title.h>
#include "ui/resolution_preset.h"
#include "ui/ui_system.h"

namespace rex {

namespace ui::detail {

bool ResolveConfiguredSize(const UISettings& ui, i32& width_out, i32& height_out) {
  if (TryParseResolutionPreset(ui.resolution.Get(), width_out, height_out)) {
    return true;
  }
  const i32 window_width = ui.window_width.Get();
  const i32 window_height = ui.window_height.Get();
  if (window_width > 0 && window_height > 0) {
    width_out = window_width;
    height_out = window_height;
    return true;
  }
  width_out = ui.video_mode_width.Get();
  height_out = ui.video_mode_height.Get();
  return width_out > 0 && height_out > 0;
}

void ResolveModeSize(const UISettings& ui, u32& width_out, u32& height_out) {
  i32 width = 0;
  i32 height = 0;
  width_out = 0;
  height_out = 0;
  if (TryParseResolutionPreset(ui.resolution.Get(), width, height) && width > 0 && height > 0) {
    width_out = static_cast<u32>(width);
    height_out = static_cast<u32>(height);
  }
}

void ResolveFullscreenMode(const UISettings& ui, u32& width_out, u32& height_out, f32& refresh_out) {
  refresh_out = 0.0f;
  const std::string text = ui.fullscreen_mode.Get();
  unsigned width = 0;
  unsigned height = 0;
  float refresh = 0.0f;
  char tail = 0;
  // "WxH" or "WxH@Hz"; anything else (empty, "desktop") falls back to ui.resolution / the desktop's mode
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const int fields = std::sscanf(text.c_str(), "%ux%u@%f%c", &width, &height, &refresh, &tail);
#pragma clang diagnostic pop
  if ((fields == 2 || fields == 3) && width >= 320 && height >= 200 && width <= 16384 && height <= 16384) {
    width_out = width;
    height_out = height;
    refresh_out = fields == 3 && refresh > 0.0f && refresh < 1000.0f ? refresh : 0.0f;
    return;
  }
  ResolveModeSize(ui, width_out, height_out);
}

AspectRatio ParseAspectRatio(std::string_view text) {
  if (text == "4:3") {
    return AspectRatio::k4x3;
  }
  if (text == "5:4") {
    return AspectRatio::k5x4;
  }
  if (text == "3:2") {
    return AspectRatio::k3x2;
  }
  if (text == "16:10") {
    return AspectRatio::k16x10;
  }
  if (text == "16:9") {
    return AspectRatio::k16x9;
  }
  if (text == "21:9") {
    return AspectRatio::k21x9;
  }
  if (text == "32:9") {
    return AspectRatio::k32x9;
  }
  return AspectRatio::kStretch;
}

}  // namespace ui::detail

VideoMode ui::UISystem::GetVideoMode() const {
  i32 width = settings_.video_mode_width.Get();
  i32 height = settings_.video_mode_height.Get();
  if (settings_.video_mode_width.GetLayer() == SettingLayer::kDefault &&
      settings_.video_mode_height.GetLayer() == SettingLayer::kDefault) {
    ui::detail::ResolveConfiguredSize(settings_, width, height);
  }
  VideoMode mode;
  mode.width = static_cast<u32>(std::clamp(width, 640, 0x0FFF));
  mode.height = static_cast<u32>(std::clamp(height, 480, 0x0FFF));
  mode.refresh_rate =
      static_cast<f32>(std::clamp(settings_.video_mode_refresh_rate.Get(), 24.0, 240.0));
  return mode;
}

}  // namespace rex
