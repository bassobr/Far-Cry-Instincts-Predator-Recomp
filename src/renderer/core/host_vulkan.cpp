// Standalone Vulkan device with a Win32 swapchain for NVRHI (renderer/host_device.h). This project's own
// code; the acquire/present flow follows NVRHI's samples (Donut DeviceManager_VK, MIT, not copied).
// The loader is the graphics driver's vulkan-1.dll; no Vulkan SDK is needed.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.hpp>

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "kk/backend/vk_pipeline_cache.h"
#include "kk/backend/vulkan_dispatch.h"
#include "renderer/diagnostics.h"
#include "renderer/host_device.h"

namespace fcp::nr {

namespace {

bool HasExtension(const std::vector<vk::ExtensionProperties>& list, const char* name) {
  for (const auto& e : list)
    if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    LogLine("[vulkan] %s", data->pMessage);
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) Counters::Get().validation_errors++;
  }
  return VK_FALSE;
}

// Occlusion queries: one VkQueryPool (type OCCLUSION, precise if the device supports it).
class VulkanOcclusion final : public OcclusionQueries {
 public:
  VulkanOcclusion(vk::Device dev, bool precise, uint32_t count) : dev_(dev), precise_(precise), count_(count) {
    vk::QueryPoolCreateInfo ci({}, vk::QueryType::eOcclusion, count);
    pool_ = dev_.createQueryPool(ci);
  }
  ~VulkanOcclusion() override {
    if (pool_) dev_.destroyQueryPool(pool_);
  }
  uint32_t capacity() const override { return count_; }
  void Begin(nvrhi::ICommandList* cl, uint32_t index) override {
    vk::CommandBuffer cb(VkCommandBuffer(cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer));
    if (!cb) return;
    cb.resetQueryPool(pool_, index, 1);
    cb.beginQuery(pool_, index, precise_ ? vk::QueryControlFlagBits::ePrecise : vk::QueryControlFlags());
  }
  void End(nvrhi::ICommandList* cl, uint32_t index) override {
    vk::CommandBuffer cb(VkCommandBuffer(cl->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer).pointer));
    if (cb) cb.endQuery(pool_, index);
  }
  bool Read(uint32_t index, uint64_t* samples) override {
    uint64_t v = 0;
    const vk::Result r = dev_.getQueryPoolResults(pool_, index, 1, sizeof(v), &v, sizeof(v), vk::QueryResultFlagBits::e64);
    if (r != vk::Result::eSuccess) return false;
    *samples = v;
    return true;
  }
  bool ok() const { return bool(pool_); }

 private:
  vk::Device dev_;
  vk::QueryPool pool_;
  bool precise_;
  uint32_t count_;
};

class VulkanHost final : public HostDevice {
 public:
  ~VulkanHost() override {
    if (dev_) dev_.waitIdle();
    occlusion_.reset();
    images_.clear();
    frame_queries_.clear();
    barrier_list_ = nullptr;
    nvrhi_ = nullptr;
    validation_ = nullptr;
    if (dev_) {
      for (auto s : acquire_) dev_.destroySemaphore(s);
      for (auto s : present_) dev_.destroySemaphore(s);
      if (timeline_) dev_.destroySemaphore(timeline_);
      if (swapchain_) dev_.destroySwapchainKHR(swapchain_);
    }
    pipeline_cache_.reset();
    if (dev_) dev_.destroy();
    if (instance_) {
      if (surface_) instance_.destroySurfaceKHR(surface_);
      if (messenger_) instance_.destroyDebugUtilsMessengerEXT(messenger_);
      instance_.destroy();
    }
    if (library_) FreeLibrary(library_);
  }

  bool Init(const HostOptions& o, std::string* error) {
    options_ = o;
    try {
      return InitInner(error);
    } catch (const std::exception& e) {
      if (error) *error = std::string("Vulkan: ") + e.what();
      return false;
    }
  }

