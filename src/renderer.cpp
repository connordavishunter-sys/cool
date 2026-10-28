#include "renderer.hpp"

#include <SDL2/SDL_image.h>
#include <SDL2/SDL_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace {

constexpr VkMemoryPropertyFlags HOST = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkMemoryPropertyFlags DEVICE = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr float NEAR_PLANE = 0.05f;
constexpr float LIGHT_INTENSITY = 8.0f;

struct FrameUBO {
    glm::mat4 viewProj, invViewProj;
    glm::vec4 sunDir, sunColor, skyZenith, skyHorizon, fog;
    glm::uvec4 info;
};
struct LightGpu { glm::vec4 pos, color; };
struct AccumPC { glm::mat4 invViewProj, prevViewProj; glm::vec4 camDelta; };
struct PostPC { glm::mat4 invViewProj; glm::vec4 selection, selSize, params, grade; };

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

std::vector<uint8_t> loadRGBA(const std::string& path, int& w, int& h) {
    SDL_Surface* s = IMG_Load(path.c_str());
    if (!s) throw std::runtime_error("Failed to load " + path + ": " + IMG_GetError());
    SDL_Surface* c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(s);
    w = c->w;
    h = c->h;
    std::vector<uint8_t> out(w * h * 4);
    for (int y = 0; y < h; ++y) std::memcpy(&out[y * w * 4], (uint8_t*)c->pixels + y * c->pitch, w * 4);
    SDL_FreeSurface(c);
    return out;
}

void uploadImage(VkCtx& ctx, Image& img, const std::vector<uint8_t>& data, const std::vector<VkBufferImageCopy>& regions) {
    Buffer st = ctx.createBuffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST);
    std::memcpy(st.mapped, data.data(), data.size());
    ctx.submitNow([&](VkCommandBuffer cmd) {
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cmd, st.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(), regions.data());
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    ctx.destroyBuffer(st);
}

VkSampler makeSampler(VkDevice dev, VkFilter filter, VkSamplerAddressMode mode, float maxLod) {
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = filter;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = mode;
    ci.maxLod = maxLod;
    VkSampler s;
    VK_CHECK(vkCreateSampler(dev, &ci, nullptr, &s));
    return s;
}

VkDescriptorSetLayout makeSetLayout(VkDevice dev, const std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>>& bindings) {
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (uint32_t i = 0; i < bindings.size(); ++i) b.push_back({i, bindings[i].first, 1, bindings[i].second, nullptr});
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)b.size();
    ci.pBindings = b.data();
    VkDescriptorSetLayout l;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &l));
    return l;
}

VkPipelineLayout makeLayout(VkDevice dev, VkDescriptorSetLayout set, uint32_t pushSize, VkShaderStageFlags pushStages) {
    VkPushConstantRange pr{pushStages, 0, pushSize};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &set;
    ci.pushConstantRangeCount = pushSize ? 1 : 0;
    ci.pPushConstantRanges = &pr;
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(dev, &ci, nullptr, &l));
    return l;
}

struct GraphicsDesc {
    VkShaderModule vs, fs;
    VkPipelineLayout layout;
    const VkPipelineVertexInputStateCreateInfo* vertexInput;
    VkFormat color, depth = VK_FORMAT_UNDEFINED;
    bool depthTest = false, depthWrite = false, blend = false;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    const VkSpecializationInfo* spec = nullptr;
};

VkPipeline makeGraphics(VkDevice dev, const GraphicsDesc& d) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = d.vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = d.fs;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = d.spec;

    VkPipelineVertexInputStateCreateInfo emptyInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = d.cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depthTest;
    ds.depthWriteEnable = d.depthWrite;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reverse-Z
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    if (d.blend) {
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &d.color;
    rendering.depthAttachmentFormat = d.depth;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = d.vertexInput ? d.vertexInput : &emptyInput;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = d.layout;
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    return p;
}

void setViewport(VkCommandBuffer cmd, uint32_t w, uint32_t h) {
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

}  // namespace

// ---------------------------------------------------------------- setup

void Renderer::init(VkCtx& c, const std::string& baseDir, const Font& font) {
    ctx = &c;
    dev = c.device;
    base = baseDir;
    const bool rt = c.rtSupported;

    nearestRepeat = makeSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16);
    nearestClamp = makeSampler(dev, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
    linearClamp = makeSampler(dev, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
    loadTextures(font);

    // Shared quad index buffer: (0,1,2)(0,2,3) per quad.
    {
        std::vector<uint32_t> idx(MAX_QUADS * 6);
        for (uint32_t q = 0; q < MAX_QUADS; ++q) {
            const uint32_t o[6] = {0, 1, 2, 0, 2, 3};
            for (int i = 0; i < 6; ++i) idx[q * 6 + i] = q * 4 + o[i];
        }
        VkBufferUsageFlags usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (rt) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        VkDeviceSize bytes = idx.size() * 4;
        indexBuffer = c.createBuffer(bytes, usage, DEVICE);
        Buffer st = c.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST);
        std::memcpy(st.mapped, idx.data(), bytes);
        c.submitNow([&](VkCommandBuffer cmd) {
            VkBufferCopy r{0, 0, bytes};
            vkCmdCopyBuffer(cmd, st.buffer, indexBuffer.buffer, 1, &r);
        });
        c.destroyBuffer(st);
    }

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.cmdPool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    for (Frame& f : frames) {
        VK_CHECK(vkAllocateCommandBuffers(dev, &cai, &f.cmd));
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(dev, &fci, nullptr, &f.fence));
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &f.acquired));
        f.ubo = c.createBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, HOST);
        f.lights = c.createBuffer(MAX_LIGHTS * sizeof(LightGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST);
        f.chunkTable = c.createBuffer(MAX_INSTANCES * 24, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, HOST);
        f.staging = c.createBuffer(STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, HOST);
        f.uiVerts = c.createBuffer(UI_VERTS * sizeof(UiVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, HOST);
        if (!rt) continue;

        const VkBufferUsageFlags asUsage = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        const VkBufferUsageFlags scratchUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        f.instances = c.createBuffer(MAX_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, HOST);
        f.blasScratch = c.createBuffer(BLAS_SCRATCH_SIZE, scratchUsage, DEVICE);

        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geom;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        c.vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &MAX_INSTANCES, &sizes);
        f.tlasBuffer = c.createBuffer(sizes.accelerationStructureSize, asUsage, DEVICE);
        f.tlasScratch = c.createBuffer(sizes.buildScratchSize + c.asScratchAlign, scratchUsage, DEVICE);
        VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        aci.buffer = f.tlasBuffer.buffer;
        aci.size = sizes.accelerationStructureSize;
        aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(c.vkCreateAccelerationStructureKHR(dev, &aci, nullptr, &f.tlas));
    }

    createDescriptors();
    createPipelines();
    recreateSwapchain();
}

