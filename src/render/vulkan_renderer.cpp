// vulkan_renderer.cpp — Vulkan IRenderer implementation.
// Compiles GLSL shaders via glslang at runtime (or pre-compiled SPIR-V).
// VMA is used for all GPU memory allocation.

#ifndef _WIN32

#include "vulkan_renderer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

// glslang for runtime GLSL → SPIR-V compilation
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>

// VMA implementation (include once in a .cpp)
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace idhmfis {

// ---------------------------------------------------------------------------
// GLSL shader sources
// ---------------------------------------------------------------------------

// Beam compute shader — accumulates Gaussian beam profiles into RGBA16F storage
static const char* kBeamComputeGLSL = R"GLSL(
#version 460
#extension GL_EXT_shader_explicit_arithmetic_types : enable

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

struct LaserPoint {
    vec2  pos;
    vec3  color;
    int   blanked;
    float pad;
};

layout(rgba16f, set = 0, binding = 0) uniform image2D gOutput;

layout(set = 0, binding = 1) readonly buffer PointBuffer {
    LaserPoint points[];
} gPointBuf;

layout(push_constant) uniform PC {
    uint  pointCount;
    float beamThickness;
    uint  width;
    uint  height;
} pc;

vec2 ndcToPixel(vec2 ndc) {
    float px =  (ndc.x * 0.5 + 0.5) * float(pc.width);
    float py = (-ndc.y * 0.5 + 0.5) * float(pc.height);
    return vec2(px, py);
}

float dist2PointSegment(vec2 P, vec2 A, vec2 B) {
    vec2 AB = B - A;
    vec2 AP = P - A;
    float t = dot(AP, AB) / max(dot(AB, AB), 1e-8);
    t = clamp(t, 0.0, 1.0);
    vec2 closest = A + t * AB;
    vec2 diff = P - closest;
    return dot(diff, diff);
}

vec3 colorAtT(vec2 P, vec2 A, vec2 B, vec3 cA, vec3 cB) {
    vec2 AB = B - A;
    vec2 AP = P - A;
    float t = dot(AP, AB) / max(dot(AB, AB), 1e-8);
    t = clamp(t, 0.0, 1.0);
    return mix(cA, cB, t);
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= pc.width || id.y >= pc.height) return;

    vec2 pixel = vec2(float(id.x) + 0.5, float(id.y) + 0.5);

    float sigma    = max(pc.beamThickness * 0.5, 0.5);
    float inv2sig2 = 1.0 / (2.0 * sigma * sigma);
    float cutoff2  = (3.0 * sigma) * (3.0 * sigma);

    vec4 accum = vec4(0.0);

    for (uint i = 0; i + 1 < pc.pointCount; ++i) {
        LaserPoint A = gPointBuf.points[i];
        LaserPoint B = gPointBuf.points[i + 1];

        if (A.blanked != 0 || B.blanked != 0) continue;

        vec2 pA = ndcToPixel(A.pos);
        vec2 pB = ndcToPixel(B.pos);

        float margin = 3.0 * sigma;
        if (float(id.x) < min(pA.x, pB.x) - margin ||
            float(id.x) > max(pA.x, pB.x) + margin ||
            float(id.y) < min(pA.y, pB.y) - margin ||
            float(id.y) > max(pA.y, pB.y) + margin) continue;

        float d2 = dist2PointSegment(pixel, pA, pB);
        if (d2 > cutoff2) continue;

        float gaussian = exp(-d2 * inv2sig2);
        vec3 col = colorAtT(pixel, pA, pB, A.color, B.color);
        accum.rgb += col * gaussian;
    }

    vec4 existing = imageLoad(gOutput, ivec2(id));
    imageStore(gOutput, ivec2(id), existing + vec4(accum.rgb, 0.0));
}
)GLSL";

// Post-process compute shader
static const char* kPostComputeGLSL = R"GLSL(
#version 460

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D gHDR;
layout(set = 0, binding = 1) uniform sampler2D gBloomTmp;
layout(rgba16f, set = 0, binding = 2) uniform image2D gBloomOut;
layout(rgba8,   set = 0, binding = 3) uniform image2D gSDR;

layout(push_constant) uniform PC {
    uint  width;
    uint  height;
    float bloomRadius;
    float hazeDensity;
    float exposure;
    uint  filmGrain;
    float grainAmount;
    uint  frameCounter;
    uint  passMode;   // 0=bloom_h, 1=bloom_v, 2=compose
} pc;

float hash(uvec2 p, uint seed) {
    uint n = p.x * 1619u + p.y * 31337u + seed * 1013904223u;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = n ^ (n >> 16u);
    return float(n) / float(0xFFFFFFFFu);
}

vec3 reinhardExt(vec3 v, float maxWhite) {
    vec3 num = v * (1.0 + v / (maxWhite * maxWhite));
    return num / (1.0 + v);
}