  nvrhi::IDevice* device() override { return validation_ ? validation_.Get() : nvrhi_.Get(); }
  OcclusionQueries* occlusion_queries() override { return occlusion_.get(); }
  HostApi api() const override { return HostApi::kVulkan; }
  std::string description() const override { return description_; }
  bool has_swapchain() const override { return bool(swapchain_); }
  nvrhi::Format back_buffer_format() const override { return back_format_; }
  uint32_t max_samples() const override { return max_samples_; }
  DriverPipelineCache* pipeline_cache() override { return pipeline_cache_.get(); }
  void SavePipelineCache() override {
    if (!pipeline_cache_ || options_.pipeline_cache_path.empty()) return;
    size_t bytes = 0;
    std::string note;
    pipeline_cache_->SaveFile(options_.pipeline_cache_path, &bytes, &note);
  }

  // Split present: fence per present (own timeline semaphore), waiting and acquire without the lock.
  void WaitFrameSlot() override {
    if (!swapchain_ || frame_ < frame_fences_.size()) return;
    const uint64_t v = frame_fences_[frame_ % frame_fences_.size()];
    if (!v || TimelineValue() >= v) return;
    const auto t0 = std::chrono::steady_clock::now();
    WaitFence(v);
    Counters::Get().AddWait(t0);
  }
  bool AcquireImage() override {
    if (!swapchain_) return false;
    const vk::Semaphore s = acquire_[frame_ % acquire_.size()];
    const vk::Result r = dev_.acquireNextImageKHR(swapchain_, UINT64_MAX, s, nullptr, &index_);
    acquired_ = r == vk::Result::eSuccess || r == vk::Result::eSuboptimalKHR;
    return acquired_;
  }
  uint64_t SignalFence() override {
    if (!timeline_) return 0;
    const uint64_t v = ++timeline_next_;
    vk::TimelineSemaphoreSubmitInfo ti;
    ti.signalSemaphoreValueCount = 1;
    ti.pSignalSemaphoreValues = &v;
    vk::SubmitInfo si;
    si.pNext = &ti;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &timeline_;
    if (queue_.submit(1, &si, nullptr) != vk::Result::eSuccess) return 0;
    return v;
  }
  void WaitFence(uint64_t value) override {
    if (!timeline_ || !value) return;
    vk::SemaphoreWaitInfo wi;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    (void)dev_.waitSemaphores(&wi, UINT64_MAX);
  }