void Renderer::loadTextures(const Font& font) {
    // Block texture array with a CPU-generated mip chain.
    const uint32_t size = 16, mips = 5, layers = (uint32_t)kBlockTextureCount;
    blockArray = ctx->createImage(size, size, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  mips, layers, VK_IMAGE_ASPECT_COLOR_BIT, true);
    std::vector<uint8_t> data;
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t l = 0; l < layers; ++l) {
        int w, h;
        std::vector<uint8_t> px = loadRGBA(base + "assets/textures/blocks/" + kBlockTextures[l] + ".png", w, h);
        if (w != (int)size || h != (int)size) {  // resample to 16x16
            std::vector<uint8_t> r(size * size * 4);
            for (uint32_t y = 0; y < size; ++y)
                for (uint32_t x = 0; x < size; ++x)
                    std::memcpy(&r[(y * size + x) * 4], &px[((y * h / size) * w + x * w / size) * 4], 4);
            px = r;
        }
        uint32_t s = size;
        for (uint32_t m = 0; m < mips; ++m) {
            VkBufferImageCopy reg{};
            reg.bufferOffset = data.size();
            reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1};
            reg.imageExtent = {s, s, 1};
            regions.push_back(reg);
            data.insert(data.end(), px.begin(), px.end());
            uint32_t n = s / 2;
            if (!n) break;
            std::vector<uint8_t> next(n * n * 4);
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n; ++x)
                    for (int ch = 0; ch < 4; ++ch) {
                        int sum = px[((2 * y) * s + 2 * x) * 4 + ch] + px[((2 * y) * s + 2 * x + 1) * 4 + ch] +
                                  px[((2 * y + 1) * s + 2 * x) * 4 + ch] + px[((2 * y + 1) * s + 2 * x + 1) * 4 + ch];
                        next[(y * n + x) * 4 + ch] = (uint8_t)(sum / 4);
                    }
            px = std::move(next);
            s = n;
        }
    }
    uploadImage(*ctx, blockArray, data, regions);

    const char* files[UiTexCount] = {nullptr, "button_9_slice", "ui_background_9_slice", "slot", "slider_background_9_slice",
                                     "slider_handle", "title", nullptr, "heart", "hunger"};
    for (int t = 0; t < UiTexCount; ++t) {
        int w, h;
        std::vector<uint8_t> px;
        if (t == TexWhite) { w = h = 1; px = {255, 255, 255, 255}; }
        else if (t == TexFont) { w = font.atlasW; h = font.atlasH; px = font.rgba; }
        else px = loadRGBA(base + "assets/textures/ui/" + files[t] + ".png", w, h);
        uiTextures[t] = ctx->createImage(w, h, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy reg{};
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
        uploadImage(*ctx, uiTextures[t], px, {reg});
    }
}

