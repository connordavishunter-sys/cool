#pragma once
#include "vk_context.hpp"

#include <glm/glm.hpp>

#include <string>
#include <vector>

// NVIDIA DLSS Super Resolution and Frame Generation through NGX. Without the SDK (ORDIS_DLSS undefined) or a capable GPU it is never available.
class Dlss {
public:
    // Instance and device extensions NGX needs; the context enables the ones the driver has.
    static void requiredExtensions(std::vector<const char*>& instance, std::vector<const char*>& device);

    bool init(VkCtx& ctx, const std::string& dir);  // dir holds the DLSS library and receives NGX logs
    void shutdown();
    bool available() const { return ok; }
    bool frameGenAvailable() const { return fgOk; }

    // Quality: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance.
    VkExtent2D renderSize(int quality, VkExtent2D out) const;
    void create(VkCommandBuffer cmd, int quality, VkExtent2D in, VkExtent2D out);
    void release();
    // Inputs must be in SHADER_READ_ONLY_OPTIMAL and the output in GENERAL. Motion is in render pixels, current to previous.
    void evaluate(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& output,
                  glm::vec2 jitter, bool reset);

    // Frame generation: one interpolated frame between the previous and current backbuffer. Matrices are glm
    // (column-major), which is the row-major, row-vector layout NGX expects.
    struct FgCamera {
        glm::mat4 viewToClip, clipToPrevClip;
        glm::vec3 pos, up, right, fwd;
        float nearPlane, farPlane, fov, aspect;
    };
    void createFg(VkCommandBuffer cmd, VkExtent2D out, VkExtent2D render, VkFormat format);
    void releaseFg();
    // Same layouts as evaluate(); the interpolated output is in GENERAL.
    void evaluateFg(VkCommandBuffer cmd, const Image& backbuffer, const Image& hudless, const Image& depth, const Image& motion,
                    const Image& interp, glm::vec2 jitter, const FgCamera& cam, bool reset);

private:
    VkDevice dev = VK_NULL_HANDLE;
    bool ok = false, fgOk = false, initialized = false;
    void* fgFeature = nullptr;
    void* params = nullptr;   // NVSDK_NGX_Parameter
    void* feature = nullptr;  // NVSDK_NGX_Handle
    VkExtent2D inSize{};
};
