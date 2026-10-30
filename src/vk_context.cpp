#include "vk_context.hpp"

#include "dlss.hpp"

#include <SDL2/SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

void vkFail(const char* what, VkResult r) {
    std::fprintf(stderr, "Vulkan error %d in %s\n", (int)r, what);
    std::abort();
}

static bool hasExtensions(VkPhysicalDevice gpu, const std::vector<const char*>& wanted) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, exts.data());
    for (const char* w : wanted)
        if (std::none_of(exts.begin(), exts.end(), [&](auto& e) { return !std::strcmp(e.extensionName, w); }))
            return false;
    return true;
}

void VkCtx::init(SDL_Window* w) {
    window = w;
    unsigned extCount = 0;
    SDL_Vulkan_GetInstanceExtensions(w, &extCount, nullptr);
    std::vector<const char*> instExts(extCount);
    SDL_Vulkan_GetInstanceExtensions(w, &extCount, instExts.data());
    std::vector<const char*> dlssInst, dlssDev;
    Dlss::requiredExtensions(dlssInst, dlssDev);
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
    for (const char* e : dlssInst)
        if (std::any_of(available.begin(), available.end(), [&](auto& a) { return !std::strcmp(a.extensionName, e); }) &&
            std::none_of(instExts.begin(), instExts.end(), [&](const char* x) { return !std::strcmp(x, e); }))
            instExts.push_back(e);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Ordis";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (std::getenv("ORDIS_VALIDATION")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
    if (!SDL_Vulkan_CreateSurface(w, instance, &surface)) throw std::runtime_error(SDL_GetError());

    const std::vector<const char*> rtExts = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME,
                                             VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};

    // Pick the best GPU with a graphics+present queue, preferring discrete and ray-query capable ones.
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(instance, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(instance, &gpuCount, gpus.data());
    int bestScore = -1;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3 || !hasExtensions(g, {VK_KHR_SWAPCHAIN_EXTENSION_NAME})) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qs.data());
        for (uint32_t i = 0; i < qn; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface, &present);
            if (!present || !(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !(qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
            int score = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 0) + (hasExtensions(g, rtExts) ? 500 : 0);
            if (score > bestScore) { bestScore = score; gpu = g; queueFamily = i; }
            break;
        }
    }
    if (!gpu) throw std::runtime_error("No Vulkan 1.3 GPU found");
    vkGetPhysicalDeviceProperties(gpu, &props);
    vkGetPhysicalDeviceMemoryProperties(gpu, &memProps);

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &asf};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    bool rtExtsOk = hasExtensions(gpu, rtExts);
    if (rtExtsOk) f13.pNext = &rqf;
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    rtSupported = rtExtsOk && asf.accelerationStructure && rqf.rayQuery && f12.bufferDeviceAddress && f2.features.shaderInt64;
    if (std::getenv("ORDIS_NO_RT")) rtSupported = false;

    // Enable only what we use.
    VkPhysicalDeviceAccelerationStructureFeaturesKHR easf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    easf.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceRayQueryFeaturesKHR erqf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &easf};
    erqf.rayQuery = VK_TRUE;
    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, rtSupported ? &erqf : nullptr};
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    e12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    e2.features.shaderInt64 = rtSupported ? VK_TRUE : VK_FALSE;

    std::vector<const char*> devExts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (rtSupported) devExts.insert(devExts.end(), rtExts.begin(), rtExts.end());
    for (const char* e : dlssDev)
        if (hasExtensions(gpu, {e}) && std::none_of(devExts.begin(), devExts.end(), [&](const char* x) { return !std::strcmp(x, e); }))
            devExts.push_back(e);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    if (rtSupported) {
#define LOAD(fn) fn = (PFN_##fn)vkGetDeviceProcAddr(device, #fn)
        LOAD(vkCreateAccelerationStructureKHR);
        LOAD(vkDestroyAccelerationStructureKHR);
        LOAD(vkGetAccelerationStructureBuildSizesKHR);
        LOAD(vkCmdBuildAccelerationStructuresKHR);
        LOAD(vkGetAccelerationStructureDeviceAddressKHR);
#undef LOAD
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asp};
        vkGetPhysicalDeviceProperties2(gpu, &p2);
        asScratchAlign = std::max(asp.minAccelerationStructureScratchOffsetAlignment, 256u);
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &cmdPool));

    std::printf("GPU: %s, ray tracing: %s\n", props.deviceName, rtSupported ? "yes" : "no");
    createSwapchain();
}

void VkCtx::createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps);
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
    VkSurfaceFormatKHR fmt = formats[0];
    for (auto f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { fmt = f; break; }

    // Without vsync prefer mailbox (uncapped, no tearing), then immediate; FIFO is always available.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    swapVsync = vsync;
    if (!vsync || std::getenv("ORDIS_NOVSYNC")) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &n, nullptr);
        std::vector<VkPresentModeKHR> modes(n);
        vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &n, modes.data());
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR || (m == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR)) mode = m;
    }

    int w, h;
    SDL_Vulkan_GetDrawableSize(window, &w, &h);
    swapExtent = caps.currentExtent;
    if (swapExtent.width == UINT32_MAX) {
        swapExtent.width = std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
        swapExtent.height = std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    uint32_t count = std::max(caps.minImageCount + 1, 3u);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = count;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = swapExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain));
    swapFormat = fmt.format;

    vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
    swapImages.resize(n);
    vkGetSwapchainImagesKHR(device, swapchain, &n, swapImages.data());
    for (auto img : swapImages) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = swapFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        VK_CHECK(vkCreateImageView(device, &vci, nullptr, &v));
        swapViews.push_back(v);
    }
}

void VkCtx::destroySwapchain() {
    for (auto v : swapViews) vkDestroyImageView(device, v, nullptr);
    swapViews.clear();
    swapImages.clear();
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
}

void VkCtx::destroy() {
    destroySwapchain();
    vkDestroyCommandPool(device, cmdPool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
}

static uint32_t memoryType(const VkPhysicalDeviceMemoryProperties& mp, uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("No suitable memory type");
}

Buffer VkCtx::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) mai.pNext = &flags;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(memProps, req.memoryTypeBits, memory);
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    if (memory & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VK_CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped));
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = b.buffer;
        b.address = vkGetBufferDeviceAddress(device, &ai);
    }
    return b;
}

void VkCtx::destroyBuffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = {};
}

Image VkCtx::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, uint32_t mips, uint32_t layers,
                         VkImageAspectFlags aspect, bool array, VkImageCreateFlags flags) {
    Image img;
    img.format = format;
    img.width = w; img.height = h; img.mips = mips; img.layers = layers;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = flags;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    VK_CHECK(vkCreateImage(device, &ici, nullptr, &img.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(memProps, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &img.memory));
    VK_CHECK(vkBindImageMemory(device, img.image, img.memory, 0));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, mips, 0, layers};
    VK_CHECK(vkCreateImageView(device, &vci, nullptr, &img.view));
    return img;
}

void VkCtx::destroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = {};
}

void VkCtx::submitNow(const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    record(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    std::lock_guard lock(queueMutex);
    VK_CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}

VkShaderModule VkCtx::loadShader(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Missing shader " + path);
    std::vector<char> code(f.tellg());
    f.seekg(0);
    f.read(code.data(), code.size());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = (const uint32_t*)code.data();
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
    return m;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.image = image;
    b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

void memoryBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}