void Renderer::createDescriptors() {
    const bool rt = ctx->rtSupported;
    const auto FRAG = VK_SHADER_STAGE_FRAGMENT_BIT, COMP = VK_SHADER_STAGE_COMPUTE_BIT;
    const auto SAMPLER = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, STORAGE_IMG = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

    std::vector<std::pair<VkDescriptorType, VkShaderStageFlags>> world = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | FRAG}, {SAMPLER, FRAG}};
    if (rt) {
        world.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, FRAG});
        world.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FRAG});
        world.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, FRAG});
    }
    worldSetLayout = makeSetLayout(dev, world);
    accumSetLayout = makeSetLayout(dev, {{STORAGE_IMG, COMP}, {SAMPLER, COMP}, {STORAGE_IMG, COMP}, {STORAGE_IMG, COMP},
                                         {STORAGE_IMG, COMP}, {STORAGE_IMG, COMP}});
    postSetLayout = makeSetLayout(dev, {{SAMPLER, FRAG}, {SAMPLER, FRAG}, {SAMPLER, FRAG}});
    uiSetLayout = makeSetLayout(dev, {{SAMPLER, FRAG}, {SAMPLER, FRAG}});

    std::vector<VkDescriptorPoolSize> sizes = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8}, {SAMPLER, 64}, {STORAGE_IMG, 32},
                                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
    if (rt) sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 8});
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 32;
    pci.poolSizeCount = (uint32_t)sizes.size();
    pci.pPoolSizes = sizes.data();
    VK_CHECK(vkCreateDescriptorPool(dev, &pci, nullptr, &descPool));

    auto alloc = [&](VkDescriptorSetLayout l) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &l;
        VkDescriptorSet s;
        VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &s));
        return s;
    };

    VkDescriptorImageInfo blocks{nearestRepeat, blockArray.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (Frame& f : frames) {
        f.worldSet = alloc(worldSetLayout);
        VkDescriptorBufferInfo ubo{f.ubo.buffer, 0, VK_WHOLE_SIZE}, table{f.chunkTable.buffer, 0, VK_WHOLE_SIZE},
            lights{f.lights.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asw.accelerationStructureCount = 1;
        asw.pAccelerationStructures = &f.tlas;
        VkWriteDescriptorSet w[5] = {};
        for (int i = 0; i < 5; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = f.worldSet;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &ubo;
        w[1].descriptorType = SAMPLER;
        w[1].pImageInfo = &blocks;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[2].pNext = &asw;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &table;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[4].pBufferInfo = &lights;
        vkUpdateDescriptorSets(dev, rt ? 5 : 2, w, 0, nullptr);
    }
    for (int p = 0; p < 2; ++p) {
        accumSets[p] = alloc(accumSetLayout);
        postSets[p] = alloc(postSetLayout);
    }
    for (int t = 0; t < UiTexCount; ++t) {
        uiSets[t] = alloc(uiSetLayout);
        VkSampler s = (t == TexFont || t == TexTitle) ? linearClamp : nearestClamp;
        VkDescriptorImageInfo img{s, uiTextures[t].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = w[1].dstSet = uiSets[t];
        w[0].descriptorCount = w[1].descriptorCount = 1;
        w[0].descriptorType = w[1].descriptorType = SAMPLER;
        w[0].pImageInfo = &img;
        w[1].dstBinding = 1;
        w[1].pImageInfo = &blocks;
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }
}

void Renderer::createPipelines() {
    const std::string dir = base + "shaders/";
    auto load = [&](const char* n) { return ctx->loadShader(dir + n); };
    VkShaderModule worldVs = load("world.vert.spv"), worldFs = load(ctx->rtSupported ? "world_rt.frag.spv" : "world.frag.spv");
    VkShaderModule skyVs = load("sky.vert.spv"), skyFs = load("sky.frag.spv");
    VkShaderModule fullVs = load("fullscreen.vert.spv"), postFs = load("post.frag.spv"), accumCs = load("accum.comp.spv");
    VkShaderModule uiVs = load("ui.vert.spv"), uiFs = load("ui.frag.spv");

    worldLayout = makeLayout(dev, worldSetLayout, 16, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    accumLayout = makeLayout(dev, accumSetLayout, sizeof(AccumPC), VK_SHADER_STAGE_COMPUTE_BIT);
    postLayout = makeLayout(dev, postSetLayout, sizeof(PostPC), VK_SHADER_STAGE_FRAGMENT_BIT);
    uiLayout = makeLayout(dev, uiSetLayout, 8, VK_SHADER_STAGE_VERTEX_BIT);

    const VkFormat HDR = VK_FORMAT_R16G16B16A16_SFLOAT, DEPTH = VK_FORMAT_D32_SFLOAT;
    VkVertexInputBindingDescription wb{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription wa[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32_UINT, 12},
                                               {2, 0, VK_FORMAT_R32_UINT, 16}};
    VkPipelineVertexInputStateCreateInfo worldInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    worldInput.vertexBindingDescriptionCount = 1;
    worldInput.pVertexBindingDescriptions = &wb;
    worldInput.vertexAttributeDescriptionCount = 3;
    worldInput.pVertexAttributeDescriptions = wa;

    skyPipe = makeGraphics(dev, {skyVs, skyFs, worldLayout, nullptr, HDR, DEPTH});
    for (int kind = 0; kind < 4; ++kind) {
        VkSpecializationMapEntry entry{0, 0, sizeof(int)};
        VkSpecializationInfo spec{1, &entry, sizeof(int), &kind};
        GraphicsDesc d{worldVs, worldFs, worldLayout, &worldInput, HDR, DEPTH};
        d.depthTest = true;
        bool blended = kind == KindWater || kind == KindTranslucent;
        d.depthWrite = !blended;
        d.blend = blended;
        d.cull = blended ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        d.spec = &spec;
        worldPipes[kind] = makeGraphics(dev, d);
    }
    postPipe = makeGraphics(dev, {fullVs, postFs, postLayout, nullptr, ctx->swapFormat});

    VkVertexInputBindingDescription ub{0, sizeof(UiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ua[4] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
                                               {2, 0, VK_FORMAT_R32_SFLOAT, 16}, {3, 0, VK_FORMAT_R8G8B8A8_UNORM, 20}};
    VkPipelineVertexInputStateCreateInfo uiInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    uiInput.vertexBindingDescriptionCount = 1;
    uiInput.pVertexBindingDescriptions = &ub;
    uiInput.vertexAttributeDescriptionCount = 4;
    uiInput.pVertexAttributeDescriptions = ua;
    GraphicsDesc ud{uiVs, uiFs, uiLayout, &uiInput, ctx->swapFormat};
    ud.blend = true;
    uiPipe = makeGraphics(dev, ud);

    VkComputePipelineCreateInfo cci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cci.stage.module = accumCs;
    cci.stage.pName = "main";
    cci.layout = accumLayout;
    VK_CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cci, nullptr, &accumPipe));

    for (auto m : {worldVs, worldFs, skyVs, skyFs, fullVs, postFs, accumCs, uiVs, uiFs}) vkDestroyShaderModule(dev, m, nullptr);
}

void Renderer::createTargets() {
    currentScale = std::clamp(currentScale, 0.25f, 1.0f);
    renderW = std::max(1u, (uint32_t)(ctx->swapExtent.width * currentScale));
    renderH = std::max(1u, (uint32_t)(ctx->swapExtent.height * currentScale));
    hdr = ctx->createImage(renderW, renderH, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    depth = ctx->createImage(renderW, renderH, VK_FORMAT_D32_SFLOAT,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 1, 1, VK_IMAGE_ASPECT_DEPTH_BIT);
    for (int i = 0; i < 2; ++i) {
        hist[i] = ctx->createImage(renderW, renderH, VK_FORMAT_R32G32B32A32_SFLOAT,
                                   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        dist[i] = ctx->createImage(renderW, renderH, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    }
    ctx->submitNow([&](VkCommandBuffer cmd) {
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (Image* img : {&hist[0], &hist[1], &dist[0], &dist[1]}) {
            imageBarrier(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            vkCmdClearColorImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        }
        imageBarrier(cmd, hdr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        imageBarrier(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    });
    historyValid = false;
    writeTargetDescriptors();
}

void Renderer::writeTargetDescriptors() {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(ctx->gpu, VK_FORMAT_R32G32B32A32_SFLOAT, &fp);
    VkSampler histSampler = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? linearClamp : nearestClamp;
    const auto GENERAL = VK_IMAGE_LAYOUT_GENERAL, DEPTH_RO = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    for (int p = 0; p < 2; ++p) {
        VkDescriptorImageInfo a[6] = {{VK_NULL_HANDLE, hdr.view, GENERAL},          {nearestClamp, depth.view, DEPTH_RO},
                                      {VK_NULL_HANDLE, hist[1 - p].view, GENERAL}, {VK_NULL_HANDLE, dist[1 - p].view, GENERAL},
                                      {VK_NULL_HANDLE, hist[p].view, GENERAL},     {VK_NULL_HANDLE, dist[p].view, GENERAL}};
        VkDescriptorImageInfo q[3] = {{linearClamp, hdr.view, GENERAL}, {histSampler, hist[p].view, GENERAL}, {nearestClamp, depth.view, DEPTH_RO}};
        VkWriteDescriptorSet w[9] = {};
        for (int i = 0; i < 9; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].descriptorCount = 1;
            if (i < 6) {
                w[i].dstSet = accumSets[p];
                w[i].dstBinding = i;
                w[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[i].pImageInfo = &a[i];
            } else {
                w[i].dstSet = postSets[p];
                w[i].dstBinding = i - 6;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &q[i - 6];
            }
        }
        vkUpdateDescriptorSets(dev, 9, w, 0, nullptr);
    }
}

void Renderer::destroyTargets() {
    for (Image* img : {&hdr, &depth, &hist[0], &hist[1], &dist[0], &dist[1]}) ctx->destroyImage(*img);
}

void Renderer::recreateSwapchain() {
    vkDeviceWaitIdle(dev);
    if (ctx->swapchain) ctx->destroySwapchain();
    ctx->createSwapchain();
    for (auto s : renderDone) vkDestroySemaphore(dev, s, nullptr);
    renderDone.resize(ctx->swapImages.size());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : renderDone) VK_CHECK(vkCreateSemaphore(dev, &sci, nullptr, &s));
    destroyTargets();
    createTargets();
}

void Renderer::destroyChunk(GpuChunk& c, Frame& f) {
    Buffer vb = c.vb, blasBuf = c.blasBuffer;
    VkAccelerationStructureKHR blas = c.blas;
    f.deletions.push_back([this, vb, blasBuf, blas]() mutable {
        if (blas) ctx->vkDestroyAccelerationStructureKHR(dev, blas, nullptr);
        ctx->destroyBuffer(vb);
        ctx->destroyBuffer(blasBuf);
    });
}

void Renderer::clearWorld() {
    vkDeviceWaitIdle(dev);
    for (Frame& f : frames) {
        for (auto& d : f.deletions) d();
        f.deletions.clear();
    }
    for (auto& [k, c] : chunks) {
        destroyChunk(c, frames[0]);
    }
    for (auto& d : frames[0].deletions) d();
    frames[0].deletions.clear();
    chunks.clear();
    pendingMeshes.clear();
    historyValid = false;
}

void Renderer::destroy() {
    vkDeviceWaitIdle(dev);
    clearWorld();
    destroyTargets();
    for (Frame& f : frames) {
        vkDestroyFence(dev, f.fence, nullptr);
        vkDestroySemaphore(dev, f.acquired, nullptr);
        if (f.tlas) ctx->vkDestroyAccelerationStructureKHR(dev, f.tlas, nullptr);
        for (Buffer* b : {&f.ubo, &f.lights, &f.chunkTable, &f.instances, &f.staging, &f.uiVerts, &f.tlasBuffer, &f.tlasScratch, &f.blasScratch})
            ctx->destroyBuffer(*b);
    }
    for (auto s : renderDone) vkDestroySemaphore(dev, s, nullptr);
    ctx->destroyBuffer(indexBuffer);
    ctx->destroyImage(blockArray);
    for (auto& t : uiTextures) ctx->destroyImage(t);
    for (auto p : {skyPipe, worldPipes[0], worldPipes[1], worldPipes[2], worldPipes[3], accumPipe, postPipe, uiPipe}) vkDestroyPipeline(dev, p, nullptr);
    for (auto l : {worldLayout, accumLayout, postLayout, uiLayout}) vkDestroyPipelineLayout(dev, l, nullptr);
    for (auto l : {worldSetLayout, accumSetLayout, postSetLayout, uiSetLayout}) vkDestroyDescriptorSetLayout(dev, l, nullptr);
    vkDestroyDescriptorPool(dev, descPool, nullptr);
    for (auto s : {nearestRepeat, nearestClamp, linearClamp}) vkDestroySampler(dev, s, nullptr);
}

// ---------------------------------------------------------------- per frame

void Renderer::uploadMeshes(VkCommandBuffer cmd, Frame& f) {
    const bool rt = ctx->rtSupported;
    const size_t maxBuilds = 64;
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    geoms.reserve(maxBuilds * 2);  // pGeometries point into this, so it must never reallocate
    infos.reserve(maxBuilds);
    ranges.reserve(maxBuilds * 2);
    VkDeviceSize stagingOff = 0, scratchOff = 0;
    bool copied = false;

    VkBufferUsageFlags vbUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (rt) vbUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    while (!pendingMeshes.empty() && infos.size() < maxBuilds) {
        MeshData& m = pendingMeshes.front();
        if (m.verts.size() / 4 > MAX_QUADS) {  // pathological chunk: drop the excess
            m.verts.resize(MAX_QUADS * 4);
            m.solidQuads = std::min(m.solidQuads, MAX_QUADS);
            m.cutoutQuads = std::min(m.cutoutQuads, MAX_QUADS - m.solidQuads);
            m.waterQuads = std::min(m.waterQuads, MAX_QUADS - m.solidQuads - m.cutoutQuads);
            m.translucentQuads = MAX_QUADS - m.solidQuads - m.cutoutQuads - m.waterQuads;
        }
        VkDeviceSize bytes = m.verts.size() * sizeof(Vertex);
        if (bytes > STAGING_SIZE - stagingOff) break;

        // The BLAS holds up to three geometries: opaque solid quads, alpha-tested cutout quads and
        // translucent quads that tint shadow rays. Water is left out.
        VkAccelerationStructureGeometryKHR geom[3]{};
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        const uint32_t kindQuads[3] = {m.solidQuads, m.cutoutQuads, m.translucentQuads};
        const uint32_t kindFirst[3] = {0, m.solidQuads, m.solidQuads + m.cutoutQuads + m.waterQuads};
        uint32_t prims[3], firstQuad[3];
        for (int k = 0; k < 3 && rt; ++k) {
            uint32_t quads = kindQuads[k];
            if (quads == 0) continue;
            uint32_t g = bi.geometryCount++;
            firstQuad[g] = kindFirst[k];
            prims[g] = quads * 2;
            geom[g].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            geom[g].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            geom[g].flags = k == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : k == 2 ? VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR : 0;
            auto& t = geom[g].geometry.triangles;
            t.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            t.vertexStride = sizeof(Vertex);
            t.maxVertex = quads * 4 - 1;
            t.indexType = VK_INDEX_TYPE_UINT32;
            t.indexData.deviceAddress = indexBuffer.address;
        }
        bool buildBlas = bi.geometryCount > 0;
        if (buildBlas) {
            bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
            bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            bi.pGeometries = geom;
            ctx->vkGetAccelerationStructureBuildSizesKHR(dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, prims, &sizes);
            if (scratchOff + alignUp(sizes.buildScratchSize, ctx->asScratchAlign) > BLAS_SCRATCH_SIZE) break;
        }

        int64_t key = chunkKey(m.cx, m.cz);
        if (auto it = chunks.find(key); it != chunks.end()) {
            destroyChunk(it->second, f);
            chunks.erase(it);
        }
        if (bytes == 0) {
            pendingMeshes.pop_front();
            continue;
        }

        GpuChunk g;
        g.cx = m.cx;
        g.cz = m.cz;
        g.quads[KindOpaque] = m.solidQuads;
        g.quads[KindCutout] = m.cutoutQuads;
        g.quads[KindWater] = m.waterQuads;
        g.quads[KindTranslucent] = m.translucentQuads;
        g.minY = m.minY;
        g.maxY = m.maxY;
        g.lights = std::move(m.lights);
        g.vb = ctx->createBuffer(bytes, vbUsage, DEVICE);
        std::memcpy((uint8_t*)f.staging.mapped + stagingOff, m.verts.data(), bytes);
        VkBufferCopy region{stagingOff, 0, bytes};
        vkCmdCopyBuffer(cmd, f.staging.buffer, g.vb.buffer, 1, &region);
        stagingOff = alignUp(stagingOff + bytes, 16);
        copied = true;

        if (buildBlas) {
            g.blasBuffer = ctx->createBuffer(sizes.accelerationStructureSize,
                                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, DEVICE);
            VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
            aci.buffer = g.blasBuffer.buffer;
            aci.size = sizes.accelerationStructureSize;
            aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            VK_CHECK(ctx->vkCreateAccelerationStructureKHR(dev, &aci, nullptr, &g.blas));
            VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
            dai.accelerationStructure = g.blas;
            g.blasAddress = ctx->vkGetAccelerationStructureDeviceAddressKHR(dev, &dai);

            for (uint32_t i = 0; i < bi.geometryCount; ++i) {
                g.geomAddress[i] = g.vb.address + firstQuad[i] * 4 * sizeof(Vertex);
                geom[i].geometry.triangles.vertexData.deviceAddress = g.geomAddress[i];
                geoms.push_back(geom[i]);
                ranges.push_back({prims[i], 0, 0, 0});
            }
            bi.pGeometries = &geoms[geoms.size() - bi.geometryCount];
            bi.dstAccelerationStructure = g.blas;
            bi.scratchData.deviceAddress = f.blasScratch.address + scratchOff;
            scratchOff += alignUp(sizes.buildScratchSize, ctx->asScratchAlign);
            infos.push_back(bi);
        }
        chunks.emplace(key, std::move(g));
        pendingMeshes.pop_front();
    }

    if (copied) memoryBarrier(cmd);
    if (!infos.empty()) {
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ptrs;
        for (size_t i = 0, r = 0; i < infos.size(); r += infos[i++].geometryCount) ptrs.push_back(&ranges[r]);
        ctx->vkCmdBuildAccelerationStructuresKHR(cmd, (uint32_t)infos.size(), infos.data(), ptrs.data());
        memoryBarrier(cmd);
    }
}

void Renderer::renderWorld(VkCommandBuffer cmd, Frame& f, const SceneParams& sp) {
    const bool rt = ctx->rtSupported;
    const int mode = rt ? sp.mode : LightClassic;
    uploadMeshes(cmd, f);

    // Camera in render space (camera at the origin) to keep float precision far from spawn.
    glm::vec3 fwd(std::sin(sp.yaw) * std::cos(sp.pitch), std::sin(sp.pitch), -std::cos(sp.yaw) * std::cos(sp.pitch));
    glm::mat4 view = glm::lookAt(glm::vec3(0), fwd, glm::vec3(0, 1, 0));
    float fy = 1.0f / std::tan(glm::radians(sp.fov) * 0.5f);
    glm::mat4 proj(0.0f);  // infinite reverse-Z with a Vulkan Y flip
    proj[0][0] = fy / ((float)renderW / renderH);
    proj[1][1] = -fy;
    proj[2][3] = -1;
    proj[3][2] = NEAR_PLANE;
    glm::mat4 viewProj = proj * view, invViewProj = glm::inverse(viewProj);

    const int ccx = (int)std::floor(sp.camPos.x / CW), ccz = (int)std::floor(sp.camPos.z / CW);
    const int rd = sp.renderDist;
    auto chunkOffset = [&](const GpuChunk& c) {
        return glm::vec3((float)(c.cx * CW - sp.camPos.x), (float)-sp.camPos.y, (float)(c.cz * CW - sp.camPos.z));
    };

    // Evict meshes that drifted out of range.
    for (auto it = chunks.begin(); it != chunks.end();) {
        if (std::max(std::abs(it->second.cx - ccx), std::abs(it->second.cz - ccz)) > rd + 3) {
            destroyChunk(it->second, f);
            it = chunks.erase(it);
        } else {
            ++it;
        }
    }

    // Frustum culling against the four side planes.
    glm::vec4 planes[4];
    for (int i = 0; i < 2; ++i) {
        glm::vec4 row3(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]);
        glm::vec4 row(viewProj[0][i], viewProj[1][i], viewProj[2][i], viewProj[3][i]);
        planes[i * 2] = row3 + row;
        planes[i * 2 + 1] = row3 - row;
    }
    std::vector<std::pair<float, const GpuChunk*>> visible;
    std::vector<const GpuChunk*> inRange;
    for (auto& [k, c] : chunks) {
        int dx = c.cx - ccx, dz = c.cz - ccz;
        if (dx * dx + dz * dz > rd * rd) continue;
        inRange.push_back(&c);
        glm::vec3 lo = chunkOffset(c) + glm::vec3(0, c.minY, 0), hi = chunkOffset(c) + glm::vec3(CW, c.maxY, CW);
        bool inside = true;
        for (auto& p : planes) {
            glm::vec3 v(p.x > 0 ? hi.x : lo.x, p.y > 0 ? hi.y : lo.y, p.z > 0 ? hi.z : lo.z);
            if (glm::dot(glm::vec3(p), v) + p.w < 0) { inside = false; break; }
        }
        if (inside) visible.push_back({glm::dot(lo + glm::vec3(8, 0, 8), lo + glm::vec3(8, 0, 8)), &c});
    }
    std::sort(visible.begin(), visible.end(), [](auto& a, auto& b) { return a.first < b.first; });

    // Sun, sky and frame constants.
    float angle = sp.timeOfDay * 6.2831853f;
    glm::vec3 sun = glm::normalize(glm::vec3(std::cos(angle), std::sin(angle), 0.3f));
    float day = glm::clamp(sun.y * 3.0f + 0.3f, 0.0f, 1.0f);
    float sunset = std::exp(-std::abs(sun.y) * 6.0f);
    FrameUBO u{};
    u.viewProj = viewProj;
    u.invViewProj = invViewProj;
    if (sun.y > -0.05f) {
        float s = glm::smoothstep(-0.05f, 0.15f, sun.y);
        u.sunDir = glm::vec4(sun, day);
        u.sunColor = glm::vec4(glm::mix(glm::vec3(1.0f, 0.93f, 0.82f), glm::vec3(1.0f, 0.55f, 0.3f), sunset) * 3.0f * s, 1);
    } else {
        float s = glm::smoothstep(0.05f, 0.25f, -sun.y);
        u.sunDir = glm::vec4(-sun, 0);
        u.sunColor = glm::vec4(glm::vec3(0.035f, 0.045f, 0.08f) * s, 1);
    }
    glm::vec3 zenith = glm::mix(glm::vec3(0.004f, 0.006f, 0.018f), glm::vec3(0.22f, 0.42f, 0.85f), day);
    glm::vec3 horizon = glm::mix(glm::vec3(0.015f, 0.02f, 0.045f), glm::vec3(0.62f, 0.76f, 0.95f), day);
    horizon = glm::mix(horizon, glm::vec3(0.95f, 0.5f, 0.28f), sunset * 0.7f);
    u.skyZenith = glm::vec4(zenith, 1);
    u.skyHorizon = glm::vec4(horizon, 1);
    float fogEnd = rd * (float)CW;
    u.fog = glm::vec4(fogEnd * 0.7f, fogEnd, sp.underwater ? 1.0f : 0.0f, sp.seconds);

    // Nearby block lights for the ray traced modes.
    uint32_t lightCount = 0;
    if (rt && mode != LightClassic) {
        std::vector<std::pair<float, LightGpu>> lights;
        for (const GpuChunk* c : inRange) {
            glm::vec3 o = chunkOffset(*c);
            if (glm::length(glm::vec2(o.x + 8, o.z + 8)) > 72) continue;
            for (auto& l : c->lights) {
                glm::vec3 p = o + glm::vec3(l.x + 0.5f, l.y + 0.5f, l.z + 0.5f);
                const auto& e = blockInfo((uint8_t)l.w).emit;
                lights.push_back({glm::dot(p, p), {glm::vec4(p, 1), glm::vec4(glm::vec3(e[0], e[1], e[2]) / 15.0f * LIGHT_INTENSITY, 0)}});
            }
        }
        size_t n = std::min<size_t>(lights.size(), MAX_LIGHTS);
        std::partial_sort(lights.begin(), lights.begin() + n, lights.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < n; ++i) ((LightGpu*)f.lights.mapped)[i] = lights[i].second;
        lightCount = (uint32_t)n;
    }
    u.info = glm::uvec4(mode, (uint32_t)frameIndex, lightCount, 0);
    std::memcpy(f.ubo.mapped, &u, sizeof(u));

    // Top-level acceleration structure over every chunk in range.
    if (rt) {
        auto* inst = (VkAccelerationStructureInstanceKHR*)f.instances.mapped;
        auto* table = (uint64_t*)f.chunkTable.mapped;
        uint32_t n = 0;
        for (const GpuChunk* c : inRange) {
            if (!c->blas || n >= MAX_INSTANCES) continue;
            glm::vec3 o = chunkOffset(*c);
            VkAccelerationStructureInstanceKHR& i = inst[n];
            i = {};
            i.transform.matrix[0][0] = i.transform.matrix[1][1] = i.transform.matrix[2][2] = 1;
            i.transform.matrix[0][3] = o.x;
            i.transform.matrix[1][3] = o.y;
            i.transform.matrix[2][3] = o.z;
            i.instanceCustomIndex = n;
            i.mask = 0xFF;
            i.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            i.accelerationStructureReference = c->blasAddress;
            for (int g = 0; g < 3; ++g) table[3 * n + g] = c->geomAddress[g];
            ++n;
        }
        VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geom.geometry.instances.data.deviceAddress = f.instances.address;
        VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bi.geometryCount = 1;
        bi.pGeometries = &geom;
        bi.dstAccelerationStructure = f.tlas;
        bi.scratchData.deviceAddress = alignUp(f.tlasScratch.address, ctx->asScratchAlign);
        VkAccelerationStructureBuildRangeInfoKHR range{n, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* pr = &range;
        ctx->vkCmdBuildAccelerationStructuresKHR(cmd, 1, &bi, &pr);
        memoryBarrier(cmd);
    }

    // Scene pass.
    imageBarrier(cmd, hdr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    imageBarrier(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = hdr.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView = depth.view;
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAtt.clearValue.depthStencil = {0.0f, 0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {renderW, renderH}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    ri.pDepthAttachment = &depthAtt;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, renderW, renderH);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, worldLayout, 0, 1, &f.worldSet, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipe);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindIndexBuffer(cmd, indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    auto drawPass = [&](int kind, bool backToFront) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, worldPipes[kind]);
        for (size_t i = 0; i < visible.size(); ++i) {
            const GpuChunk& c = *visible[backToFront ? visible.size() - 1 - i : i].second;
            uint32_t count = c.quads[kind], first = 0;
            for (int k = 0; k < kind; ++k) first += c.quads[k];
            if (!count) continue;
            glm::vec4 off(chunkOffset(c), 0);
            VkDeviceSize zero = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &c.vb.buffer, &zero);
            vkCmdPushConstants(cmd, worldLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &off);
            vkCmdDrawIndexed(cmd, count * 6, 1, first * 6, 0, 0);
        }
    };
    drawPass(KindOpaque, false);
    drawPass(KindCutout, false);
    drawPass(KindWater, true);
    drawPass(KindTranslucent, true);
    vkCmdEndRendering(cmd);

    imageBarrier(cmd, hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    imageBarrier(cmd, depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                 VK_IMAGE_ASPECT_DEPTH_BIT);

    // Temporal accumulation for the path tracer.
    const int parity = (int)(frameIndex & 1);
    if (mode == LightPathTraced) {
        AccumPC pc;
        pc.invViewProj = invViewProj;
        pc.prevViewProj = prevViewProj;
        glm::dvec3 delta = sp.camPos - prevCam;
        bool reset = !historyValid || prevMode != mode || sp.resetAccumulation || glm::length(delta) > 8.0;
        pc.camDelta = glm::vec4(glm::vec3(delta), reset ? 1.0f : 0.0f);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumLayout, 0, 1, &accumSets[parity], 0, nullptr);
        vkCmdPushConstants(cmd, accumLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, (renderW + 7) / 8, (renderH + 7) / 8, 1);
        memoryBarrier(cmd);
        historyValid = true;
    }
    prevViewProj = viewProj;
    prevCam = sp.camPos;
    prevMode = mode;
}

void Renderer::renderUi(VkCommandBuffer cmd, Frame& f, const UiBatch& ui) {
    size_t n = std::min<size_t>(ui.verts.size(), UI_VERTS);
    if (!n) return;
    std::memcpy(f.uiVerts.mapped, ui.verts.data(), n * sizeof(UiVertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiPipe);
    float screen[2] = {(float)ctx->swapExtent.width, (float)ctx->swapExtent.height};
    vkCmdPushConstants(cmd, uiLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, screen);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f.uiVerts.buffer, &zero);
    for (const UiDraw& d : ui.draws) {
        if (d.first + d.count > n) break;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, uiLayout, 0, 1, &uiSets[d.tex], 0, nullptr);
        vkCmdDraw(cmd, d.count, 1, d.first, 0);
    }
}

void Renderer::render(const SceneParams& sp, const UiBatch& ui) {
    Frame& f = frames[frameSlot];
    VK_CHECK(vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX));
    for (auto& d : f.deletions) d();
    f.deletions.clear();

    int w, h;
    SDL_Vulkan_GetDrawableSize(ctx->window, &w, &h);
    if (w <= 0 || h <= 0) return;
    if ((uint32_t)w != ctx->swapExtent.width || (uint32_t)h != ctx->swapExtent.height || ctx->vsync != ctx->swapVsync) recreateSwapchain();
    if (sp.renderScale != currentScale) {
        vkDeviceWaitIdle(dev);
        destroyTargets();
        currentScale = sp.renderScale;
        createTargets();
    }

    uint32_t imageIndex;
    VkResult r = vkAcquireNextImageKHR(dev, ctx->swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) vkFail("vkAcquireNextImageKHR", r);
    VK_CHECK(vkResetFences(dev, 1, &f.fence));

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    if (sp.inWorld) renderWorld(cmd, f, sp);

    VkImage swapImg = ctx->swapImages[imageIndex];
    imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = ctx->swapViews[imageIndex];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, ctx->swapExtent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, ctx->swapExtent.width, ctx->swapExtent.height);

    if (sp.inWorld) {
        const bool pt = ctx->rtSupported && sp.mode == LightPathTraced;
        const bool filmic = ctx->rtSupported && sp.mode != LightClassic;
        glm::vec3 fwd(std::sin(sp.yaw) * std::cos(sp.pitch), std::sin(sp.pitch), -std::cos(sp.yaw) * std::cos(sp.pitch));
        float fy = 1.0f / std::tan(glm::radians(sp.fov) * 0.5f);
        glm::mat4 proj(0.0f);
        proj[0][0] = fy / ((float)renderW / renderH);
        proj[1][1] = -fy;
        proj[2][3] = -1;
        proj[3][2] = NEAR_PLANE;
        PostPC pc;
        pc.invViewProj = glm::inverse(proj * glm::lookAt(glm::vec3(0), fwd, glm::vec3(0, 1, 0)));
        pc.selection = glm::vec4(glm::vec3(sp.selLo - sp.camPos), sp.hasSelection ? 1.0f : 0.0f);
        pc.selSize = glm::vec4(glm::vec3(sp.selHi - sp.selLo), 0);
        float angle = sp.timeOfDay * 6.2831853f;
        float day = glm::clamp(std::sin(angle) * 3.0f + 0.3f, 0.0f, 1.0f);
        pc.params = glm::vec4(pt ? 1.0f : 0.0f, filmic ? glm::mix(3.0f, 1.0f, day) : 1.0f, filmic ? 1.0f : 0.0f,
                              2.0f / (fy * ctx->swapExtent.height));
        // Brightness 0.5 is neutral; it bends the display curve to lift or deepen the darks.
        pc.grade = glm::vec4(std::exp2((0.5f - sp.brightness) * 1.4f), 0, 0, 0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postLayout, 0, 1, &postSets[frameIndex & 1], 0, nullptr);
        vkCmdPushConstants(cmd, postLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    renderUi(cmd, f, ui);
    vkCmdEndRendering(cmd);
    imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone[imageIndex];
    VK_CHECK(vkQueueSubmit(ctx->queue, 1, &si, f.fence));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &ctx->swapchain;
    pi.pImageIndices = &imageIndex;
    r = vkQueuePresentKHR(ctx->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreateSwapchain();
    else if (r != VK_SUCCESS) vkFail("vkQueuePresentKHR", r);

    frameSlot = (frameSlot + 1) % FRAMES;
    ++frameIndex;
}
