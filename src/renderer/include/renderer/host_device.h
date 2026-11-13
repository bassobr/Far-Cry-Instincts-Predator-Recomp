// HostDevice: NVRHI on its own Vulkan or D3D12 device, optionally with a swapchain for a window.
//
// Standalone (renderer_probe, tests), without ReXGlue. A second implementation on top of the SDK's provider
// (King Kong's vulkan_host.cpp / d3d12_host.cpp as a template) could follow with the same interface, should
// window and swapchain move to the ReXGlue presenter.
//
// Switches (read once at startup, diagnostics.h): FCP_GPU_DEBUG=1 (D3D12 debug layer or Vulkan validation,
// if installed, and NVRHI validation), FCP_GPU_WARP=1 (D3D12 on WARP).
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <nvrhi/nvrhi.h>

namespace fcp::nr {

class DriverPipelineCache;

enum class HostApi : uint8_t { kVulkan, kD3D12 };
const char* HostApiName(HostApi api);

struct HostOptions {
  HostApi api = HostApi::kVulkan;
  void* window = nullptr;          // HWND; null: no swapchain (offscreen only)
  uint32_t width = 0, height = 0;  // swapchain size
  bool vsync = true;
  bool debug = false;
  bool warp = false;               // D3D12 only
  // Vulkan: the driver pipeline cache as a file (kk/backend/vk_pipeline_cache.h); empty: none.
  std::string pipeline_cache_path;
  // Split present: WaitFrameSlot/AcquireImage run without the caller's lock before BeginFrame
  // (BeginFrame then no longer waits itself).
  bool split_present = false;
};

// Occlusion queries through the native handles (NVRHI has none). A fixed ring of slots; the caller
// allocates the slots, calls Begin/End on its open command list (Vulkan: outside a render pass; the caller
// ends it first with clearState) and reads only once the list has finished (event query).
class OcclusionQueries {
 public:
  virtual ~OcclusionQueries() = default;
  virtual uint32_t capacity() const = 0;
  virtual void Begin(nvrhi::ICommandList* cl, uint32_t index) = 0;
  // Ends the query and makes the result readable (D3D12: ResolveQueryData into the readback buffer).
  virtual void End(nvrhi::ICommandList* cl, uint32_t index) = 0;
  // Non-blocking: false if the result is not there yet.
  virtual bool Read(uint32_t index, uint64_t* samples) = 0;
};

class HostDevice {
 public:
  // Occlusion queries (null: not available).
  virtual OcclusionQueries* occlusion_queries() { return nullptr; }
  virtual ~HostDevice() = default;
  virtual nvrhi::IDevice* device() = 0;
  virtual HostApi api() const = 0;
  virtual std::string description() const = 0;  // GPU, driver
  virtual bool has_swapchain() const = 0;
  virtual nvrhi::Format back_buffer_format() const { return nvrhi::Format::RGBA8_UNORM; }
  // Highest MSAA level (1, 2, 4, 8) the device supports as a render target for all screen-target formats
  // (RGBA8, RGBA16F, RGB10A2, D32S8) ([graphics] msaa).
  virtual uint32_t max_samples() const { return 4; }
  // Next swapchain image (in state Present or Undefined; null without a swapchain).
  virtual nvrhi::ITexture* BeginFrame() = 0;
  // Transitions the image to the present state and shows it; waits for at most two frames of lead.
  virtual void Present() = 0;
  // Split present (HostOptions::split_present), only from the present thread and without the caller's lock:
  // waits until at most two presents are outstanding on the GPU, or acquires the next swapchain image (false: none,
  // skip the present). Neither touches NVRHI or the queue.
  virtual void WaitFrameSlot() {}
  virtual bool AcquireImage() { return true; }
  // CPU fence after everything submitted so far: SignalFence under the caller's lock (queue access), WaitFence
  // from any thread without the lock. 0: no fence (WaitFence then does not wait).
  virtual uint64_t SignalFence() { return 0; }
  virtual void WaitFence(uint64_t value) { (void)value; }
  virtual void WaitIdle() = 0;
  // Resize the swapchain to the new window size (waits for the GPU). False without a swapchain or on failure.
  virtual bool Resize(uint32_t width, uint32_t height) { (void)width, (void)height; return false; }
  virtual uint32_t swapchain_width() const { return 0; }
  virtual uint32_t swapchain_height() const { return 0; }
  virtual DriverPipelineCache* pipeline_cache() { return nullptr; }
  // Saves the driver pipeline cache (Vulkan) if one is active.
  virtual void SavePipelineCache() {}
};

std::unique_ptr<HostDevice> CreateVulkanHost(const HostOptions& options, std::string* error);
std::unique_ptr<HostDevice> CreateD3D12Host(const HostOptions& options, std::string* error);
std::unique_ptr<HostDevice> CreateHostDevice(const HostOptions& options, std::string* error);

// NVRHI messages to the log (stderr), errors counted (diagnostics.h: Counters::nvrhi_errors).
nvrhi::IMessageCallback* HostMessageCallback();

}  // namespace fcp::nr
