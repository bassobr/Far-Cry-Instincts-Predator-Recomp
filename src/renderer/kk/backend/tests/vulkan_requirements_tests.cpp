// From King Kong Recompiled (github.com/TekRantGaming/king-kong-recompiled, e3e5d3f, native-renderer/backend/tests/vulkan_requirements_tests.cpp),
// MIT, Copyright (c) 2026 TekRantGaming and contributors: src/renderer/kk/LICENSE-KingKongRecompiled.txt.
// Changes: src/renderer/kk/CHANGES.md.
#include "kk/backend/tests/check.h"
#include "kk/backend/vulkan_requirements.h"
#include <cstdio>
#include <string>
#include <vector>

struct Features {
  bool shaderSampledImageArrayDynamicIndexing = true;
  bool shaderStorageBufferArrayDynamicIndexing = true;
  bool shaderSampledImageArrayNonUniformIndexing = true;
  bool shaderStorageBufferArrayNonUniformIndexing = true;
  bool descriptorBindingSampledImageUpdateAfterBind = true;
  bool descriptorBindingStorageBufferUpdateAfterBind = true;
  bool descriptorBindingUpdateUnusedWhilePending = true;
  bool descriptorBindingPartiallyBound = true;
  bool runtimeDescriptorArray = true;
};

struct Limits {
  unsigned maxPerStageDescriptorUpdateAfterBindSampledImages = fcp::nr::kNativeSampledImageSlots;
  unsigned maxPerStageDescriptorUpdateAfterBindStorageBuffers = fcp::nr::kNativeStorageBufferSlots;
  unsigned maxPerStageDescriptorUpdateAfterBindSamplers = fcp::nr::kNativeSamplerSlots;
  unsigned maxDescriptorSetUpdateAfterBindSampledImages = fcp::nr::kNativeSampledImageSlots;
  unsigned maxDescriptorSetUpdateAfterBindStorageBuffers = fcp::nr::kNativeStorageBufferSlots;
  unsigned maxDescriptorSetUpdateAfterBindSamplers = fcp::nr::kNativeSamplerSlots;
  unsigned maxPerStageUpdateAfterBindResources = fcp::nr::kNativeSampledImageSlots + fcp::nr::kNativeStorageBufferSlots + fcp::nr::kNativeSamplerSlots + fcp::nr::kNativeConstantBufferSlots;
  unsigned maxUpdateAfterBindDescriptorsInAllPools = fcp::nr::kNativeSampledImageSlots + fcp::nr::kNativeStorageBufferSlots + fcp::nr::kNativeSamplerSlots;
};

