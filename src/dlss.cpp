#include "dlss.hpp"

#ifdef ORDIS_DLSS
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssg_vk.h>

#include <cstdio>
#include <cstring>

namespace {
const char* const kProjectId = "6c0f3a52-9d1e-4b7a-8f25-3e4d1c9b0a71";

NVSDK_NGX_PerfQuality_Value perfQuality(int q) {
    const NVSDK_NGX_PerfQuality_Value v[] = {NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
                                             NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
                                             NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    return v[q];
}

NVSDK_NGX_Resource_VK resource(const Image& img, bool write) {
    VkImageAspectFlags aspect = img.format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    return NVSDK_NGX_Create_ImageView_Resource_VK(img.view, img.image, {aspect, 0, 1, 0, 1}, img.format, img.width, img.height, write);
}
}  // namespace

void Dlss::requiredExtensions(std::vector<const char*>& instance, std::vector<const char*>& device) {
    unsigned ni = 0, nd = 0;
    const char **ie = nullptr, **de = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ni, &ie, &nd, &de))) return;
    instance.insert(instance.end(), ie, ie + ni);
    device.insert(device.end(), de, de + nd);
}

bool Dlss::init(VkCtx& ctx, const std::string& dir) {
    dev = ctx.device;
    std::wstring wdir(dir.begin(), dir.end());
    const wchar_t* paths[] = {wdir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {paths, 1};
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", wdir.c_str(), ctx.instance,
                                                              ctx.gpu, ctx.device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &info);
    if (NVSDK_NGX_FAILED(r)) {
        std::printf("DLSS: NGX unavailable (0x%x)\n", (unsigned)r);
        return false;
    }
    initialized = true;
    NVSDK_NGX_Parameter* p = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&p))) return false;
    params = p;
    int avail = 0;
    NVSDK_NGX_Parameter_GetI(p, NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    ok = avail;
    int fg = 0;
    NVSDK_NGX_Parameter_GetI(p, NVSDK_NGX_Parameter_FrameGeneration_Available, &fg);
    fgOk = fg;
    std::printf("DLSS: %s, frame generation %s\n", ok ? "available" : "not supported on this GPU or driver", fgOk ? "available" : "unavailable");
    return ok || fgOk;
}

void Dlss::shutdown() {
    release();
    releaseFg();
    if (params) NVSDK_NGX_VULKAN_DestroyParameters((NVSDK_NGX_Parameter*)params);
    if (initialized) NVSDK_NGX_VULKAN_Shutdown1(dev);
    params = nullptr;
    initialized = ok = fgOk = false;
}

VkExtent2D Dlss::renderSize(int quality, VkExtent2D out) const {
    unsigned w = out.width, h = out.height, maxW, maxH, minW, minH;
    float sharpness;
    if (NVSDK_NGX_FAILED(NGX_DLSS_GET_OPTIMAL_SETTINGS((NVSDK_NGX_Parameter*)params, out.width, out.height, perfQuality(quality), &w, &h,
                                                       &maxW, &maxH, &minW, &minH, &sharpness)) || !w || !h)
        return out;
    return {w, h};
}

void Dlss::create(VkCommandBuffer cmd, int quality, VkExtent2D in, VkExtent2D out) {
    release();
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = in.width;
    cp.Feature.InHeight = in.height;
    cp.Feature.InTargetWidth = out.width;
    cp.Feature.InTargetHeight = out.height;
    cp.Feature.InPerfQualityValue = perfQuality(quality);
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Handle* h = nullptr;
    NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(dev, cmd, 1, 1, &h, (NVSDK_NGX_Parameter*)params, &cp);
    if (NVSDK_NGX_FAILED(r)) std::printf("DLSS: feature creation failed (0x%x)\n", (unsigned)r);
    feature = h;
    inSize = in;
}

void Dlss::release() {
    if (feature) NVSDK_NGX_VULKAN_ReleaseFeature((NVSDK_NGX_Handle*)feature);
    feature = nullptr;
}

void Dlss::evaluate(VkCommandBuffer cmd, const Image& color, const Image& depth, const Image& motion, const Image& output, glm::vec2 jitter,
                    bool reset) {
    if (!feature) return;
    NVSDK_NGX_Resource_VK c = resource(color, false), d = resource(depth, false), m = resource(motion, false), o = resource(output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &c;
    ep.Feature.pInOutput = &o;
    ep.pInDepth = &d;
    ep.pInMotionVectors = &m;
    ep.InJitterOffsetX = jitter.x;
    ep.InJitterOffsetY = jitter.y;
    ep.InRenderSubrectDimensions = {inSize.width, inSize.height};
    ep.InReset = reset;
    ep.InMVScaleX = ep.InMVScaleY = 1;
    NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, (NVSDK_NGX_Handle*)feature, (NVSDK_NGX_Parameter*)params, &ep);
}

