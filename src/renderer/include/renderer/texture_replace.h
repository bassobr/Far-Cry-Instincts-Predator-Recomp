// Runtime replacement of individual guest textures (button glyphs).
//
// Neutral interface: the renderer knows neither input devices nor button glyphs. It asks a source
// (src/fcp/button_prompts.cpp) whether a freshly converted texture is one that can be replaced, and on every
// binding of such a texture which image applies in its place now. The image is uploaded once and cached
// (key: PromptImage::id); placement and UVs of the glyphs stay the game's, only the resolution may be an
// integer multiple.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fcp::nr {

struct ReplacementImage {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;  // RGBA8, rows top first, no mip levels
  uint64_t id = 0;            // unique per content (redrawn = new ID)
};

class TextureReplaceSource {
 public:
  virtual ~TextureReplaceSource() = default;
  // Cheap pre-check before decompression: can a texture of this size qualify at all?
  virtual bool Wants(uint32_t width, uint32_t height) const = 0;
  // Render thread, under the renderer lock, once per newly converted (or rewritten) guest texture
  // with BC2 blocks (DXT3), one level, 2D. `blocks` = level 0 as converted (standard BC2), `rgba` = the same
  // level decompressed (RGBA8). Returns: the texture's kind for Current (>= 0) or -1 (not replaceable).
  virtual int Classify(uint32_t width, uint32_t height, const uint8_t* blocks, size_t block_bytes,
                       const uint8_t* rgba) = 0;
  // Render thread, on every binding of a classified texture: the image that applies in its place now (nullptr:
  // the game's). `scale` = factor of the screen targets (render scale). Must be cheap.
  virtual std::shared_ptr<const ReplacementImage> Current(int kind, float scale) = 0;
};

}  // namespace fcp::nr
