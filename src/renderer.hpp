#pragma once
#include "dlss.hpp"
#include "ui.hpp"
#include "vk_context.hpp"
#include "world.hpp"

#include <glm/glm.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

enum LightingMode { LightClassic, LightRayTraced, LightPathTraced, LightModeCount };
enum AntiAliasing { AaOff, AaFxaa, AaTaa, AaDlaa, AaDlssQuality, AaDlssBalanced, AaDlssPerformance, AaDlssUltraPerformance, AaCount };
inline bool isDlss(int aa) { return aa >= AaDlaa; }

// Tuning for the ray and path traced lighting modes.
struct RtSettings {
    int shadowRays = 8;          // block lights per pixel that get a shadow ray (Ray Traced)
    float rtSunSoftness = 0;     // 0..1, sun shadow penumbra (Ray Traced)
    float ambient = 1;           // sky fill light multiplier (Ray Traced)
    bool reflections = true;     // ray traced water reflections
    int bounces = 2;             // indirect bounces (Path Traced)
    int samples = 1;             // paths per pixel per frame (Path Traced)
    int history = 96;            // frames accumulated at most (Path Traced)
    float ptSunSoftness = 0.3f;  // 0..1 (Path Traced)
    float lightStrength = 1;     // block light and glow multiplier
    bool operator==(const RtSettings&) const = default;
};

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
    int aa = AaOff;
    bool frameGen = false;
    RtSettings rt;
    bool underwater = false;
    bool hasSelection = false;
    glm::dvec3 selLo{0}, selHi{1};  // world space bounds of the selected block's shape
    const std::vector<Vertex>* particles = nullptr;  // camera-relative quads
    const std::vector<Vertex>* clouds = nullptr;     // camera-relative quads
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
    bool dlssAvailable() const { return dlss.available(); }
    bool frameGenAvailable() const { return dlss.frameGenAvailable(); }
    bool frameGenActive() const { return fgActive; }
    VkExtent2D renderSize() const { return {renderW, renderH}; }

private:
    static constexpr int FRAMES = 2;
    static constexpr uint32_t MAX_INSTANCES = 4096, MAX_LIGHTS = 128, MAX_QUADS = 1 << 18, MAX_PARTICLE_QUADS = 2048, MAX_CLOUD_QUADS = 8192;
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
        Buffer ubo, lights, chunkTable, instances, staging, uiVerts, particles, clouds, tlasBuffer, tlasScratch, blasScratch;
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
    Image hdr, depth, hist[2], dist[2], motion, taa[2], upscaled;
    uint32_t renderW = 0, renderH = 0;
    float currentScale = 0;
    int currentDlss = -1;  // the DLSS mode the targets were made for, -1 without DLSS
    Dlss dlss;
    bool historyValid = false;
    glm::mat4 prevViewProj{1};
    glm::dvec3 prevCam{0};
    int prevMode = -1, prevAa = -1;
    RtSettings prevRt;

    // DLSS frame generation: every rendered frame is followed by an interpolated one. The pacer thread copies
    // both to the swapchain and presents them half a frame apart, so the main thread never waits on presentation.
    struct FgSlot {
        Image hudless, color, interp;  // UNORM images; the scene and UI are drawn through sRGB views
        VkImageView hudlessSrgb = VK_NULL_HANDLE, colorSrgb = VK_NULL_HANDLE;
        VkCommandBuffer copy[2]{};
        VkFence copyDone[2]{};
        VkSemaphore acquired[2]{};
    };
    FgSlot fg[FRAMES];
    bool currentFg = false, fgActive = false, fgWasActive = false, fgReset = false;
    Dlss::FgCamera fgCam{};
    glm::vec2 fgJitter{0};
    VkCommandPool pacerPool = VK_NULL_HANDLE;
    std::thread pacer;
    std::mutex pacerMutex;
    std::condition_variable pacerCv;
    std::deque<uint32_t> fgJobs;  // frame slots waiting to be presented
    int pacerSlot = -1;           // slot being presented
    bool pacerStop = false;
    std::atomic<bool> fgOutOfDate{false};

    Buffer indexBuffer;
    VkSampler nearestRepeat = VK_NULL_HANDLE, nearestClamp = VK_NULL_HANDLE, linearClamp = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout worldSetLayout = VK_NULL_HANDLE, accumSetLayout = VK_NULL_HANDLE, postSetLayout = VK_NULL_HANDLE,
                          uiSetLayout = VK_NULL_HANDLE, temporalSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet accumSets[2]{}, postSets[2]{}, uiSets[UiTexCount]{}, temporalSets[2]{};
    VkPipelineLayout worldLayout = VK_NULL_HANDLE, accumLayout = VK_NULL_HANDLE, postLayout = VK_NULL_HANDLE,
                     uiLayout = VK_NULL_HANDLE, temporalLayout = VK_NULL_HANDLE;
    VkPipeline skyPipe = VK_NULL_HANDLE, worldPipes[4]{}, depthPipes[2]{}, equalPipes[2]{}, accumPipe = VK_NULL_HANDLE, postPipe = VK_NULL_HANDLE,
               uiPipe = VK_NULL_HANDLE, motionPipe = VK_NULL_HANDLE, taaPipe = VK_NULL_HANDLE;

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
    void drawPost(VkCommandBuffer cmd, const SceneParams& sp);
    void pacerLoop();
    void presentFg(FgSlot& s, int which);
    void waitPacer(int slot);  // until that slot (or, with -1, every slot) has been presented
};
