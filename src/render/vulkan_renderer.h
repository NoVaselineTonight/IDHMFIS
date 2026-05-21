#pragma once
// vulkan_renderer.h — Vulkan implementation of IRenderer.
// Used on macOS (MoltenVK) and as a cross-platform fallback.
// Uses VMA for memory allocation.

#ifndef _WIN32

#include "renderer.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace idhmfis {

// ---------------------------------------------------------------------------
// Helper: allocated image with VMA
// ---------------------------------------------------------------------------
struct VkAllocatedImage {
    VkImage       image  = VK_NULL_HANDLE;
    VkImageView   view   = VK_NULL_HANDLE;
    VmaAllocation alloc  = VK_NULL_HANDLE;
    VkFormat      format = VK_FORMAT_UNDEFINED;
    uint32_t      width  = 0;
    uint32_t      height = 0;
};

// ---------------------------------------------------------------------------
// Helper: allocated buffer with VMA
// ---------------------------------------------------------------------------
struct VkAllocatedBuffer {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VmaAllocation alloc  = VK_NULL_HANDLE;
    void*         mapped = nullptr;   // non-null if persistently mapped
    VkDeviceSize  size   = 0;
};

// ---------------------------------------------------------------------------
// GPU-side laser point (matches beam_vert.glsl in/layout)
// ---------------------------------------------------------------------------
struct VkGpuLaserPoint {
    float x, y;       // NDC -1..1
    float r, g, b;    // linear RGB
    int   blanked;
    float pad;
};

// ---------------------------------------------------------------------------
// Push constants for beam pipeline
// ---------------------------------------------------------------------------
struct BeamPushConst {
    uint32_t point_count;
    float    beam_thickness;
    uint32_t width;
    uint32_t height;
};

// ---------------------------------------------------------------------------
// Push constants for post pipeline
// ---------------------------------------------------------------------------
struct PostPushConst {
    uint32_t width;
    uint32_t height;
    float    bloom_radius;
    float    haze_density;
    float    exposure;
    uint32_t film_grain;
    float    grain_amount;
    uint32_t frame_counter;
    uint32_t pass_mode;    // 0=bloom_h, 1=bloom_v, 2=compose
};

// ---------------------------------------------------------------------------
// VulkanRenderer
// ---------------------------------------------------------------------------
class VulkanRenderer : public IRenderer {
public:
    VulkanRenderer();
    ~VulkanRenderer() override;

    bool init(void* native_window_handle, const RenderConfig& cfg) override;
    void resize(int w, int h)                                        override;
    void set_config(const RenderConfig& cfg)                         override;
    void render_frame(const PointBuffer& pts)                        override;
    void present()                                                   override;
    void* get_preview_texture()                                      override;
    const uint8_t* get_cpu_frame(int& out_stride_bytes)              override;
    void flush()                                                     override;
    void shutdown()                                                  override;

private:
    bool create_instance();
    bool create_surface(void* native_handle);
    bool pick_physical_device();
    bool create_logical_device();
    bool create_vma_allocator();
    bool create_swap_chain();
    bool create_render_pass();
    bool create_framebuffers();
    bool create_command_pool_and_buffers();
    bool create_sync_objects();
    bool create_hdr_image();
    bool create_sdr_image();
    bool create_bloom_tmp_image();
    bool create_readback_buffer();
    bool create_point_buffer(uint32_t max_points);
    bool create_descriptor_sets();
    bool create_pipelines();

    VkShaderModule create_shader_from_glsl(const char* src, size_t len,
                                            VkShaderStageFlagBits stage);
    VkShaderModule create_shader_from_spirv(const uint32_t* words, size_t word_count);

    void transition_image(VkCommandBuffer cmd, VkImage image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                          VkAccessFlags src_access, VkAccessFlags dst_access);

    VkCommandBuffer begin_one_shot();
    void            end_one_shot(VkCommandBuffer cmd);

    void destroy_swap_chain_resources();
    void recreate_swap_chain();

    RenderConfig cfg_;

    // ---- Instance / device ----
    VkInstance               instance_       = VK_NULL_HANDLE;
    VkSurfaceKHR             surface_        = VK_NULL_HANDLE;
    VkPhysicalDevice         phys_device_    = VK_NULL_HANDLE;
    VkDevice                 device_         = VK_NULL_HANDLE;
    VkQueue                  graphics_queue_ = VK_NULL_HANDLE;
    VkQueue                  present_queue_  = VK_NULL_HANDLE;
    uint32_t                 graphics_family_= UINT32_MAX;
    uint32_t                 present_family_ = UINT32_MAX;

    VmaAllocator             allocator_      = VK_NULL_HANDLE;

    // ---- Swap chain ----
    VkSwapchainKHR           swap_chain_     = VK_NULL_HANDLE;
    VkFormat                 sc_format_      = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D               sc_extent_      = {};
    std::vector<VkImage>     sc_images_;
    std::vector<VkImageView> sc_image_views_;
    std::vector<VkFramebuffer> sc_framebuffers_;

    // ---- Render pass (for final blit to swap chain) ----
    VkRenderPass             render_pass_    = VK_NULL_HANDLE;

    // ---- Internal render targets ----
    VkAllocatedImage         hdr_image_;     // RGBA16F compute UAV
    VkAllocatedImage         sdr_image_;     // RGBA8 compute UAV / ImGui SRV
    VkAllocatedImage         bloom_tmp_;     // Bloom intermediate

    // ---- Readback ----
    VkAllocatedBuffer        readback_buf_;
    std::vector<uint8_t>     cpu_frame_data_;
    int                      cpu_frame_stride_ = 0;

    // ---- Point buffer ----
    VkAllocatedBuffer        point_buf_;
    VkAllocatedBuffer        point_buf_staging_;
    uint32_t                 point_buf_capacity_ = 0;

    // ---- Command infrastructure ----
    VkCommandPool            cmd_pool_       = VK_NULL_HANDLE;
    static constexpr int     kMaxFrames      = 2;
    VkCommandBuffer          cmd_bufs_[kMaxFrames]{};
    VkFence                  render_fence_[kMaxFrames]{};
    VkSemaphore              image_available_[kMaxFrames]{};
    VkSemaphore              render_done_[kMaxFrames]{};
    uint32_t                 current_frame_  = 0;
    uint32_t                 frame_counter_  = 0;

    // ---- Descriptors ----
    VkDescriptorPool         desc_pool_      = VK_NULL_HANDLE;
    VkDescriptorSetLayout    beam_dset_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet          beam_dset_        = VK_NULL_HANDLE;
    VkDescriptorSetLayout    post_dset_layout_ = VK_NULL_HANDLE;
    VkDescriptorSet          post_dset_        = VK_NULL_HANDLE;

    VkSampler                linear_sampler_   = VK_NULL_HANDLE;

    // ---- Pipelines ----
    VkPipelineLayout         beam_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline               beam_pipeline_        = VK_NULL_HANDLE;
    VkPipelineLayout         post_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline               post_pipeline_        = VK_NULL_HANDLE;

    // ---- Preview ----
    // ImGui preview: the VkDescriptorSet wrapping sdr_image_ as a combined
    // image sampler. Returned as void* (ImTextureID).
    VkDescriptorSet          imgui_preview_dset_ = VK_NULL_HANDLE;
    mutable std::mutex       preview_mutex_;

    bool                     initialised_ = false;
    void*                    native_handle_ = nullptr;
};

} // namespace idhmfis

#endif // ifndef _WIN32
