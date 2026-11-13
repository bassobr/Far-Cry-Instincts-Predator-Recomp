// Measurement program: can the SPIR-V shaders produced by fcshaders be built into pipelines in Vulkan, and how long
// does it take? Layout per tools/shaders/include/fcshaders/abi.h: set 0 = 3 uniform buffers, sets 1-4 = image arrays,
// set 5 = sampler array, set 6 = storage buffer array (vertex pulling). No vertex inputs.
//
//   pso_vulkan <run>/spirv <pairs.tsv> [--threads N] [--group g] [--variants K] [--save cache.bin]
//              [--load cache.bin] [--out times.tsv]
//
// --save/--load: write a VkPipelineCache to disk or pre-populate it (cost of an own disk cache).
// Needs only the graphics driver's vulkan-1.dll (volk), no Vulkan SDK.
#include "probe_common.h"

#define VK_NO_PROTOTYPES
#include "volk.h"

using namespace probe;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("%s: %d\n", #x, (int)r_); return 1; } } while (0)

int main(int argc, char** argv) {
    Options o = parseArgs(argc, argv);
    std::vector<Pair> pairs = readPairs(o);
    std::printf("Pairs: %zu, variants per pair: %d, threads: %d\n", pairs.size(), o.variants, o.threads);

    CHECK(volkInitialize());
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    volkLoadInstance(instance);

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance, &n, devs.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (!phys || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = d;
    }
    if (!phys) { std::printf("no Vulkan device\n"); return 1; }
    VkPhysicalDeviceFloatControlsProperties fc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES};
    VkPhysicalDeviceDescriptorIndexingProperties dip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    dip.pNext = &fc;
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props.pNext = &dip;
    vkGetPhysicalDeviceProperties2(phys, &props);
    const auto& lim = props.properties.limits;
    std::printf("Device: %s, API %u.%u.%u\n", props.properties.deviceName, VK_VERSION_MAJOR(props.properties.apiVersion),
                VK_VERSION_MINOR(props.properties.apiVersion), VK_VERSION_PATCH(props.properties.apiVersion));
    std::printf("maxVertexOutputComponents=%u maxFragmentInputComponents=%u maxClipDistances=%u "
                "shaderSignedZeroInfNanPreserveFloat32=%u\n",
                lim.maxVertexOutputComponents, lim.maxFragmentInputComponents, lim.maxClipDistances,
                fc.shaderSignedZeroInfNanPreserveFloat32);
    std::printf("maxPerStageUpdateAfterBind: Sampler=%u SampledImages=%u StorageBuffers=%u; without UpdateAfterBind: "
                "Sampler=%u SampledImages=%u StorageBuffers=%u\n",
                dip.maxPerStageDescriptorUpdateAfterBindSamplers, dip.maxPerStageDescriptorUpdateAfterBindSampledImages,
                dip.maxPerStageDescriptorUpdateAfterBindStorageBuffers, lim.maxPerStageDescriptorSamplers,
                lim.maxPerStageDescriptorSampledImages, lim.maxPerStageDescriptorStorageBuffers);

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    std::printf("Features: runtimeDescriptorArray=%u partiallyBound=%u updateAfterBind(image/sampler/storage)=%u/%u/%u "
                "shaderClipDistance=%u\n",
                f12.runtimeDescriptorArray, f12.descriptorBindingPartiallyBound,
                f12.descriptorBindingSampledImageUpdateAfterBind, f12.descriptorBindingSampledImageUpdateAfterBind,
                f12.descriptorBindingStorageBufferUpdateAfterBind, f2.features.shaderClipDistance);
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.runtimeDescriptorArray = VK_TRUE;
    e12.descriptorBindingPartiallyBound = VK_TRUE;
    e12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    e12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    e12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.features.shaderClipDistance = VK_TRUE;
    e2.pNext = &e12;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device;
    CHECK(vkCreateDevice(phys, &dci, nullptr, &device));
    volkLoadDevice(device);

    // Descriptor layouts.
    VkDescriptorSetLayout sets[7];
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    {
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            b[i].descriptorCount = 1;
            b[i].stageFlags = both;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3;
        ci.pBindings = b;
        CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &sets[0]));
    }
    const VkDescriptorType types[7] = {VK_DESCRIPTOR_TYPE_MAX_ENUM, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_SAMPLER,
                                       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    const uint32_t counts[7] = {0, 8192, 64, 256, 64, 1024, 4096};
    for (int s = 1; s < 7; ++s) {
        VkDescriptorSetLayoutBinding b{};
        b.descriptorType = types[s];
        b.descriptorCount = counts[s];
        b.stageFlags = both;
        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        if (types[s] != VK_DESCRIPTOR_TYPE_SAMPLER)
            flags |= VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = 1;
        bf.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &bf;
        if (types[s] != VK_DESCRIPTOR_TYPE_SAMPLER) ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        ci.bindingCount = 1;
        ci.pBindings = &b;
        CHECK(vkCreateDescriptorSetLayout(device, &ci, nullptr, &sets[s]));
    }
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 7;
    plci.pSetLayouts = sets;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout));

    // Render pass: one colour target, depth D32F+S8.
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[1].format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (auto& a : att) {
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].initialLayout = att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2;
    rpci.pAttachments = att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    VkRenderPass renderPass;
    CHECK(vkCreateRenderPass(device, &rpci, nullptr, &renderPass));

    // Shader modules (each file once), time measured separately.
    std::map<std::string, VkShaderModule> modules;
    std::vector<double> modMs;
    size_t bytes = 0;
    auto tm = Clock::now();
    for (auto& p : pairs)
        for (const std::string* nme : {&p.vs, &p.ps})
            if (!modules.count(*nme)) {
                std::vector<uint8_t> code = readFile(o.shaderDir + "/" + *nme + ".spv");
                if (code.empty()) { std::printf("missing: %s\n", nme->c_str()); return 1; }
                bytes += code.size();
                VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                ci.codeSize = code.size();
                ci.pCode = (const uint32_t*)code.data();
                auto t = Clock::now();
                VkShaderModule m;
                CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
                modMs.push_back(msSince(t));
                modules[*nme] = m;
            }
    std::printf("Shaders loaded: %zu files, %.1f MB\n", modules.size(), bytes / 1048576.0);
    printStats("Vulkan vkCreateShaderModule (wall incl. file reads)", modMs, msSince(tm), 1, 0);

    VkPipelineCache cache = VK_NULL_HANDLE;
    if (!o.save.empty() || !o.load.empty()) {
        std::vector<uint8_t> init;
        if (!o.load.empty()) init = readFile(o.load);
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = init.size();
        ci.pInitialData = init.data();
        auto t = Clock::now();
        CHECK(vkCreatePipelineCache(device, &ci, nullptr, &cache));
        std::printf("Pipeline cache created (initial data %.1f MB) in %.1f ms\n", init.size() / 1048576.0, msSince(t));
    }

    const int total = (int)pairs.size() * o.variants;
    std::vector<double> ms(total, 0.0);
    std::vector<VkResult> results(total, VK_SUCCESS);
    std::vector<VkPipeline> pipes(total, VK_NULL_HANDLE);
    std::atomic<int> failed{0};
    auto t0 = Clock::now();
    parallelFor(total, o.threads, [&](int i) {
        const Pair& p = pairs[i / o.variants];
        const int v = i % o.variants;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = modules[p.vs];
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = modules[p.ps];
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = (v & 4) ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = (v & 2) ? VK_FALSE : VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 0xF;
        if (v & 1) {
            ba.blendEnable = VK_TRUE;
            ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            ba.dstColorBlendFactor = (v & 8) ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &msaa;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        ci.renderPass = renderPass;
        auto t = Clock::now();
        VkResult r = vkCreateGraphicsPipelines(device, cache, 1, &ci, nullptr, &pipes[i]);
        ms[i] = msSince(t);
        results[i] = r;
        if (r != VK_SUCCESS) ++failed;
    });
    double wall = msSince(t0);
    printStats("Vulkan vkCreateGraphicsPipelines", ms, wall, o.threads, failed.load());
    for (int i = 0, shown = 0; i < total && shown < 10; ++i)
        if (results[i] != VK_SUCCESS) {
            const Pair& p = pairs[i / o.variants];
            std::printf("  error %d: %s + %s (variant %d)\n", (int)results[i], p.vs.c_str(), p.ps.c_str(),
                        i % o.variants);
            ++shown;
        }

    if (!o.save.empty() && cache) {
        size_t size = 0;
        vkGetPipelineCacheData(device, cache, &size, nullptr);
        std::vector<uint8_t> data(size);
        vkGetPipelineCacheData(device, cache, &size, data.data());
        std::ofstream f(o.save, std::ios::binary);
        f.write((const char*)data.data(), size);
        std::printf("Pipeline cache saved: %.1f MB\n", size / 1048576.0);
    }
    if (!o.out.empty()) {
        std::ofstream f(o.out);
        for (int i = 0; i < total; ++i) {
            const Pair& p = pairs[i / o.variants];
            f << p.vs << '\t' << p.ps << '\t' << (i % o.variants) << '\t' << ms[i] << '\t' << (int)results[i] << '\n';
        }
    }
    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return failed.load() ? 3 : 0;
}