static int VulkanRequirementsMain() {
  Features p;
  std::vector<std::string> missing;
  auto report = [&](const char* name) { missing.emplace_back(name); };
  if (!fcp::nr::CheckNativeBindlessFeatures(p, report) || !missing.empty()) return 1;
  bool Features::* members[] = {
    &Features::shaderSampledImageArrayDynamicIndexing, &Features::shaderStorageBufferArrayDynamicIndexing,
    &Features::shaderSampledImageArrayNonUniformIndexing, &Features::shaderStorageBufferArrayNonUniformIndexing,
    &Features::descriptorBindingSampledImageUpdateAfterBind, &Features::descriptorBindingStorageBufferUpdateAfterBind,
    &Features::descriptorBindingUpdateUnusedWhilePending, &Features::descriptorBindingPartiallyBound,
    &Features::runtimeDescriptorArray
  };
  for (auto member : members) {
    Features unavailable = p;
    unavailable.*member = false;
    missing.clear();
    if (fcp::nr::CheckNativeBindlessFeatures(unavailable, report) || missing.size() != 1) return 2;
  }
  missing.clear();
  Features unavailable{false,false,false,false,false,false,false,false,false};
  if (fcp::nr::CheckNativeBindlessFeatures(unavailable, report) || missing.size() != 9) return 3;
  Limits limits;
  auto limit_report = [&](const char* name, unsigned, unsigned) { missing.emplace_back(name); };
  missing.clear();
  if (!fcp::nr::CheckNativeBindlessLimits(7, limits, limit_report) || !missing.empty()) return 4;
  unsigned Limits::* limit_members[] = {
    &Limits::maxPerStageDescriptorUpdateAfterBindSampledImages, &Limits::maxPerStageDescriptorUpdateAfterBindStorageBuffers,
    &Limits::maxPerStageDescriptorUpdateAfterBindSamplers, &Limits::maxDescriptorSetUpdateAfterBindSampledImages,
    &Limits::maxDescriptorSetUpdateAfterBindStorageBuffers, &Limits::maxDescriptorSetUpdateAfterBindSamplers,
    &Limits::maxPerStageUpdateAfterBindResources, &Limits::maxUpdateAfterBindDescriptorsInAllPools
  };
  for (auto member : limit_members) {
    Limits too_small = limits;
    --(too_small.*member);
    missing.clear();
    if (fcp::nr::CheckNativeBindlessLimits(7, too_small, limit_report) || missing.size() != 1) return 5;
  }
  missing.clear();
  if (fcp::nr::CheckNativeBindlessLimits(6, limits, limit_report) || missing.size() != 1) return 6;
  fcp::nr::NativeTableCapacities selected;
  if (!fcp::nr::SelectNativeTableCapacities(7, limits, false, selected) || selected != fcp::nr::kNativeTableCapacities) return 7;
  if (!fcp::nr::SelectNativeTableCapacities(7, limits, true, selected) || selected != fcp::nr::kSmallTableCapacities) return 8;
  for (unsigned type = 0; type < 3; ++type) {
    Limits single = limits;
    auto want = fcp::nr::kNativeTableCapacities;
    if (type == 0) {
      single.maxPerStageDescriptorUpdateAfterBindSamplers = single.maxDescriptorSetUpdateAfterBindSamplers = 128;
      want[4] = 128;
    } else if (type == 1) {
      single.maxPerStageDescriptorUpdateAfterBindStorageBuffers = single.maxDescriptorSetUpdateAfterBindStorageBuffers = 300;
      want[5] = 300;
    } else {
      single.maxPerStageDescriptorUpdateAfterBindSampledImages = single.maxDescriptorSetUpdateAfterBindSampledImages = 512;
      for (unsigned i = 0; i < 4; ++i) want[i] = 128;
    }
    if (!fcp::nr::SelectNativeTableCapacities(7, single, false, selected) || selected != want) return 17;
  }
  Limits low = limits;
  low.maxPerStageDescriptorUpdateAfterBindSampledImages = low.maxDescriptorSetUpdateAfterBindSampledImages = 4096;
  low.maxPerStageDescriptorUpdateAfterBindStorageBuffers = low.maxDescriptorSetUpdateAfterBindStorageBuffers = 1024;
  low.maxPerStageDescriptorUpdateAfterBindSamplers = low.maxDescriptorSetUpdateAfterBindSamplers = 128;
  low.maxPerStageUpdateAfterBindResources = 4096;
  if (!fcp::nr::SelectNativeTableCapacities(7, low, false, selected) || !fcp::nr::NativeTablesFit(selected, low)) return 9;
  Limits floor = limits;
  floor.maxPerStageDescriptorUpdateAfterBindSampledImages = floor.maxDescriptorSetUpdateAfterBindSampledImages = 512;
  floor.maxPerStageDescriptorUpdateAfterBindStorageBuffers = floor.maxDescriptorSetUpdateAfterBindStorageBuffers = 256;
  floor.maxPerStageDescriptorUpdateAfterBindSamplers = floor.maxDescriptorSetUpdateAfterBindSamplers = 128;
  floor.maxPerStageUpdateAfterBindResources = 902;
  floor.maxUpdateAfterBindDescriptorsInAllPools = 896;
  if (!fcp::nr::SelectNativeTableCapacities(7, floor, false, selected) || selected != fcp::nr::kMinimumTableCapacities) return 10;
  missing.clear();
  if (!fcp::nr::CheckNativeBindlessLimits(7, floor, limit_report, fcp::nr::kMinimumTableCapacities) || !missing.empty()) return 15;
  for (auto member : limit_members) {
    Limits below = floor;
    --(below.*member);
    if (fcp::nr::SelectNativeTableCapacities(7, below, false, selected)) return 11;
    missing.clear();
    if (fcp::nr::CheckNativeBindlessLimits(7, below, limit_report, fcp::nr::kMinimumTableCapacities) || missing.size() != 1) return 16;
  }
  if (fcp::nr::SelectNativeTableCapacities(6, limits, false, selected)) return 12;
  // Independently vary stage, set, aggregate and pool budgets. Every valid
  // combination must yield a layout inside all limits, including forced-small.
  unsigned seed = 37;
  for (unsigned n = 0; n < 256; ++n) {
    Limits varied = floor;
    for (auto member : limit_members) {
      seed = seed * 1664525u + 1013904223u;
      varied.*member += seed % (limits.*member - floor.*member + 1);
    }
    if (!fcp::nr::SelectNativeTableCapacities(7, varied, n % 2 != 0, selected) ||
        !fcp::nr::NativeTablesFit(selected, varied)) return 13;
    for (std::size_t i = 0; i < selected.size(); ++i)
      if (selected[i] < fcp::nr::kMinimumTableCapacities[i] || selected[i] > fcp::nr::kNativeTableCapacities[i]) return 14;
  }
  std::puts("Native Vulkan requirements: feature, limit, capacity-isolation and 256 mixed-budget cases passed");
  return 0;  // FCP: main returned 0 implicitly
}

// FCP: the file's own main as one case of the common runner.
TEST(VulkanRequirements) { CHECK_EQ(VulkanRequirementsMain(), 0); }
