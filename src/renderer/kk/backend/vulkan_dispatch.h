// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/backend/vulkan_dispatch.h),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
// NVRHI's Vulkan backend calls Vulkan through vulkan.hpp's default dynamic
// dispatcher. With NVRHI built static, the dispatcher's storage lives in the
// program that links it (here: vulkan_dispatch.cpp), and the program must
// initialise it before nvrhi::vulkan::createDevice.
#pragma once

#include <vulkan/vulkan_core.h>

namespace fcp::nr {

void InitVulkanDispatch(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                        VkDevice device);

// The loader's entry points the dispatcher was initialised with (null before InitVulkanDispatch), for
// libraries that take them (the upscalers' SDKs).
PFN_vkGetInstanceProcAddr VulkanGetInstanceProcAddr();
PFN_vkGetDeviceProcAddr VulkanGetDeviceProcAddr();

}  // namespace fcp::nr