float gaussWeight(float x, float sigma) {
    return exp(-(x * x) / (2.0 * sigma * sigma));
}

void bloomHorizontal(uvec2 px) {
    float sigma  = max(pc.bloomRadius, 0.5);
    int   radius = int(ceil(sigma * 3.0));
    vec3  sum    = vec3(0.0);
    float wsum   = 0.0;
    for (int d = -radius; d <= radius; ++d) {
        int sx = clamp(int(px.x) + d, 0, int(pc.width) - 1);
        float w   = gaussWeight(float(d), sigma);
        vec3  tap = texelFetch(gHDR, ivec2(sx, px.y), 0).rgb;
        vec3  bright = max(tap - 1.0, 0.0);
        sum  += bright * w;
        wsum += w;
    }
    imageStore(gBloomOut, ivec2(px), vec4(sum / max(wsum, 1e-6), 0.0));
}

void bloomVertical(uvec2 px) {
    float sigma  = max(pc.bloomRadius, 0.5);
    int   radius = int(ceil(sigma * 3.0));
    vec3  sum    = vec3(0.0);
    float wsum   = 0.0;
    for (int d = -radius; d <= radius; ++d) {
        int sy = clamp(int(px.y) + d, 0, int(pc.height) - 1);
        float w   = gaussWeight(float(d), sigma);
        sum  += texelFetch(gBloomTmp, ivec2(px.x, sy), 0).rgb * w;
        wsum += w;
    }
    imageStore(gBloomOut, ivec2(px), vec4(sum / max(wsum, 1e-6), 0.0));
}

void compose(uvec2 px) {
    vec3  hdr   = texelFetch(gHDR,      ivec2(px), 0).rgb;
    vec3  bloom = texelFetch(gBloomTmp, ivec2(px), 0).rgb;
    vec3  color = hdr + bloom;

    vec2  uv      = (vec2(px) + 0.5) / vec2(pc.width, pc.height);
    vec2  centred = uv * 2.0 - 1.0;
    float dist    = length(centred);
    float haze    = exp(-dist * dist * pc.hazeDensity * 2.0);
    vec3  hazecol = vec3(0.06, 0.08, 0.12) * pc.hazeDensity;
    color = color * (1.0 - pc.hazeDensity * 0.15) + hazecol * (1.0 - haze);

    color *= pc.exposure;
    color  = reinhardExt(color, 4.0);
    color  = pow(max(color, 0.0), vec3(1.0 / 2.2));

    if (pc.filmGrain != 0u) {
        float noise = hash(px, pc.frameCounter) * 2.0 - 1.0;
        color += noise * pc.grainAmount;
    }

    color = clamp(color, 0.0, 1.0);
    imageStore(gSDR, ivec2(px), vec4(color, 1.0));
}

void main() {
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= pc.width || id.y >= pc.height) return;

    if      (pc.passMode == 0u) bloomHorizontal(id);
    else if (pc.passMode == 1u) bloomVertical(id);
    else                        compose(id);
}
)GLSL";

// ---------------------------------------------------------------------------
// glslang helper — compile GLSL → SPIR-V words
// ---------------------------------------------------------------------------
static bool glsl_to_spirv(const char* src, VkShaderStageFlagBits stage,
                            std::vector<uint32_t>& spirv_out)
{
    static bool glslang_inited = false;
    if (!glslang_inited) {
        glslang::InitializeProcess();
        glslang_inited = true;
    }

    EShLanguage lang;
    switch (stage) {
        case VK_SHADER_STAGE_VERTEX_BIT:   lang = EShLangVertex;  break;
        case VK_SHADER_STAGE_FRAGMENT_BIT: lang = EShLangFragment; break;
        case VK_SHADER_STAGE_COMPUTE_BIT:  lang = EShLangCompute; break;
        default: return false;
    }

    glslang::TShader shader(lang);
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 460);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

    TBuiltInResource resources = glslang::DefaultTBuiltInResource;
    EShMessages msg = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(&resources, 460, false, msg)) {
        fprintf(stderr, "[Vulkan] GLSL parse error:\n%s\n%s\n",
                shader.getInfoLog(), shader.getInfoDebugLog());
        return false;
    }

    glslang::TProgram prog;
    prog.addShader(&shader);
    if (!prog.link(msg)) {
        fprintf(stderr, "[Vulkan] GLSL link error:\n%s\n", prog.getInfoLog());
        return false;
    }

    glslang::GlslangToSpv(*prog.getIntermediate(lang), spirv_out);
    return true;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