void Dlss::createFg(VkCommandBuffer cmd, VkExtent2D out, VkExtent2D render, VkFormat format) {
    releaseFg();
    NVSDK_NGX_DLSSG_Create_Params cp{};
    cp.Width = out.width;
    cp.Height = out.height;
    cp.NativeBackbufferFormat = format;
    cp.RenderWidth = render.width;
    cp.RenderHeight = render.height;
    NVSDK_NGX_Handle* h = nullptr;
    NVSDK_NGX_Result r = NGX_VK_CREATE_DLSSG(cmd, 1, 1, &h, (NVSDK_NGX_Parameter*)params, &cp);
    if (NVSDK_NGX_FAILED(r)) std::printf("DLSS: frame generation creation failed (0x%x)\n", (unsigned)r);
    fgFeature = h;
}

void Dlss::releaseFg() {
    if (fgFeature) NVSDK_NGX_VULKAN_ReleaseFeature((NVSDK_NGX_Handle*)fgFeature);
    fgFeature = nullptr;
}

void Dlss::evaluateFg(VkCommandBuffer cmd, const Image& backbuffer, const Image& hudless, const Image& depth, const Image& motion,
                      const Image& interp, glm::vec2 jitter, const FgCamera& cam, bool reset) {
    if (!fgFeature) return;
    NVSDK_NGX_Resource_VK b = resource(backbuffer, false), hl = resource(hudless, false), d = resource(depth, false),
                          m = resource(motion, false), o = resource(interp, true);
    NVSDK_NGX_VK_DLSSG_Eval_Params ep{};
    ep.pBackbuffer = &b;
    ep.pHudless = &hl;
    ep.pDepth = &d;
    ep.pMVecs = &m;
    ep.pOutputInterpFrame = &o;
    NVSDK_NGX_DLSSG_Opt_Eval_Params op{};
    auto mat = [](float (&dst)[4][4], const glm::mat4& src) { std::memcpy(dst, &src, sizeof dst); };
    mat(op.cameraViewToClip, cam.viewToClip);
    mat(op.clipToCameraView, glm::inverse(cam.viewToClip));
    mat(op.clipToLensClip, glm::mat4(1));
    mat(op.clipToPrevClip, cam.clipToPrevClip);
    mat(op.prevClipToClip, glm::inverse(cam.clipToPrevClip));
    op.jitterOffset[0] = jitter.x;
    op.jitterOffset[1] = jitter.y;
    op.mvecScale[0] = op.mvecScale[1] = 1;
    for (int i = 0; i < 3; ++i) {
        op.cameraPos[i] = cam.pos[i];
        op.cameraUp[i] = cam.up[i];
        op.cameraRight[i] = cam.right[i];
        op.cameraFwd[i] = cam.fwd[i];
    }
    op.cameraNear = cam.nearPlane;
    op.cameraFar = cam.farPlane;
    op.cameraFOV = cam.fov;
    op.cameraAspectRatio = cam.aspect;
    op.depthInverted = true;
    op.cameraMotionIncluded = true;
    op.reset = reset;
    NVSDK_NGX_Result r = NGX_VK_EVALUATE_DLSSG(cmd, (NVSDK_NGX_Handle*)fgFeature, (NVSDK_NGX_Parameter*)params, &ep, &op);
    if (NVSDK_NGX_FAILED(r)) std::printf("DLSS: frame generation failed (0x%x)\n", (unsigned)r);
}

#else

void Dlss::requiredExtensions(std::vector<const char*>&, std::vector<const char*>&) {}
bool Dlss::init(VkCtx&, const std::string&) { return false; }
void Dlss::shutdown() {}
VkExtent2D Dlss::renderSize(int, VkExtent2D out) const { return out; }
void Dlss::create(VkCommandBuffer, int, VkExtent2D, VkExtent2D) {}
void Dlss::release() {}
void Dlss::evaluate(VkCommandBuffer, const Image&, const Image&, const Image&, const Image&, glm::vec2, bool) {}
void Dlss::createFg(VkCommandBuffer, VkExtent2D, VkExtent2D, VkFormat) {}
void Dlss::releaseFg() {}
void Dlss::evaluateFg(VkCommandBuffer, const Image&, const Image&, const Image&, const Image&, const Image&, glm::vec2, const FgCamera&, bool) {}

#endif
