// Diagnostics built in from the start: switches, counters, frame dump, shader marking.
//
// Switches (prefix FCP_, read once at startup, never per draw; Switches::Get()):
//   FCP_RENDER_BACKEND=vulkan|d3d12   graphics API (default vulkan)
//   FCP_GPU_DEBUG=1                   D3D12 debug layer / Vulkan validation (if installed) + NVRHI validation
//   FCP_GPU_WARP=1                    D3D12 on WARP
//   FCP_RENDER_SOLID_PS=<hash>        draw draws using this pixel shader (container hash, 16 hex) in a solid color
//   FCP_RENDER_SOLID_VS=<hash>        the same for a vertex shader
//   FCP_RENDER_SOLID_COLOR=RRGGBBAA   signal color (default FF00FFFF, magenta)
//   FCP_DUMP_FRAMES=<file.bmp|png>    write the final image ("%d" in the name: frame number)
//   FCP_DUMP_FRAME_AT=<n>             only frame n (default: the last one the caller lets dump)
//   FCP_SHADER_PACK=<file>            disc pack; FCP_SHADER_CACHE=<dir> disk cache
//   FCP_SHADER_PACK_STALE_OK=1        use an outdated pack anyway
//   FCP_PIPELINE_CACHE=<file>         pipeline list (kk/backend/pipeline_cache.h)
//   FCP_DRIVER_PIPELINE_CACHE=<file>  driver pipeline cache (Vulkan)
//   FCP_TRACE_DRAWS=1                 log every call at the DrawSink (CountingSink)
//   FCP_STATS=1                       print counters on shutdown
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "renderer/draw_sink.h"

namespace nvrhi {
class IDevice;
class ITexture;
}  // namespace nvrhi

namespace fcp::nr {

struct Switches {
  std::string backend = "vulkan";
  bool gpu_debug = false;
  bool gpu_warp = false;
  uint64_t solid_ps = 0;   // 0 = off
  uint64_t solid_vs = 0;
  uint32_t solid_color = 0xFF00FFFFu;  // RRGGBBAA
  std::string dump_frames;
  int64_t dump_frame_at = -1;
  std::string shader_pack, shader_cache, pipeline_cache, driver_pipeline_cache;
  bool shader_pack_stale_ok = false;
  bool trace_draws = false;
  bool stats = false;
  // Reads the environment (on the first call only).
  static const Switches& Get();
  // For tests: from a function instead of the environment.
  static Switches Read(const char* (*getenv_fn)(const char*));
  // Whether a draw with these shaders is drawn in a solid color (shader marking).
  bool IsMarked(uint64_t vs_hash, uint64_t ps_hash) const {
    return (solid_vs && vs_hash == solid_vs) || (solid_ps && ps_hash == solid_ps);
  }
};

// Renderer counters (process-wide, atomic; readable at any time).
struct Counters {
  std::atomic<uint64_t> draws{0}, clears{0}, resolves{0}, tilings{0}, queries{0}, swaps{0}, shaders_created{0};
  std::atomic<uint64_t> draws_marked{0};             // drawn with shader marking
  std::atomic<uint64_t> draws_skipped{0};            // pipeline was still missing
  std::atomic<uint64_t> pipelines_created{0};        // newly compiled (driver)
  std::atomic<uint64_t> pipelines_cached{0};         // from our own in-memory pipeline cache
  std::atomic<uint64_t> pipeline_create_us{0};       // total creation time
  std::atomic<uint64_t> wait_us{0};                  // time spent waiting for the GPU (frame lead, readback)
  std::atomic<uint64_t> validation_errors{0};        // Vulkan validation errors
  std::atomic<uint64_t> nvrhi_errors{0};             // NVRHI messages with severity Error/Fatal
  std::atomic<bool> driver_cache_loaded{false};
  static Counters& Get();
  void AddWait(std::chrono::steady_clock::time_point since);
  std::string Summary() const;
};

// Decorator: counts every call and logs it with FCP_TRACE_DRAWS (stderr).
class CountingSink final : public ForwardingSink {
 public:
  explicit CountingSink(DrawSink& inner, bool trace = Switches::Get().trace_draws) : ForwardingSink(inner), trace_(trace) {}
  void OnDraw(const DrawRecord& r) override;
  void OnClear(const ClearRecord& r) override;
  void OnResolve(const ResolveRecord& r) override;
  void OnTiling(const TilingRecord& r) override;
  uint32_t OnQuery(const QueryRecord& r, uint32_t* samples) override;
  void OnShaderCreated(const ShaderCreatedRecord& r) override;
  void OnSwap(const SwapRecord& r) override;

 private:
  bool trace_;
};

// Renderer log: one line per call to the sink (in the game: the log thread), otherwise stderr.
void SetLogSink(void (*sink)(const char* line));
void LogLine(const char* format, ...);

// Image as 24-bit BMP or PNG (extension decides) from RGBA8 rows (top first).
bool WriteImage(const std::string& path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba,
                std::string* error = nullptr);
// Reads back a texture (RGBA8/BGRA8, mip 0) through a staging texture as RGBA8 (waits for the GPU).
bool ReadbackRgba8(nvrhi::IDevice* device, nvrhi::ITexture* texture, std::vector<uint8_t>& rgba, std::string* error = nullptr);
// Replace "%d" in the dump name with the frame number.
std::string DumpPath(const std::string& pattern, uint64_t frame);

}  // namespace fcp::nr
