// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/backend/vulkan_dispatch.cpp),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
#include "kk/backend/vulkan_dispatch.h"

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace fcp::nr {

void InitVulkanDispatch(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                        VkDevice device) {
  VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);
  if (instance) {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance));
  }
  if (device) {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device));
  }
}

PFN_vkGetInstanceProcAddr VulkanGetInstanceProcAddr() { return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr; }

PFN_vkGetDeviceProcAddr VulkanGetDeviceProcAddr() { return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr; }

}  // namespace fcp::nr
