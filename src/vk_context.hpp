#pragma once
#include <SDL2/SDL.h>
#include <vulkan/vulkan.h>

#include <functional>
#include <mutex>
#include <string>
#include <vector>

[[noreturn]] void vkFail(const char* what, VkResult r);
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) vkFail(#x, r_); } while (0)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mips = 1, layers = 1;
};

struct VkCtx {
    SDL_Window* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memProps{};
    VkCommandPool cmdPool = VK_NULL_HANDLE;

    bool rtSupported = false;
    uint32_t asScratchAlign = 256;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;

    bool vsync = true;      // requested; the renderer recreates the swapchain when it changes
    bool swapVsync = true;  // what the current swapchain was created with
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkImageView> swapViews;
    std::mutex queueMutex;  // the frame generation pacer presents from its own thread

    void init(SDL_Window* window);
    void destroy();
    void createSwapchain();
    void destroySwapchain();

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory);
    void destroyBuffer(Buffer& b);
    Image createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, uint32_t mips = 1,
                      uint32_t layers = 1, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool array = false,
                      VkImageCreateFlags flags = 0);
    void destroyImage(Image& img);
    void submitNow(const std::function<void(VkCommandBuffer)>& record);
    VkShaderModule loadShader(const std::string& path);
};

// Coarse but always-correct synchronisation helpers.
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
void memoryBarrier(VkCommandBuffer cmd);
