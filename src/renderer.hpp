#pragma once
#include "ui.hpp"
#include "vk_context.hpp"
#include "world.hpp"

#include <glm/glm.hpp>

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

enum LightingMode { LightClassic, LightRayTraced, LightPathTraced, LightModeCount };

struct SceneParams {
    bool inWorld = false;
    glm::dvec3 camPos{0};
    float yaw = 0, pitch = 0, fov = 70;
    float timeOfDay = 0.3f;  // 0..1, 0.25 = noon
    float seconds = 0;
    int mode = LightClassic;
    int renderDist = 8;
    float renderScale = 1;
    float brightness = 0.5f;  // 0 moody .. 1 bright
    bool underwater = false;
    bool hasSelection = false;
    glm::dvec3 selLo{0}, selHi{1};  // world space bounds of the selected block's shape
    bool resetAccumulation = false;
};

class Renderer {
public:
    void init(VkCtx& ctx, const std::string& baseDir, const Font& font);
    void destroy();

    void queueMesh(MeshData&& m) { pendingMeshes.push_back(std::move(m)); }
    void clearWorld();
    void render(const SceneParams& sp, const UiBatch& ui);
    bool rtSupported() const { return ctx->rtSupported; }
    size_t gpuChunkCount() const { return chunks.size(); }

private:
    static constexpr int FRAMES = 2;
    static constexpr uint32_t MAX_INSTANCES = 4096, MAX_LIGHTS = 128, MAX_QUADS = 1 << 18;
    static constexpr VkDeviceSize STAGING_SIZE = 32 << 20, BLAS_SCRATCH_SIZE = 64 << 20, UI_VERTS = 1 << 17;

    struct GpuChunk {
        int cx = 0, cz = 0;
        Buffer vb;
        uint32_t quads[4]{};  // per BlockKind, stored in that order
        float minY = 0, maxY = 0;
        VkAccelerationStructureKHR blas = VK_NULL_HANDLE;
        Buffer blasBuffer;
        VkDeviceAddress blasAddress = 0, geomAddress[3]{};  // vertex data of each BLAS geometry
        std::vector<glm::ivec4> lights;
    };

    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        Buffer ubo, lights, chunkTable, instances, staging, uiVerts, tlasBuffer, tlasScratch, blasScratch;
        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        VkDescriptorSet worldSet = VK_NULL_HANDLE;
        std::vector<std::function<void()>> deletions;
    };

    VkCtx* ctx = nullptr;
    VkDevice dev = VK_NULL_HANDLE;
    std::string base;
    Frame frames[FRAMES];
    std::vector<VkSemaphore> renderDone;  // one per swapchain image
    uint32_t frameSlot = 0;
    uint64_t frameIndex = 0;

    Image blockArray, uiTextures[UiTexCount];
    Image hdr, depth, hist[2], dist[2];
    uint32_t renderW = 0, renderH = 0;
    float currentScale = 0;
    bool historyValid = false;
    glm::mat4 prevViewProj{1};
    glm::dvec3 prevCam{0};
    int prevMode = -1;

    Buffer indexBuffer;
    VkSampler nearestRepeat = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE, linearClamp = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout worldSetLayout = VK_NULL_HANDLE, accumSetLayout = VK_NULL_HANDLE, postSetLayout = VK_NULL_HANDLE,
                          uiSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet accumSets[2]{}, postSets[2]{}, uiSets[UiTexCount]{};
    VkPipelineLayout worldLayout = VK_NULL_HANDLE, accumLayout = VK_NULL_HANDLE, postLayout = VK_NULL_HANDLE,
                     uiLayout = VK_NULL_HANDLE;
    VkPipeline skyPipe = VK_NULL_HANDLE, worldPipes[4]{}, accumPipe = VK_NULL_HANDLE, postPipe = VK_NULL_HANDLE,
               uiPipe = VK_NULL_HANDLE;

    std::unordered_map<int64_t, GpuChunk> chunks;
    std::deque<MeshData> pendingMeshes;

    void loadTextures(const Font& font);
    void createDescriptors();
    void createPipelines();
    void createTargets();
    void destroyTargets();
    void writeTargetDescriptors();
    void recreateSwapchain();
    void destroyChunk(GpuChunk& c, Frame& f);
    void uploadMeshes(VkCommandBuffer cmd, Frame& f);
    void renderWorld(VkCommandBuffer cmd, Frame& f, const SceneParams& sp);
    void renderUi(VkCommandBuffer cmd, Frame& f, const UiBatch& ui);
};
