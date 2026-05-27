#pragma once
// d3d12_renderer.h — Windows D3D12 implementation of IRenderer.
// Uses DXC-compiled HLSL compute shaders for beam rasterisation and post-process.

#ifdef _WIN32

#include "renderer.h"

// Windows / D3D12 headers — order matters
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace idhmfis {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// GPU-side representation of a laser point (matches beam_raster.hlsl struct)
// ---------------------------------------------------------------------------
struct GpuLaserPoint {
    float x, y;          // normalised device coordinates -1..1
    float r, g, b;       // linear RGB 0..1
    int   blanked;        // 1 = beam off
    float pad;
};

// ---------------------------------------------------------------------------
// Constant buffer matching PassCB in beam_raster.hlsl
// ---------------------------------------------------------------------------
struct BeamPassCB {
    uint32_t point_count;
    float    beam_thickness;   // pixels
    uint32_t width;
    uint32_t height;
    float    pad[4];
};

// ---------------------------------------------------------------------------
// Constant buffer matching PostCB in post_process.hlsl
// ---------------------------------------------------------------------------
struct PostPassCB {
    uint32_t width;
    uint32_t height;
    float    bloom_radius;
    float    haze_density;
    float    exposure;
    uint32_t film_grain;       // 0 or 1
    float    grain_amount;
    uint32_t frame_counter;
    float    pad[8];
};

// ---------------------------------------------------------------------------
// D3D12Renderer
// ---------------------------------------------------------------------------
class D3D12Renderer : public IRenderer {
public:
    D3D12Renderer();
    ~D3D12Renderer() override;

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
    // ---- Device / adapter ----
    bool create_device();
    bool create_command_infrastructure();
    bool create_swap_chain(HWND hwnd);
    bool create_descriptor_heaps();
    bool create_hdr_render_target();
    bool create_sdr_readback_buffer();
    bool compile_and_create_pipelines();
    bool create_gpu_point_buffer(uint32_t max_points);

    void wait_for_fence(uint64_t value);
    void signal_and_wait();

    bool compile_shader(const wchar_t* path, const char* entry,
                        const char* target, ID3DBlob** blob_out);
    bool compile_shader_from_string(const char* src, size_t len,
                                    const char* entry, const char* target,
                                    ID3DBlob** blob_out);

    // Heap index helpers
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(int idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle(int idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE cbv_srv_uav_cpu(int idx) const;
    D3D12_GPU_DESCRIPTOR_HANDLE cbv_srv_uav_gpu(int idx) const;

    void upload_constant_buffer(ID3D12Resource* cb, const void* data, size_t size);
    void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    // ---- Config ----
    RenderConfig cfg_;
    HWND         hwnd_ = nullptr;

    // ---- Core D3D12 objects ----
    ComPtr<ID3D12Device5>              device_;
    ComPtr<IDXGIFactory6>              dxgi_factory_;
    ComPtr<IDXGISwapChain4>            swap_chain_;
    ComPtr<ID3D12CommandQueue>         cmd_queue_;
    ComPtr<ID3D12CommandAllocator>     cmd_alloc_[2]; // double-buffered
    ComPtr<ID3D12GraphicsCommandList4> cmd_list_;

    // ---- Fence ----
    ComPtr<ID3D12Fence>                fence_;
    HANDLE                             fence_event_ = nullptr;
    uint64_t                           fence_value_ = 0;
    uint64_t                           frame_index_ = 0;

    // ---- Descriptor heaps ----
    ComPtr<ID3D12DescriptorHeap>       rtv_heap_;
    ComPtr<ID3D12DescriptorHeap>       dsv_heap_;
    ComPtr<ID3D12DescriptorHeap>       srv_heap_;  // CBV/SRV/UAV (shader-visible)
    // M-16: ClearUnorderedAccessViewFloat requires a NON-shader-visible CPU
    // descriptor handle as its second argument.  Allocate a tiny separate heap
    // with FLAG_NONE and copy the UAV descriptors used for clears into it.
    ComPtr<ID3D12DescriptorHeap>       uav_clear_heap_;  // non-shader-visible, 1 descriptor
    D3D12_CPU_DESCRIPTOR_HANDLE        hdr_uav_clear_cpu_{};  // CPU handle in uav_clear_heap_

    UINT rtv_descriptor_size_     = 0;
    UINT srv_descriptor_size_     = 0;
    // SRV heap slot assignments:
    //   0  = HDR RT UAV  (beam raster output)
    //   1  = HDR RT SRV  (post-process input)
    //   2  = SDR RT UAV  (post-process output)
    //   3  = SDR RT SRV  (ImGui preview)
    //   4  = GPU point buffer SRV
    //   5  = Bloom temp UAV
    //   6  = Bloom temp SRV
    static constexpr int kSrvSlotHdrUav    = 0;
    static constexpr int kSrvSlotHdrSrv    = 1;
    static constexpr int kSrvSlotSdrUav    = 2;
    static constexpr int kSrvSlotSdrSrv    = 3;  // ImGui preview
    static constexpr int kSrvSlotPointBuf  = 4;
    static constexpr int kSrvSlotBloomUav  = 5;
    static constexpr int kSrvSlotBloomSrv  = 6;
    static constexpr int kSrvHeapSize      = 32;

    // ---- Swap chain back buffers ----
    static constexpr int kBackBufferCount = 2;
    ComPtr<ID3D12Resource>             back_buffers_[kBackBufferCount];

    // ---- HDR accumulation render target (RGBA16F) ----
    ComPtr<ID3D12Resource>             hdr_rt_;
    D3D12_GPU_DESCRIPTOR_HANDLE        hdr_uav_gpu_{};

    // ---- SDR output render target (RGBA8) ----
    ComPtr<ID3D12Resource>             sdr_rt_;
    D3D12_GPU_DESCRIPTOR_HANDLE        sdr_uav_gpu_{};
    D3D12_GPU_DESCRIPTOR_HANDLE        sdr_srv_gpu_{};

    // ---- Bloom temporary buffer ----
    ComPtr<ID3D12Resource>             bloom_tmp_;

    // ---- CPU readback staging buffer ----
    ComPtr<ID3D12Resource>             readback_buf_;
    std::vector<uint8_t>               cpu_frame_data_;
    int                                cpu_frame_stride_ = 0;

    // ---- GPU point buffer (structured buffer) ----
    ComPtr<ID3D12Resource>             point_buf_gpu_;
    ComPtr<ID3D12Resource>             point_buf_upload_;
    uint32_t                           point_buf_capacity_ = 0;

    // ---- Constant buffers ----
    ComPtr<ID3D12Resource>             beam_cb_;
    ComPtr<ID3D12Resource>             post_cb_;

    // ---- Pipelines ----
    // Beam raster: compute pipeline
    ComPtr<ID3D12RootSignature>        beam_root_sig_;
    ComPtr<ID3D12PipelineState>        beam_pso_;

    // Post-process: compute pipeline
    ComPtr<ID3D12RootSignature>        post_root_sig_;
    ComPtr<ID3D12PipelineState>        post_pso_;

    // ---- Frame counter for grain seed ----
    uint32_t frame_counter_ = 0;

    // ---- Preview texture handle for ImGui ----
    // We return the GPU descriptor handle cast to void* (ImTextureID convention)
    D3D12_GPU_DESCRIPTOR_HANDLE        imgui_preview_handle_{};
    mutable std::mutex                 preview_mutex_;

    // ---- Initialised flag ----
    bool initialised_ = false;
};

} // namespace idhmfis

#endif // _WIN32