VulkanRenderer::VulkanRenderer()  = default;
VulkanRenderer::~VulkanRenderer() {
    if (initialised_) shutdown();
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool VulkanRenderer::init(void* native_window_handle, const RenderConfig& cfg) {
    cfg_           = cfg;
    native_handle_ = native_window_handle;

    if (!create_instance())                  return false;
    if (!create_surface(native_handle_))     return false;
    if (!pick_physical_device())             return false;
    if (!create_logical_device())            return false;
    if (!create_vma_allocator())             return false;
    if (surface_ != VK_NULL_HANDLE &&
        !create_swap_chain())                return false;
    if (!create_command_pool_and_buffers())  return false;
    if (!create_sync_objects())              return false;
    if (!create_hdr_image())                 return false;
    if (!create_sdr_image())                 return false;
    if (!create_bloom_tmp_image())           return false;
    if (!create_readback_buffer())           return false;
    if (!create_point_buffer(8192))          return false;
    if (!create_descriptor_sets())           return false;
    if (!create_pipelines())                 return false;

    initialised_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// create_instance
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_instance() {
    VkApplicationInfo app_info{};
    app_info.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName   = "IDHMFIS";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName        = "IDHMFIS";
    app_info.apiVersion         = VK_API_VERSION_1_2;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef __APPLE__
        "VK_EXT_metal_surface",
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#elif defined(VK_USE_PLATFORM_XCB_KHR)
        VK_KHR_XCB_SURFACE_EXTENSION_NAME,
#endif
    };

    std::vector<const char*> layers;
#ifdef _DEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    VkInstanceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo        = &app_info;
    ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.data();
#ifdef __APPLE__
    ci.flags                   = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

    if (vkCreateInstance(&ci, nullptr, &instance_) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkCreateInstance failed.\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// create_surface — platform-specific
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_surface(void* native_handle) {
    if (!native_handle) {
        // Headless mode — no surface
        return true;
    }
#ifdef __APPLE__
    // native_handle is a CAMetalLayer*
    VkMetalSurfaceCreateInfoEXT ci{};
    ci.sType  = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT;
    ci.pLayer = native_handle;
    auto fn = (PFN_vkCreateMetalSurfaceEXT)
              vkGetInstanceProcAddr(instance_, "vkCreateMetalSurfaceEXT");
    if (!fn || fn(instance_, &ci, nullptr, &surface_) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] Metal surface creation failed.\n");
        return false;
    }
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    // XCB — native_handle expected to be xcb_connection_t* encoded with window in high bits
    // Caller must provide proper surface creation; simplified here.
    fprintf(stderr, "[Vulkan] XCB surface creation not implemented in this build.\n");
    return false;
#endif
    return true;
}

// ---------------------------------------------------------------------------
// pick_physical_device
// ---------------------------------------------------------------------------
bool VulkanRenderer::pick_physical_device() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        fprintf(stderr, "[Vulkan] No physical devices found.\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(instance_, &count, devs.data());

    for (auto d : devs) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);

        // Find queue families
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qcount, qprops.data());

        for (uint32_t i = 0; i < qcount; ++i) {
            if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                graphics_family_ = i;
            }
            if (surface_ != VK_NULL_HANDLE) {
                VkBool32 present_support = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present_support);
                if (present_support) present_family_ = i;
            } else {
                present_family_ = graphics_family_;
            }
            if (graphics_family_ != UINT32_MAX && present_family_ != UINT32_MAX) break;
        }

        if (graphics_family_ != UINT32_MAX) {
            phys_device_ = d;
            fprintf(stderr, "[Vulkan] Using device: %s\n", props.deviceName);
            break;
        }
    }

    return phys_device_ != VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// create_logical_device
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_logical_device() {
    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_cis;

    auto add_queue = [&](uint32_t family) {
        for (auto& q : queue_cis) if (q.queueFamilyIndex == family) return;
        VkDeviceQueueCreateInfo qi{};
        qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = family;
        qi.queueCount       = 1;
        qi.pQueuePriorities = &priority;
        queue_cis.push_back(qi);
    };
    add_queue(graphics_family_);
    if (present_family_ != graphics_family_) add_queue(present_family_);

    std::vector<const char*> extensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
#ifdef __APPLE__
    extensions.push_back("VK_KHR_portability_subset");
#endif

    VkPhysicalDeviceFeatures feats{};
    feats.shaderStorageImageExtendedFormats = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount    = static_cast<uint32_t>(queue_cis.size());
    ci.pQueueCreateInfos       = queue_cis.data();
    ci.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.pEnabledFeatures        = &feats;

    if (vkCreateDevice(phys_device_, &ci, nullptr, &device_) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkCreateDevice failed.\n");
        return false;
    }

    vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
    vkGetDeviceQueue(device_, present_family_,  0, &present_queue_);
    return true;
}