  nvrhi::ITexture* BeginFrame() override {
    if (!swapchain_) return nullptr;
    if (options_.split_present) {
      // WaitFrameSlot/AcquireImage already ran without the lock.
      if (!acquired_) return nullptr;
      acquired_ = false;
      nvrhi_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, acquire_[frame_ % acquire_.size()], 0);
      return images_[index_];
    }
    // At most two frames of lead: wait for the submission before last.
    nvrhi::IEventQuery* q = frame_queries_[frame_ % frame_queries_.size()];
    if (frame_ >= frame_queries_.size()) {
      const auto t0 = std::chrono::steady_clock::now();
      device()->waitEventQuery(q);
      Counters::Get().AddWait(t0);
    }
    const vk::Semaphore s = acquire_[frame_ % acquire_.size()];
    const vk::Result r = dev_.acquireNextImageKHR(swapchain_, UINT64_MAX, s, nullptr, &index_);
    if (r != vk::Result::eSuccess && r != vk::Result::eSuboptimalKHR) return nullptr;
    nvrhi_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, s, 0);
    return images_[index_];
  }

  void Present() override {
    if (!swapchain_) return;
    barrier_list_->open();
    barrier_list_->setTextureState(images_[index_], nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
    barrier_list_->commitBarriers();
    barrier_list_->close();
    nvrhi_->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, present_[index_], 0);
    device()->executeCommandList(barrier_list_);
    if (options_.split_present) {
      frame_fences_[frame_ % frame_fences_.size()] = SignalFence();
    } else {
      nvrhi::IEventQuery* q = frame_queries_[frame_ % frame_queries_.size()];
      device()->resetEventQuery(q);
      device()->setEventQuery(q, nvrhi::CommandQueue::Graphics);
    }
    vk::PresentInfoKHR info;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &present_[index_];
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain_;
    info.pImageIndices = &index_;
    (void)queue_.presentKHR(&info);
    device()->runGarbageCollection();
    ++frame_;
  }

  void WaitIdle() override {
    if (device()) device()->waitForIdle();
  }

  uint32_t swapchain_width() const override { return width_; }
  uint32_t swapchain_height() const override { return height_; }

  bool Resize(uint32_t width, uint32_t height) override {
    if (!swapchain_ || !width || !height) return false;
    device()->waitForIdle();
    images_.clear();
    for (auto s : acquire_) dev_.destroySemaphore(s);
    for (auto s : present_) dev_.destroySemaphore(s);
    acquire_.clear();
    present_.clear();
    vk::SwapchainKHR old = swapchain_;
    swapchain_ = nullptr;
    options_.width = width;
    options_.height = height;
    std::string error;
    bool ok = false;
    try {
      ok = CreateSwapchain(&error, old);
    } catch (const std::exception& e) {
      error = e.what();
    }
    dev_.destroySwapchainKHR(old);
    if (!ok) LogLine("[vulkan] Swapchain %ux%u: %s", width, height, error.c_str());
    return ok;
  }

 private:
  bool InitInner(std::string* error) {
    library_ = LoadLibraryW(L"vulkan-1.dll");
    if (!library_) return error && (*error = "vulkan-1.dll missing (graphics driver without Vulkan)", false);
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(library_, "vkGetInstanceProcAddr"));
    if (!gipa) return error && (*error = "vkGetInstanceProcAddr missing", false);
    InitVulkanDispatch(gipa, VK_NULL_HANDLE, VK_NULL_HANDLE);

    const auto instance_exts = vk::enumerateInstanceExtensionProperties();
    const auto layers = vk::enumerateInstanceLayerProperties();
    std::vector<const char*> exts;
    if (options_.window) exts.push_back(VK_KHR_SURFACE_EXTENSION_NAME), exts.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    std::vector<const char*> enabled_layers;
    bool debug_utils = false;
    if (options_.debug) {
      if (HasExtension(instance_exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debug_utils = true;
      }
      for (const auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
      if (enabled_layers.empty()) LogLine("[vulkan] FCP_GPU_DEBUG: no validation layer installed, NVRHI validation only");
    }
    vk::ApplicationInfo app("predator_renderer", 1, "fcp", 1, VK_API_VERSION_1_3);
    vk::InstanceCreateInfo ici({}, &app, uint32_t(enabled_layers.size()), enabled_layers.data(), uint32_t(exts.size()), exts.data());
    instance_ = vk::createInstance(ici);
    InitVulkanDispatch(gipa, instance_, VK_NULL_HANDLE);
    if (debug_utils) {
      vk::DebugUtilsMessengerCreateInfoEXT mi;
      mi.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
      mi.messageType = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
                       vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
      mi.pfnUserCallback = reinterpret_cast<vk::PFN_DebugUtilsMessengerCallbackEXT>(&DebugCallback);
      messenger_ = instance_.createDebugUtilsMessengerEXT(mi);
    }
    if (options_.window) {
      vk::Win32SurfaceCreateInfoKHR si({}, GetModuleHandleW(nullptr), static_cast<HWND>(options_.window));
      surface_ = instance_.createWin32SurfaceKHR(si);
    }

    // Device: discrete before integrated, Vulkan 1.3, one graphics queue (with present if there is a window).
    int best = -1;
    uint32_t best_family = 0;
    const auto devices = instance_.enumeratePhysicalDevices();
    for (size_t i = 0; i < devices.size(); ++i) {
      const auto props = devices[i].getProperties();
      if (props.apiVersion < VK_API_VERSION_1_3) continue;
      const auto families = devices[i].getQueueFamilyProperties();
      for (uint32_t f = 0; f < families.size(); ++f) {
        if (!(families[f].queueFlags & vk::QueueFlagBits::eGraphics)) continue;
        if (surface_ && !devices[i].getSurfaceSupportKHR(f, surface_)) continue;
        const bool discrete = props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
        if (best < 0 || (discrete && devices[best].getProperties().deviceType != vk::PhysicalDeviceType::eDiscreteGpu)) {
          best = int(i);
          best_family = f;
        }
        break;
      }
    }
    if (best < 0) return error && (*error = "no Vulkan 1.3 device with a graphics queue", false);
    phys_ = devices[best];
    family_ = best_family;
    const auto props = phys_.getProperties();
    char driver[64];
    std::snprintf(driver, sizeof(driver), "%u.%u.%u", VK_VERSION_MAJOR(props.driverVersion),
                  VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
    description_ = std::string("Vulkan ") + props.deviceName.data() + ", driver " + driver;
    {
      // MSAA: color and depth/stencil together (framebuffer*SampleCounts apply to all formats with optimal tiling).
      const vk::SampleCountFlags both = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                                        props.limits.framebufferStencilSampleCounts;
      max_samples_ = 1;
      for (uint32_t s : {2u, 4u, 8u})
        if (both & vk::SampleCountFlags(vk::SampleCountFlagBits(s))) max_samples_ = s;
    }

    // Features: what the translated shaders (bindless, SPIR-V 1.5) and NVRHI (Vulkan 1.3) need.
    vk::PhysicalDeviceFeatures2 have2;
    vk::PhysicalDeviceVulkan12Features have12;
    vk::PhysicalDeviceVulkan13Features have13;
    have2.pNext = &have12;
    have12.pNext = &have13;
    phys_.getFeatures2(&have2);
    std::string missing;
    auto need = [&](vk::Bool32 v, const char* name) {
      if (!v) missing += std::string(missing.empty() ? "" : ", ") + name;
      return v;
    };
    vk::PhysicalDeviceFeatures f10;
    f10.shaderSampledImageArrayDynamicIndexing = need(have2.features.shaderSampledImageArrayDynamicIndexing, "shaderSampledImageArrayDynamicIndexing");
    f10.shaderStorageBufferArrayDynamicIndexing = need(have2.features.shaderStorageBufferArrayDynamicIndexing, "shaderStorageBufferArrayDynamicIndexing");
    f10.shaderClipDistance = need(have2.features.shaderClipDistance, "shaderClipDistance");
    f10.independentBlend = have2.features.independentBlend;
    f10.samplerAnisotropy = have2.features.samplerAnisotropy;
    f10.depthClamp = have2.features.depthClamp;
    f10.fillModeNonSolid = have2.features.fillModeNonSolid;
    f10.textureCompressionBC = need(have2.features.textureCompressionBC, "textureCompressionBC");
    f10.depthBiasClamp = have2.features.depthBiasClamp;
    f10.occlusionQueryPrecise = have2.features.occlusionQueryPrecise;  // real counts instead of just 0/≠0
    vk::PhysicalDeviceVulkan12Features f12;
    f12.descriptorIndexing = need(have12.descriptorIndexing, "descriptorIndexing");
    f12.shaderSampledImageArrayNonUniformIndexing = need(have12.shaderSampledImageArrayNonUniformIndexing, "shaderSampledImageArrayNonUniformIndexing");
    f12.shaderStorageBufferArrayNonUniformIndexing = need(have12.shaderStorageBufferArrayNonUniformIndexing, "shaderStorageBufferArrayNonUniformIndexing");
    f12.descriptorBindingSampledImageUpdateAfterBind = need(have12.descriptorBindingSampledImageUpdateAfterBind, "descriptorBindingSampledImageUpdateAfterBind");
    f12.descriptorBindingStorageBufferUpdateAfterBind = need(have12.descriptorBindingStorageBufferUpdateAfterBind, "descriptorBindingStorageBufferUpdateAfterBind");
    f12.descriptorBindingUpdateUnusedWhilePending = need(have12.descriptorBindingUpdateUnusedWhilePending, "descriptorBindingUpdateUnusedWhilePending");
    f12.descriptorBindingPartiallyBound = need(have12.descriptorBindingPartiallyBound, "descriptorBindingPartiallyBound");
    f12.descriptorBindingVariableDescriptorCount = have12.descriptorBindingVariableDescriptorCount;
    f12.runtimeDescriptorArray = need(have12.runtimeDescriptorArray, "runtimeDescriptorArray");
    f12.timelineSemaphore = need(have12.timelineSemaphore, "timelineSemaphore");
    f12.bufferDeviceAddress = have12.bufferDeviceAddress;
    f12.shaderFloat16 = have12.shaderFloat16;
    f12.descriptorBindingUniformBufferUpdateAfterBind = have12.descriptorBindingUniformBufferUpdateAfterBind;
    vk::PhysicalDeviceVulkan13Features f13;
    f13.dynamicRendering = need(have13.dynamicRendering, "dynamicRendering");
    f13.synchronization2 = need(have13.synchronization2, "synchronization2");
    f13.maintenance4 = have13.maintenance4;
    if (!missing.empty()) return error && (*error = "Vulkan features missing: " + missing, false);
    vk::PhysicalDeviceFeatures2 enable2;
    enable2.features = f10;
    enable2.pNext = &f12;
    f12.pNext = &f13;

    const auto device_exts = phys_.enumerateDeviceExtensionProperties();
    std::vector<const char*> dexts;
    if (surface_) dexts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const float priority = 1.0f;
    vk::DeviceQueueCreateInfo qci({}, family_, 1, &priority);
    vk::DeviceCreateInfo dci({}, 1, &qci, 0, nullptr, uint32_t(dexts.size()), dexts.data());
    dci.pNext = &enable2;
    dev_ = phys_.createDevice(dci);
    InitVulkanDispatch(gipa, instance_, dev_);
    queue_ = dev_.getQueue(family_, 0);

    nvrhi::vulkan::DeviceDesc desc;
    desc.errorCB = HostMessageCallback();
    desc.instance = instance_;
    desc.physicalDevice = phys_;
    desc.device = dev_;
    desc.graphicsQueue = queue_;
    desc.graphicsQueueIndex = int(family_);
    desc.instanceExtensions = exts.data();
    desc.numInstanceExtensions = exts.size();
    desc.deviceExtensions = dexts.data();
    desc.numDeviceExtensions = dexts.size();
    desc.bufferDeviceAddressSupported = f12.bufferDeviceAddress;
    desc.descriptorBindingUniformBufferUpdateAfterBind = f12.descriptorBindingUniformBufferUpdateAfterBind;
    // The driver cache as a file (King Kong's vk_pipeline_cache, via the KK patch DeviceDesc::pipelineCache).
    pipeline_cache_ = VulkanPipelineCache::Create(phys_, dev_);
    if (pipeline_cache_) {
      desc.pipelineCache = pipeline_cache_->handle();
      if (!options_.pipeline_cache_path.empty()) {
        std::string note;
        const auto load = pipeline_cache_->LoadFile(options_.pipeline_cache_path, &note);
        Counters::Get().driver_cache_loaded = load == DriverPipelineCache::Load::kLoaded;
      }
    }
    nvrhi_ = nvrhi::vulkan::createDevice(desc);
    if (!nvrhi_) return error && (*error = "nvrhi::vulkan::createDevice failed", false);
    if (options_.debug) validation_ = nvrhi::validation::createValidationLayer(nvrhi_);
    barrier_list_ = device()->createCommandList();
    for (int i = 0; i < 2; ++i) frame_queries_.push_back(device()->createEventQuery());
    {
      vk::SemaphoreTypeCreateInfo ti(vk::SemaphoreType::eTimeline, 0);
      vk::SemaphoreCreateInfo ci;
      ci.pNext = &ti;
      timeline_ = dev_.createSemaphore(ci);
    }
    try {
      occlusion_ = std::make_unique<VulkanOcclusion>(dev_, bool(f10.occlusionQueryPrecise), 4096);
    } catch (const std::exception& e) {
      LogLine("[vulkan] query pool: %s (no occlusion queries)", e.what());
      occlusion_.reset();
    }
    return !surface_ || CreateSwapchain(error);
  }

  bool CreateSwapchain(std::string* error, vk::SwapchainKHR old = nullptr) {
    const auto caps = phys_.getSurfaceCapabilitiesKHR(surface_);
    const auto formats = phys_.getSurfaceFormatsKHR(surface_);
    vk::SurfaceFormatKHR chosen = formats.front();
    for (const auto& f : formats)
      if (f.format == vk::Format::eB8G8R8A8Unorm || f.format == vk::Format::eR8G8B8A8Unorm) {
        chosen = f;
        break;
      }
    back_format_ = chosen.format == vk::Format::eB8G8R8A8Unorm ? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
    vk::Extent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = vk::Extent2D(options_.width, options_.height);
    width_ = extent.width, height_ = extent.height;
    uint32_t count = std::max(caps.minImageCount, 2u);
    if (caps.maxImageCount) count = std::min(count + 1, caps.maxImageCount);
    vk::SwapchainCreateInfoKHR sci;
    sci.surface = surface_;
    sci.minImageCount = count;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                     vk::ImageUsageFlagBits::eTransferSrc;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    // Without VSync, Mailbox first (does not wait for the monitor, no tearing; frame pacing comes from the game, see
    // src/fcp/framerate.h), then Immediate, otherwise FIFO. D3D12 matches this with Present(0) without the tearing flag.
    const auto modes = phys_.getSurfacePresentModesKHR(surface_);
    const auto has = [&](vk::PresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    sci.presentMode = vk::PresentModeKHR::eFifo;
    if (!options_.vsync && has(vk::PresentModeKHR::eMailbox)) sci.presentMode = vk::PresentModeKHR::eMailbox;
    else if (!options_.vsync && has(vk::PresentModeKHR::eImmediate)) sci.presentMode = vk::PresentModeKHR::eImmediate;
    sci.clipped = true;
    sci.oldSwapchain = old;
    swapchain_ = dev_.createSwapchainKHR(sci);
    const auto images = dev_.getSwapchainImagesKHR(swapchain_);
    for (vk::Image image : images) {
      nvrhi::TextureDesc td;
      td.width = width_;
      td.height = height_;
      td.format = back_format_;
      td.debugName = "Swapchain";
      td.isRenderTarget = true;
      td.initialState = nvrhi::ResourceStates::Present;
      td.keepInitialState = true;
      images_.push_back(device()->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(VkImage(image)), td));
      present_.push_back(dev_.createSemaphore({}));
      acquire_.push_back(dev_.createSemaphore({}));
    }
    if (images_.empty()) return error && (*error = "swapchain without images", false);
    return true;
  }

  HostOptions options_;
  HMODULE library_ = nullptr;
  vk::Instance instance_;
  vk::DebugUtilsMessengerEXT messenger_;
  vk::SurfaceKHR surface_;
  vk::PhysicalDevice phys_;
  uint32_t max_samples_ = 1;
  vk::Device dev_;
  vk::Queue queue_;
  uint32_t family_ = 0;
  vk::SwapchainKHR swapchain_;
  nvrhi::Format back_format_ = nvrhi::Format::RGBA8_UNORM;
  uint32_t width_ = 0, height_ = 0, index_ = 0;
  uint64_t frame_ = 0;
  std::vector<nvrhi::TextureHandle> images_;
  std::vector<vk::Semaphore> acquire_, present_;
  std::vector<nvrhi::EventQueryHandle> frame_queries_;
  vk::Semaphore timeline_;           // own CPU fence (SignalFence/WaitFence)
  uint64_t timeline_next_ = 0;       // under the caller's lock
  std::array<uint64_t, 2> frame_fences_{};  // fence per present (split present)
  bool acquired_ = false;
  uint64_t TimelineValue() const { return timeline_ ? dev_.getSemaphoreCounterValue(timeline_) : ~0ull; }
  nvrhi::CommandListHandle barrier_list_;
  nvrhi::vulkan::DeviceHandle nvrhi_;
  nvrhi::DeviceHandle validation_;
  std::unique_ptr<VulkanPipelineCache> pipeline_cache_;
  std::unique_ptr<VulkanOcclusion> occlusion_;
  std::string description_;
};

}  // namespace

std::unique_ptr<HostDevice> CreateVulkanHost(const HostOptions& options, std::string* error) {
  auto host = std::make_unique<VulkanHost>();
  if (!host->Init(options, error)) return nullptr;
  return host;
}

}  // namespace fcp::nr
