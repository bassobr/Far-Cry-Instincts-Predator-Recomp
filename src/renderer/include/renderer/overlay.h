// Overlay interface of the renderer (settings menu, FPS display).
//
// A neutral copy of ImGui's draw data so the renderer does not depend on ImGui: the bridge
// (src/fcp/render_bridge.cpp) translates ImDrawData into these structures. The renderer draws them on the
// present thread between BeginPresent and EndPresent over the game image into the swapchain image (before present),
// at the full window size. This project's own code; structured like ImGui's own renderer backends (MIT).
#pragma once

#include <cstddef>
#include <cstdint>

namespace fcp::nr {

// Like ImDrawVert: position and UV in display pixels, color RGBA8 (red in the lowest byte).
struct OverlayVertex {
  float x, y, u, v;
  uint32_t rgba;
};
static_assert(sizeof(OverlayVertex) == 20, "OverlayVertex must match ImDrawVert");

// A draw command: indices [first_index, first_index + index_count) of the index array (already relative to the
// shared vertex array), scissor rectangle in display coordinates (x0, y0, x1, y1), texture from CreateOverlayTexture.
struct OverlayCommand {
  float clip[4];
  uint64_t texture;
  uint32_t first_index, index_count;
};

// Texture request (ImGui's texture list, 1.92): create (the renderer stores the new ID in `id`), re-upload
// completely, or destroy. Pixels RGBA8, rows top first, valid only during the call.
struct OverlayTextureOp {
  enum class Kind : uint8_t { kCreate, kUpdate, kDestroy };
  Kind kind = Kind::kCreate;
  uint64_t id = 0;
  uint32_t width = 0, height = 0;
  const uint8_t* rgba = nullptr;
  bool done = false;  // set by the renderer when executed (otherwise the request comes again next frame)
};

struct OverlayFrame {
  float display_pos[2] = {0, 0};
  float display_size[2] = {0, 0};
  const OverlayVertex* vertices = nullptr;
  size_t vertex_count = 0;
  const uint32_t* indices = nullptr;
  size_t index_count = 0;
  const OverlayCommand* commands = nullptr;
  size_t command_count = 0;
};

}  // namespace fcp::nr