// ---------------------------------------------------------------------------
// create_vma_allocator
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_vma_allocator() {
    VmaAllocatorCreateInfo ai{};
    ai.physicalDevice = phys_device_;
    ai.device         = device_;
    ai.instance       = instance_;
    ai.vulkanApiVersion = VK_API_VERSION_1_2;

    if (vmaCreateAllocator(&ai, &allocator_) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vmaCreateAllocator failed.\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// create_swap_chain
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_swap_chain() {
    if (surface_ == VK_NULL_HANDLE) return true; // headless

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_device_, surface_, &caps);

    uint32_t fcount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_device_, surface_, &fcount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_device_, surface_, &fcount, formats.data());

    sc_format_ = formats[0].format;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sc_format_ = f.format;
            break;
        }
    }

    sc_extent_ = caps.currentExtent;
    if (sc_extent_.width == UINT32_MAX) {
        sc_extent_.width  = static_cast<uint32_t>(cfg_.width);
        sc_extent_.height = static_cast<uint32_t>(cfg_.height);
    }

    uint32_t image_count = std::max(caps.minImageCount + 1,
                                     std::min(3u, caps.maxImageCount));

    VkSwapchainCreateInfoKHR ci{};
    ci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface          = surface_;
    ci.minImageCount    = image_count;
    ci.imageFormat      = sc_format_;
    ci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent      = sc_extent_;
    ci.imageArrayLayers = 1;
    ci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform     = caps.currentTransform;
    ci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped          = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &ci, nullptr, &swap_chain_) != VK_SUCCESS) {
        fprintf(stderr, "[Vulkan] vkCreateSwapchainKHR failed.\n");
        return false;
    }

    uint32_t sc_count = 0;
    vkGetSwapchainImagesKHR(device_, swap_chain_, &sc_count, nullptr);
    sc_images_.resize(sc_count);
    vkGetSwapchainImagesKHR(device_, swap_chain_, &sc_count, sc_images_.data());

    sc_image_views_.resize(sc_count);
    for (uint32_t i = 0; i < sc_count; ++i) {
        VkImageViewCreateInfo iv_ci{};
        iv_ci.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv_ci.image            = sc_images_[i];
        iv_ci.viewType         = VK_IMAGE_VIEW_TYPE_2D;
        iv_ci.format           = sc_format_;
        iv_ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        iv_ci.subresourceRange.levelCount     = 1;
        iv_ci.subresourceRange.layerCount     = 1;
        vkCreateImageView(device_, &iv_ci, nullptr, &sc_image_views_[i]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// create_command_pool_and_buffers
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_command_pool_and_buffers() {
    VkCommandPoolCreateInfo ci{};
    ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.queueFamilyIndex = graphics_family_;
    ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    if (vkCreateCommandPool(device_, &ci, nullptr, &cmd_pool_) != VK_SUCCESS)
        return false;

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool        = cmd_pool_;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kMaxFrames;

    if (vkAllocateCommandBuffers(device_, &alloc, cmd_bufs_) != VK_SUCCESS)
        return false;

    return true;
}

// ---------------------------------------------------------------------------
// create_sync_objects
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_sync_objects() {
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (int i = 0; i < kMaxFrames; ++i) {
        if (vkCreateFence(device_, &fi, nullptr, &render_fence_[i])     != VK_SUCCESS) return false;
        if (vkCreateSemaphore(device_, &si, nullptr, &image_available_[i]) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(device_, &si, nullptr, &render_done_[i])     != VK_SUCCESS) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helpers: create images and buffers via VMA
// ---------------------------------------------------------------------------
static bool create_image_vma(VmaAllocator allocator, VkDevice device,
                               uint32_t w, uint32_t h, VkFormat fmt,
                               VkImageUsageFlags usage,
                               VkAllocatedImage& out) {
    VkImageCreateInfo ci{};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.extent        = { w, h, 1 };
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.format        = fmt;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage         = usage;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator, &ci, &alloc_ci, &out.image, &out.alloc, nullptr) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo iv{};
    iv.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image            = out.image;
    iv.viewType         = VK_IMAGE_VIEW_TYPE_2D;
    iv.format           = fmt;
    iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    iv.subresourceRange.levelCount = 1;
    iv.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &iv, nullptr, &out.view) != VK_SUCCESS)
        return false;

    out.format = fmt;
    out.width  = w;
    out.height = h;
    return true;
}

bool VulkanRenderer::create_hdr_image() {
    return create_image_vma(allocator_, device_,
                            static_cast<uint32_t>(cfg_.width),
                            static_cast<uint32_t>(cfg_.height),
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            hdr_image_);
}

bool VulkanRenderer::create_sdr_image() {
    return create_image_vma(allocator_, device_,
                            static_cast<uint32_t>(cfg_.width),
                            static_cast<uint32_t>(cfg_.height),
                            VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            sdr_image_);
}

bool VulkanRenderer::create_bloom_tmp_image() {
    return create_image_vma(allocator_, device_,
                            static_cast<uint32_t>(cfg_.width),
                            static_cast<uint32_t>(cfg_.height),
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            bloom_tmp_);
}

bool VulkanRenderer::create_readback_buffer() {
    UINT row_pitch = (static_cast<UINT>(cfg_.width) * 4 + 255) & ~255u;
    VkDeviceSize total = static_cast<VkDeviceSize>(row_pitch) * cfg_.height;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = total;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo alloc_info{};
    if (vmaCreateBuffer(allocator_, &bci, &alloc_ci,
                        &readback_buf_.buffer, &readback_buf_.alloc,
                        &alloc_info) != VK_SUCCESS) return false;

    readback_buf_.mapped = alloc_info.pMappedData;
    readback_buf_.size   = total;

    cpu_frame_stride_ = static_cast<int>(row_pitch);
    cpu_frame_data_.resize(static_cast<size_t>(total));
    return true;
}

bool VulkanRenderer::create_point_buffer(uint32_t max_points) {
    point_buf_capacity_ = max_points;
    VkDeviceSize size   = static_cast<VkDeviceSize>(max_points) * sizeof(VkGpuLaserPoint);

    // GPU-only storage buffer
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateBuffer(allocator_, &bci, &alloc_ci,
                        &point_buf_.buffer, &point_buf_.alloc, nullptr) != VK_SUCCESS)
        return false;
    point_buf_.size = size;

    // Staging buffer (CPU → GPU)
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    alloc_ci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo si{};
    if (vmaCreateBuffer(allocator_, &bci, &alloc_ci,
                        &point_buf_staging_.buffer, &point_buf_staging_.alloc,
                        &si) != VK_SUCCESS)
        return false;
    point_buf_staging_.mapped = si.pMappedData;
    point_buf_staging_.size   = size;

    return true;
}

// ---------------------------------------------------------------------------
// create_descriptor_sets
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_descriptor_sets() {
    // Sampler
    VkSamplerCreateInfo sci{};
    sci.sType     = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(device_, &sci, nullptr, &linear_sampler_);

    // Descriptor pool
    VkDescriptorPoolSize pool_sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,          16 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,          4 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  8 },
    };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets       = 16;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes    = pool_sizes;
    vkCreateDescriptorPool(device_, &dpi, nullptr, &desc_pool_);

    // ---- Beam descriptor set layout ----
    // binding 0: storage image (HDR output)
    // binding 1: storage buffer (points)
    {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 2;
        li.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device_, &li, nullptr, &beam_dset_layout_);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &beam_dset_layout_;
        vkAllocateDescriptorSets(device_, &ai, &beam_dset_);

        // Write descriptors
        VkDescriptorImageInfo hdr_image_info{};
        hdr_image_info.imageView   = hdr_image_.view;
        hdr_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo buf_info{};
        buf_info.buffer = point_buf_.buffer;
        buf_info.range  = VK_WHOLE_SIZE;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = beam_dset_;
        writes[0].dstBinding      = 0;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo      = &hdr_image_info;

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = beam_dset_;
        writes[1].dstBinding      = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo     = &buf_info;

        vkUpdateDescriptorSets(device_, 2, writes, 0, nullptr);
    }

    // ---- Post descriptor set layout ----
    // binding 0: sampled image (HDR input)
    // binding 1: sampled image (BloomTmp input)
    // binding 2: storage image (BloomOut)
    // binding 3: storage image (SDR output)
    {
        VkDescriptorSetLayoutBinding bindings[4]{};
        for (int i = 0; i < 4; ++i) {
            bindings[i].binding         = static_cast<uint32_t>(i);
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        VkDescriptorSetLayoutCreateInfo li{};
        li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings    = bindings;
        vkCreateDescriptorSetLayout(device_, &li, nullptr, &post_dset_layout_);

        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = desc_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &post_dset_layout_;
        vkAllocateDescriptorSets(device_, &ai, &post_dset_);

        VkDescriptorImageInfo img_infos[4]{};
        img_infos[0] = { linear_sampler_, hdr_image_.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        img_infos[1] = { linear_sampler_, bloom_tmp_.view,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        img_infos[2] = { VK_NULL_HANDLE,  bloom_tmp_.view,  VK_IMAGE_LAYOUT_GENERAL };
        img_infos[3] = { VK_NULL_HANDLE,  sdr_image_.view,  VK_IMAGE_LAYOUT_GENERAL };

        VkWriteDescriptorSet writes[4]{};
        for (int i = 0; i < 4; ++i) {
            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = post_dset_;
            writes[i].dstBinding      = static_cast<uint32_t>(i);
            writes[i].descriptorCount = 1;
            writes[i].pImageInfo      = &img_infos[i];
            writes[i].descriptorType  = (i < 2) ?
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);

        // ImGui preview uses binding 3's SDR image as a combined image sampler
        // Create a separate descriptor set for ImGui
        VkDescriptorSetLayoutBinding imgui_binding{};
        imgui_binding.binding         = 0;
        imgui_binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        imgui_binding.descriptorCount = 1;
        imgui_binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        // (ImGui layout is created by imgui_impl_vulkan — reuse it; here just store the set)
        // For now, store post_dset_ as preview — caller must bind the right sampler for ImGui.
        imgui_preview_dset_ = post_dset_;
    }

    return true;
}

// ---------------------------------------------------------------------------
// create_pipelines
// ---------------------------------------------------------------------------
bool VulkanRenderer::create_pipelines() {
    // ---- Beam pipeline ----
    {
        std::vector<uint32_t> spirv;
        if (!glsl_to_spirv(kBeamComputeGLSL, VK_SHADER_STAGE_COMPUTE_BIT, spirv))
            return false;

        VkShaderModuleCreateInfo smi{};
        smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = spirv.size() * 4;
        smi.pCode    = spirv.data();
        VkShaderModule beam_module = VK_NULL_HANDLE;
        vkCreateShaderModule(device_, &smi, nullptr, &beam_module);

        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.size       = sizeof(BeamPushConst);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &beam_dset_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pc;
        vkCreatePipelineLayout(device_, &pli, nullptr, &beam_pipeline_layout_);

        VkComputePipelineCreateInfo cpci{};
        cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.layout = beam_pipeline_layout_;
        cpci.stage  = {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, beam_module, "main", nullptr
        };

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                      &beam_pipeline_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, beam_module, nullptr);
            return false;
        }
        vkDestroyShaderModule(device_, beam_module, nullptr);
    }

    // ---- Post pipeline ----
    {
        std::vector<uint32_t> spirv;
        if (!glsl_to_spirv(kPostComputeGLSL, VK_SHADER_STAGE_COMPUTE_BIT, spirv))
            return false;

        VkShaderModuleCreateInfo smi{};
        smi.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = spirv.size() * 4;
        smi.pCode    = spirv.data();
        VkShaderModule post_module = VK_NULL_HANDLE;
        vkCreateShaderModule(device_, &smi, nullptr, &post_module);

        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pc.size       = sizeof(PostPushConst);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &post_dset_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &pc;
        vkCreatePipelineLayout(device_, &pli, nullptr, &post_pipeline_layout_);

        VkComputePipelineCreateInfo cpci{};
        cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.layout = post_pipeline_layout_;
        cpci.stage  = {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            nullptr, 0,
            VK_SHADER_STAGE_COMPUTE_BIT, post_module, "main", nullptr
        };

        if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                      &post_pipeline_) != VK_SUCCESS) {
            vkDestroyShaderModule(device_, post_module, nullptr);
            return false;
        }
        vkDestroyShaderModule(device_, post_module, nullptr);
    }

    return true;
}

// ---------------------------------------------------------------------------
// render_frame
// ---------------------------------------------------------------------------
void VulkanRenderer::render_frame(const PointBuffer& pts) {
    if (!initialised_) return;

    ++frame_counter_;
    uint32_t fi = current_frame_ % kMaxFrames;

    vkWaitForFences(device_, 1, &render_fence_[fi], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &render_fence_[fi]);

    // Upload points
    uint32_t point_count = static_cast<uint32_t>(
        std::min(pts.size(), static_cast<size_t>(point_buf_capacity_)));

    if (point_count > 0 && point_buf_staging_.mapped) {
        auto* dst = static_cast<VkGpuLaserPoint*>(point_buf_staging_.mapped);
        for (uint32_t i = 0; i < point_count; ++i) {
            dst[i].x       = pts[i].nx();
            dst[i].y       = pts[i].ny();
            dst[i].r       = pts[i].r / 255.f;
            dst[i].g       = pts[i].g / 255.f;
            dst[i].b       = pts[i].b / 255.f;
            dst[i].blanked = pts[i].blanked ? 1 : 0;
            dst[i].pad     = 0.f;
        }
        vmaFlushAllocation(allocator_, point_buf_staging_.alloc, 0, VK_WHOLE_SIZE);
    }

    VkCommandBuffer cmd = cmd_bufs_[fi];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Copy staging → GPU point buffer
    if (point_count > 0) {
        VkBufferCopy region{ 0, 0, point_count * sizeof(VkGpuLaserPoint) };
        vkCmdCopyBuffer(cmd, point_buf_staging_.buffer, point_buf_.buffer, 1, &region);

        VkBufferMemoryBarrier bmb{};
        bmb.sType         = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bmb.buffer        = point_buf_.buffer;
        bmb.size          = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // Transition HDR image to GENERAL for compute write
    transition_image(cmd, hdr_image_.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, VK_ACCESS_SHADER_WRITE_BIT);

    // Clear HDR image (via fill with zeros using a barrier trick — use vkCmdClearColorImage)
    VkClearColorValue clear_val{};
    VkImageSubresourceRange sr{};
    sr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sr.levelCount = 1;
    sr.layerCount = 1;
    vkCmdClearColorImage(cmd, hdr_image_.image,
                         VK_IMAGE_LAYOUT_GENERAL, &clear_val, 1, &sr);

    // Barrier after clear
    VkImageMemoryBarrier imb{};
    imb.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imb.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    imb.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    imb.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    imb.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    imb.image               = hdr_image_.image;
    imb.subresourceRange    = sr;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &imb);

    // ---- Beam raster pass ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, beam_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             beam_pipeline_layout_, 0, 1, &beam_dset_, 0, nullptr);
    {
        BeamPushConst pc{};
        pc.point_count    = point_count;
        pc.beam_thickness = cfg_.beam_thickness;
        pc.width          = static_cast<uint32_t>(cfg_.width);
        pc.height         = static_cast<uint32_t>(cfg_.height);
        vkCmdPushConstants(cmd, beam_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    }
    vkCmdDispatch(cmd,
                  (static_cast<uint32_t>(cfg_.width)  + 7) / 8,
                  (static_cast<uint32_t>(cfg_.height) + 7) / 8, 1);

    // Barrier: wait for beam write, then read as sampled
    {
        VkImageMemoryBarrier b{};
        b.sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask    = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask    = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout        = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image            = hdr_image_.image;
        b.subresourceRange = sr;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // Transition bloom_tmp and sdr_image to GENERAL
    transition_image(cmd, bloom_tmp_.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT);
    transition_image(cmd, sdr_image_.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     0, VK_ACCESS_SHADER_WRITE_BIT);

    // ---- Post-process: bloom H, bloom V, compose ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, post_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                             post_pipeline_layout_, 0, 1, &post_dset_, 0, nullptr);

    UINT gx = (static_cast<UINT>(cfg_.width)  + 7) / 8;
    UINT gy = (static_cast<UINT>(cfg_.height) + 7) / 8;

    auto dispatch_post = [&](uint32_t pass_mode) {
        PostPushConst pc{};
        pc.width         = static_cast<uint32_t>(cfg_.width);
        pc.height        = static_cast<uint32_t>(cfg_.height);
        pc.bloom_radius  = cfg_.bloom_radius;
        pc.haze_density  = cfg_.haze_density;
        pc.exposure      = cfg_.exposure;
        pc.film_grain    = cfg_.film_grain ? 1u : 0u;
        pc.grain_amount  = cfg_.grain_amount;
        pc.frame_counter = frame_counter_;
        pc.pass_mode     = pass_mode;
        vkCmdPushConstants(cmd, post_pipeline_layout_,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdDispatch(cmd, gx, gy, 1);

        VkMemoryBarrier mb{};
        mb.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    };

    dispatch_post(0); // bloom horizontal (HDR → bloom_tmp)
    // After bloom H, transition bloom_tmp to sampled and reopen as storage for bloom V output
    dispatch_post(1); // bloom vertical (bloom_tmp → bloom_tmp final)
    dispatch_post(2); // compose to SDR

    // Transition SDR to transfer source for readback
    transition_image(cmd, sdr_image_.image,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT);

    // Copy SDR to readback buffer
    VkBufferImageCopy copy_region{};
    copy_region.bufferRowLength   = static_cast<uint32_t>(cpu_frame_stride_) / 4;
    copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy_region.imageSubresource.layerCount = 1;
    copy_region.imageExtent = { static_cast<uint32_t>(cfg_.width),
                                 static_cast<uint32_t>(cfg_.height), 1 };
    vkCmdCopyImageToBuffer(cmd, sdr_image_.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback_buf_.buffer, 1, &copy_region);

    // Transition SDR back to GENERAL for next frame
    transition_image(cmd, sdr_image_.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{};
    submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers    = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &submit, render_fence_[fi]);

    // Wait and copy readback
    vkWaitForFences(device_, 1, &render_fence_[fi], VK_TRUE, UINT64_MAX);
    vmaInvalidateAllocation(allocator_, readback_buf_.alloc, 0, VK_WHOLE_SIZE);
    if (readback_buf_.mapped) {
        memcpy(cpu_frame_data_.data(), readback_buf_.mapped, cpu_frame_data_.size());
    }

    ++current_frame_;
}

// ---------------------------------------------------------------------------
// Remaining interface methods
// ---------------------------------------------------------------------------
void VulkanRenderer::present() {
    if (!swap_chain_) return;
    // In a real integration, present the swap chain image after blitting sdr_image_.
    // Omitted for brevity — the NDI path uses get_cpu_frame(), not present().
    uint32_t img_idx = 0;
    vkAcquireNextImageKHR(device_, swap_chain_, UINT64_MAX,
                           image_available_[current_frame_ % kMaxFrames],
                           VK_NULL_HANDLE, &img_idx);
    VkPresentInfoKHR pi{};
    pi.sType          = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.swapchainCount = 1;
    pi.pSwapchains    = &swap_chain_;
    pi.pImageIndices  = &img_idx;
    vkQueuePresentKHR(present_queue_, &pi);
}

void* VulkanRenderer::get_preview_texture() {
    std::lock_guard<std::mutex> lk(preview_mutex_);
    return reinterpret_cast<void*>(imgui_preview_dset_);
}

const uint8_t* VulkanRenderer::get_cpu_frame(int& out_stride_bytes) {
    out_stride_bytes = cpu_frame_stride_;
    return cpu_frame_data_.data();
}

void VulkanRenderer::flush() {
    vkDeviceWaitIdle(device_);
}

void VulkanRenderer::resize(int w, int h) {
    if (!initialised_) return;
    flush();
    cfg_.width  = w;
    cfg_.height = h;

    // Destroy size-dependent resources
    vkDestroyImageView(device_, hdr_image_.view,   nullptr); vmaDestroyImage(allocator_, hdr_image_.image, hdr_image_.alloc);
    vkDestroyImageView(device_, sdr_image_.view,   nullptr); vmaDestroyImage(allocator_, sdr_image_.image, sdr_image_.alloc);
    vkDestroyImageView(device_, bloom_tmp_.view,   nullptr); vmaDestroyImage(allocator_, bloom_tmp_.image, bloom_tmp_.alloc);
    vmaDestroyBuffer(allocator_, readback_buf_.buffer, readback_buf_.alloc);
    hdr_image_ = {}; sdr_image_ = {}; bloom_tmp_ = {}; readback_buf_ = {};

    create_hdr_image();
    create_sdr_image();
    create_bloom_tmp_image();
    create_readback_buffer();

    // Recreate swap chain if needed
    if (swap_chain_) {
        for (auto v : sc_image_views_) vkDestroyImageView(device_, v, nullptr);
        sc_image_views_.clear();
        sc_images_.clear();
        vkDestroySwapchainKHR(device_, swap_chain_, nullptr);
        swap_chain_ = VK_NULL_HANDLE;
        create_swap_chain();
    }
}

void VulkanRenderer::set_config(const RenderConfig& cfg) {
    bool needs_resize = (cfg.width != cfg_.width || cfg.height != cfg_.height);
    cfg_ = cfg;
    if (needs_resize) resize(cfg.width, cfg.height);
}

void VulkanRenderer::shutdown() {
    if (!initialised_) return;
    flush();

    vkDestroyPipeline(device_, beam_pipeline_,         nullptr);
    vkDestroyPipeline(device_, post_pipeline_,         nullptr);
    vkDestroyPipelineLayout(device_, beam_pipeline_layout_, nullptr);
    vkDestroyPipelineLayout(device_, post_pipeline_layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, beam_dset_layout_, nullptr);
    vkDestroyDescriptorSetLayout(device_, post_dset_layout_, nullptr);
    vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    vkDestroySampler(device_, linear_sampler_, nullptr);

    vmaDestroyBuffer(allocator_, point_buf_.buffer,         point_buf_.alloc);
    vmaDestroyBuffer(allocator_, point_buf_staging_.buffer, point_buf_staging_.alloc);
    vmaDestroyBuffer(allocator_, readback_buf_.buffer,      readback_buf_.alloc);

    vkDestroyImageView(device_, hdr_image_.view, nullptr); vmaDestroyImage(allocator_, hdr_image_.image, hdr_image_.alloc);
    vkDestroyImageView(device_, sdr_image_.view, nullptr); vmaDestroyImage(allocator_, sdr_image_.image, sdr_image_.alloc);
    vkDestroyImageView(device_, bloom_tmp_.view, nullptr); vmaDestroyImage(allocator_, bloom_tmp_.image, bloom_tmp_.alloc);

    for (int i = 0; i < kMaxFrames; ++i) {
        vkDestroyFence(device_,     render_fence_[i],    nullptr);
        vkDestroySemaphore(device_, image_available_[i], nullptr);
        vkDestroySemaphore(device_, render_done_[i],     nullptr);
    }
    vkDestroyCommandPool(device_, cmd_pool_, nullptr);

    for (auto v : sc_image_views_)  vkDestroyImageView(device_, v, nullptr);
    for (auto fb : sc_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    if (render_pass_)  vkDestroyRenderPass(device_, render_pass_, nullptr);
    if (swap_chain_)   vkDestroySwapchainKHR(device_, swap_chain_, nullptr);

    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);

    initialised_ = false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void VulkanRenderer::transition_image(VkCommandBuffer cmd, VkImage image,
                                       VkImageLayout old_layout,
                                       VkImageLayout new_layout,
                                       VkPipelineStageFlags src_stage,
                                       VkPipelineStageFlags dst_stage,
                                       VkAccessFlags src_access,
                                       VkAccessFlags dst_access) {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = old_layout;
    b.newLayout           = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

VkCommandBuffer VulkanRenderer::begin_one_shot() {
    VkCommandBufferAllocateInfo alloc{};
    alloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool        = cmd_pool_;
    alloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void VulkanRenderer::end_one_shot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &cmd);
}

} // namespace idhmfis

#endif // ifndef _WIN32
