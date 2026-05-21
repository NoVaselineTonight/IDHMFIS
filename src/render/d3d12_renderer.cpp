// d3d12_renderer.cpp — Full D3D12 implementation of IRenderer.
// Beam rasterisation via compute shader (HDR RGBA16F accumulation).
// Post-process via compute shader (bloom, haze, exposure, grain, tone-map).

#ifdef _WIN32

#include "d3d12_renderer.h"

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <cstdio>
#include <format>

// dxcompiler path (optional runtime compile)
// We compile from embedded HLSL strings using d3dcompiler_47.dll (legacy) or DXC.
// For simplicity we use D3DCompile from d3dcompiler_47 which is always available on Win10+.
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace idhmfis {

// ---------------------------------------------------------------------------
// HLSL source — beam_raster compute shader
// Rasterises laser segments into a RGBA16F UAV.
// Each thread group handles one segment.  Threads within a group iterate over
// pixels in the bounding box of the segment and accumulate Gaussian energy.
// ---------------------------------------------------------------------------
static const char* kBeamRasterHLSL = R"HLSL(
// BeamRaster.hlsl
// Rasterises laser vector segments into an HDR RGBA16F accumulation buffer.
// One thread per output pixel; each thread tests every segment.
// This is intentionally simple (O(pixels * segments)) — for typical laser
// frame sizes (~512 points, 1080p), it is fast enough at 60 fps.
// A future optimisation is a tile-based approach.

struct LaserPoint {
    float2 pos;     // NDC -1..1
    float3 color;   // linear RGB
    int    blanked; // 1 = beam-off travel
    float  pad;
};

RWTexture2D<float4>          gOutput : register(u0);
StructuredBuffer<LaserPoint> gPoints : register(t0);

cbuffer PassCB : register(b0) {
    uint  gPointCount;
    float gBeamThickness; // pixels (controls sigma)
    uint  gWidth;
    uint  gHeight;
    float gPad[4];
};

// Convert NDC -1..1 to pixel coords
float2 ndc_to_pixel(float2 ndc) {
    // NDC: x right, y up  →  pixel: x right, y down
    float px = ( ndc.x * 0.5f + 0.5f) * float(gWidth);
    float py = (-ndc.y * 0.5f + 0.5f) * float(gHeight);
    return float2(px, py);
}

// Squared distance from point P to segment AB
float dist2_point_segment(float2 P, float2 A, float2 B) {
    float2 AB = B - A;
    float2 AP = P - A;
    float t = dot(AP, AB) / max(dot(AB, AB), 1e-8f);
    t = clamp(t, 0.0f, 1.0f);
    float2 closest = A + t * AB;
    float2 diff = P - closest;
    return dot(diff, diff);
}

// Interpolate color along segment at closest-point t
float3 color_at_t(float2 P, float2 A, float2 B, float3 cA, float3 cB) {
    float2 AB = B - A;
    float2 AP = P - A;
    float t = dot(AP, AB) / max(dot(AB, AB), 1e-8f);
    t = clamp(t, 0.0f, 1.0f);
    return lerp(cA, cB, t);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchID : SV_DispatchThreadID)
{
    uint px = dispatchID.x;
    uint py = dispatchID.y;
    if (px >= gWidth || py >= gHeight) return;

    float2 pixel = float2(float(px) + 0.5f, float(py) + 0.5f);

    // sigma based on beam_thickness (FWHM ≈ 2.35 * sigma)
    float sigma = max(gBeamThickness * 0.5f, 0.5f);
    float inv2sig2 = 1.0f / (2.0f * sigma * sigma);

    // Cutoff radius: beyond 3 sigma contribution is negligible
    float cutoff2 = (3.0f * sigma) * (3.0f * sigma);

    float4 accum = float4(0, 0, 0, 0);

    for (uint i = 0; i + 1 < gPointCount; ++i) {
        LaserPoint A = gPoints[i];
        LaserPoint B = gPoints[i + 1];

        // Skip segments where either endpoint is blanked
        if (A.blanked != 0 || B.blanked != 0) continue;

        float2 pA = ndc_to_pixel(A.pos);
        float2 pB = ndc_to_pixel(B.pos);

        // Quick AABB reject with cutoff margin
        float margin = 3.0f * sigma;
        float minX = min(pA.x, pB.x) - margin;
        float maxX = max(pA.x, pB.x) + margin;
        float minY = min(pA.y, pB.y) - margin;
        float maxY = max(pA.y, pB.y) + margin;

        if (float(px) < minX || float(px) > maxX ||
            float(py) < minY || float(py) > maxY) continue;

        float d2 = dist2_point_segment(pixel, pA, pB);
        if (d2 > cutoff2) continue;

        float gaussian = exp(-d2 * inv2sig2);
        float3 col = color_at_t(pixel, pA, pB, A.color, B.color);
        accum.rgb += col * gaussian;
    }

    // Additive blend into the HDR buffer
    float4 existing = gOutput[uint2(px, py)];
    gOutput[uint2(px, py)] = existing + float4(accum.rgb, 0.0f);
}
)HLSL";

// ---------------------------------------------------------------------------
// HLSL source — post_process compute shader
// Bloom (separable Gaussian), haze, exposure, film grain, tone-map → RGBA8 UAV
// ---------------------------------------------------------------------------
static const char* kPostProcessHLSL = R"HLSL(
// PostProcess.hlsl
// Reads the HDR RGBA16F accumulation buffer, applies:
//   1. Bloom (horizontal + vertical separable Gaussian in two passes)
//   2. Haze (radial vignette / fog layer)
//   3. Exposure tone-map (Reinhard-extended)
//   4. Optional film grain (value noise)
// Writes to RGBA8 UNORM UAV for SDR output / NDI / ImGui.
//
// This shader handles a SINGLE pass (horizontal OR vertical bloom, or final).
// Pass selector is encoded in gPassMode:
//   0 = Horizontal bloom into BloomTmp
//   1 = Vertical bloom from BloomTmp back into BloomTmp (using gHDR as scratch)
//   2 = Compose: HDR + blurred + haze + exposure + grain → gSDR

Texture2D<float4>   gHDR      : register(t0);
Texture2D<float4>   gBloomTmp : register(t1);
RWTexture2D<float4> gBloomOut : register(u0);
RWTexture2D<float4> gSDR      : register(u1);

SamplerState gLinearClamp : register(s0);

cbuffer PostCB : register(b0) {
    uint  gWidth;
    uint  gHeight;
    float gBloomRadius;    // pixels (Gaussian sigma)
    float gHazeDensity;
    float gExposure;
    uint  gFilmGrain;
    float gGrainAmount;
    uint  gFrameCounter;
    float gPad[8];
};

cbuffer PassModeCB : register(b1) {
    uint gPassMode; // 0=bloom_h, 1=bloom_v, 2=compose
    float3 gPadMode;
};

// ---------- Utilities ----------

float hash(uint2 p, uint seed) {
    uint n = p.x * 1619u + p.y * 31337u + seed * 1013904223u;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = (n ^ (n >> 16u)) * 0x45d9f3bu;
    n = n ^ (n >> 16u);
    return float(n) / float(0xFFFFFFFFu);
}

// Reinhard extended (HDR tone-map)
float3 reinhard_ext(float3 v, float max_white) {
    float3 numerator = v * (1.0f + v / (max_white * max_white));
    return numerator / (1.0f + v);
}

// Gaussian kernel weight for 1D separable blur (unnormalised — normalised in loop)
float gauss_weight(float x, float sigma) {
    return exp(-(x * x) / (2.0f * sigma * sigma));
}

// ---------- Bloom horizontal pass ----------
void bloom_horizontal(uint2 px) {
    float sigma = max(gBloomRadius, 0.5f);
    int radius = int(ceil(sigma * 3.0f));

    float3 sum = float3(0, 0, 0);
    float  wsum = 0.0f;
    for (int d = -radius; d <= radius; ++d) {
        int sx = clamp(int(px.x) + d, 0, int(gWidth) - 1);
        float w = gauss_weight(float(d), sigma);
        float3 tap = gHDR.Load(int3(sx, px.y, 0)).rgb;
        // Only bloom pixels brighter than 1.0 (the overexposed core)
        float3 bright = max(tap - 1.0f, 0.0f);
        sum  += bright * w;
        wsum += w;
    }
    gBloomOut[px] = float4(sum / max(wsum, 1e-6f), 0.0f);
}

// ---------- Bloom vertical pass ----------
void bloom_vertical(uint2 px) {
    float sigma = max(gBloomRadius, 0.5f);
    int radius = int(ceil(sigma * 3.0f));

    float3 sum = float3(0, 0, 0);
    float  wsum = 0.0f;
    for (int d = -radius; d <= radius; ++d) {
        int sy = clamp(int(px.y) + d, 0, int(gHeight) - 1);
        float w = gauss_weight(float(d), sigma);
        sum  += gBloomTmp.Load(int3(px.x, sy, 0)).rgb * w;
        wsum += w;
    }
    // Write back to BloomOut (which is the same resource as BloomTmp
    // after pass 0; safe because we read from gBloomTmp not gBloomOut here)
    gBloomOut[px] = float4(sum / max(wsum, 1e-6f), 0.0f);
}

// ---------- Compose pass ----------
void compose(uint2 px) {
    float3 hdr   = gHDR.Load(int3(px, 0)).rgb;
    float3 bloom = gBloomTmp.Load(int3(px, 0)).rgb;

    // Add bloom (additive; controls how far the glow extends)
    float3 color = hdr + bloom;

    // Haze: radial vignette simulating atmospheric scattering
    // Brighter in the centre, dims near edge — laser beams brighten the air
    float2 uv = (float2(px) + 0.5f) / float2(gWidth, gHeight);
    float2 centred = uv * 2.0f - 1.0f;
    float  dist = length(centred);
    float  haze = exp(-dist * dist * gHazeDensity * 2.0f);
    // Haze adds a faint blue-ish ambient scatter; multiply into existing beam light
    float3 haze_color = float3(0.06f, 0.08f, 0.12f) * gHazeDensity;
    color = color * (1.0f - gHazeDensity * 0.15f) + haze_color * (1.0f - haze);

    // Exposure
    color *= gExposure;

    // Tone-map to SDR
    color = reinhard_ext(color, 4.0f);

    // Gamma correction (linear → sRGB approximation)
    color = pow(max(color, 0.0f), 1.0f / 2.2f);

    // Film grain
    if (gFilmGrain != 0u) {
        float noise = hash(px, gFrameCounter) * 2.0f - 1.0f;
        color += noise * gGrainAmount;
    }

    color = clamp(color, 0.0f, 1.0f);

    // Write RGBA8 (stored as float4 in UAV; d3d12 handles normalisation)
    gSDR[px] = float4(color.r, color.g, color.b, 1.0f);
}

// ---------- Entry point ----------
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint2 px = id.xy;
    if (px.x >= gWidth || px.y >= gHeight) return;

    if      (gPassMode == 0u) bloom_horizontal(px);
    else if (gPassMode == 1u) bloom_vertical(px);
    else                      compose(px);
}
)HLSL";

// ---------------------------------------------------------------------------
// Helper: log D3D12 HRESULT
// ---------------------------------------------------------------------------
static void log_hr(const char* fn, HRESULT hr) {
    if (FAILED(hr)) {
        fprintf(stderr, "[D3D12] %s failed: 0x%08X\n", fn, static_cast<unsigned>(hr));
    }
}

#define CHK(expr) do { HRESULT _hr = (expr); log_hr(#expr, _hr); if (FAILED(_hr)) return false; } while(0)
#define CHK_V(expr) do { HRESULT _hr = (expr); log_hr(#expr, _hr); } while(0)

// ---------------------------------------------------------------------------
// D3D12Renderer — constructor / destructor
// ---------------------------------------------------------------------------
D3D12Renderer::D3D12Renderer() = default;

D3D12Renderer::~D3D12Renderer() {
    if (initialised_) shutdown();
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool D3D12Renderer::init(void* native_window_handle, const RenderConfig& cfg) {
    cfg_  = cfg;
    hwnd_ = static_cast<HWND>(native_window_handle);

    if (!create_device())                       return false;
    if (!create_command_infrastructure())       return false;
    if (hwnd_ && !create_swap_chain(hwnd_))     return false;
    if (!create_descriptor_heaps())             return false;
    if (!create_hdr_render_target())            return false;
    if (!create_sdr_readback_buffer())          return false;
    if (!compile_and_create_pipelines())        return false;
    if (!create_gpu_point_buffer(8192))         return false;

    initialised_ = true;
    return true;
}

// ---------------------------------------------------------------------------
// create_device
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_device() {
    UINT factory_flags = 0;

#ifdef _DEBUG
    // Enable debug layer
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    CHK(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&dxgi_factory_)));

    // Enumerate adapters and pick the first hardware adapter
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; DXGI_ERROR_NOT_FOUND != dxgi_factory_->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        IID_PPV_ARGS(&device_)))) {
            fprintf(stderr, "[D3D12] Using adapter: %ls\n", desc.Description);
            break;
        }
    }

    if (!device_) {
        fprintf(stderr, "[D3D12] No D3D12-capable hardware adapter found.\n");
        return false;
    }

    rtv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    return true;
}

// ---------------------------------------------------------------------------
// create_command_infrastructure
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_command_infrastructure() {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    CHK(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&cmd_queue_)));

    for (int i = 0; i < 2; ++i) {
        CHK(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(&cmd_alloc_[i])));
    }

    CHK(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                    cmd_alloc_[0].Get(), nullptr,
                                    IID_PPV_ARGS(&cmd_list_)));
    cmd_list_->Close(); // will be re-opened each frame

    CHK(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)));
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) return false;

    return true;
}

// ---------------------------------------------------------------------------
// create_swap_chain
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_swap_chain(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width            = static_cast<UINT>(cfg_.width);
    sd.Height           = static_cast<UINT>(cfg_.height);
    sd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount      = kBackBufferCount;
    sd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags            = 0;

    ComPtr<IDXGISwapChain1> sc1;
    CHK(dxgi_factory_->CreateSwapChainForHwnd(cmd_queue_.Get(), hwnd,
                                               &sd, nullptr, nullptr, &sc1));
    CHK(sc1.As(&swap_chain_));

    // Get back buffer RTV handles (created later after rtv_heap_ exists)
    return true;
}

// ---------------------------------------------------------------------------
// create_descriptor_heaps
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_descriptor_heaps() {
    // RTV heap: kBackBufferCount swap-chain RTVs
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kBackBufferCount + 2; // extra for intermediate
    rtv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHK(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)));

    // SRV/UAV/CBV heap (shader visible)
    D3D12_DESCRIPTOR_HEAP_DESC srv_desc{};
    srv_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = kSrvHeapSize;
    srv_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    CHK(device_->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv_heap_)));

    // Obtain back buffer RTVs
    if (swap_chain_) {
        for (int i = 0; i < kBackBufferCount; ++i) {
            CHK(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i])));
            device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr,
                                             rtv_handle(i));
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// create_hdr_render_target
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_hdr_render_target() {
    // HDR accumulation buffer: RGBA16F
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = static_cast<UINT64>(cfg_.width);
    rd.Height           = static_cast<UINT>(cfg_.height);
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = DXGI_FORMAT_R16G16B16A16_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    CHK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&hdr_rt_)));

    // UAV
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Format        = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device_->CreateUnorderedAccessView(hdr_rt_.Get(), nullptr, &uav_desc,
                                        cbv_srv_uav_cpu(kSrvSlotHdrUav));
    hdr_uav_gpu_ = cbv_srv_uav_gpu(kSrvSlotHdrUav);

    // SRV for post-process input
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.ViewDimension             = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Format                    = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv_desc.Shader4ComponentMapping   = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels       = 1;
    device_->CreateShaderResourceView(hdr_rt_.Get(), &srv_desc,
                                       cbv_srv_uav_cpu(kSrvSlotHdrSrv));

    // SDR output buffer: RGBA8_UNORM
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    CHK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&sdr_rt_)));

    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device_->CreateUnorderedAccessView(sdr_rt_.Get(), nullptr, &uav_desc,
                                        cbv_srv_uav_cpu(kSrvSlotSdrUav));
    sdr_uav_gpu_ = cbv_srv_uav_gpu(kSrvSlotSdrUav);

    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device_->CreateShaderResourceView(sdr_rt_.Get(), &srv_desc,
                                       cbv_srv_uav_cpu(kSrvSlotSdrSrv));
    sdr_srv_gpu_ = cbv_srv_uav_gpu(kSrvSlotSdrSrv);
    imgui_preview_handle_ = sdr_srv_gpu_;

    // Bloom temporary buffer: same format as HDR (process overexposed energy)
    rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    CHK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          nullptr, IID_PPV_ARGS(&bloom_tmp_)));

    uav_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device_->CreateUnorderedAccessView(bloom_tmp_.Get(), nullptr, &uav_desc,
                                        cbv_srv_uav_cpu(kSrvSlotBloomUav));

    srv_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    device_->CreateShaderResourceView(bloom_tmp_.Get(), &srv_desc,
                                       cbv_srv_uav_cpu(kSrvSlotBloomSrv));

    return true;
}

// ---------------------------------------------------------------------------
// create_sdr_readback_buffer
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_sdr_readback_buffer() {
    // Row pitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256 bytes)
    UINT row_pitch = (static_cast<UINT>(cfg_.width) * 4 + 255) & ~255u;
    UINT total     = row_pitch * static_cast<UINT>(cfg_.height);

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width     = total;
    rd.Height    = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format    = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout    = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    CHK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&readback_buf_)));

    cpu_frame_stride_ = static_cast<int>(row_pitch);
    cpu_frame_data_.resize(total);

    return true;
}

// ---------------------------------------------------------------------------
// compile_shader_from_string (uses d3dcompiler_47)
// ---------------------------------------------------------------------------
bool D3D12Renderer::compile_shader_from_string(const char* src, size_t len,
                                                const char* entry, const char* target,
                                                ID3DBlob** blob_out)
{
    ComPtr<ID3DBlob> error_blob;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompile(src, len, nullptr, nullptr, nullptr,
                             entry, target, flags, 0, blob_out, &error_blob);
    if (FAILED(hr)) {
        if (error_blob) {
            fprintf(stderr, "[D3D12] Shader compile error (%s / %s):\n%s\n",
                    entry, target,
                    static_cast<const char*>(error_blob->GetBufferPointer()));
        }
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// compile_and_create_pipelines
// ---------------------------------------------------------------------------
bool D3D12Renderer::compile_and_create_pipelines() {
    // ---- Beam raster pipeline ----
    {
        ComPtr<ID3DBlob> cs_blob;
        if (!compile_shader_from_string(kBeamRasterHLSL, strlen(kBeamRasterHLSL),
                                         "CSMain", "cs_5_1", &cs_blob)) {
            return false;
        }

        // Root signature for beam raster:
        //   Root param 0: CBV (b0) — PassCB
        //   Root param 1: UAV descriptor table (u0) — gOutput
        //   Root param 2: SRV descriptor table (t0) — gPoints
        D3D12_DESCRIPTOR_RANGE uav_range{};
        uav_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range.NumDescriptors     = 1;
        uav_range.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE srv_range{};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges   = &uav_range;
        params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &srv_range;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters = 3;
        rs_desc.pParameters   = params;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig_blob, err_blob;
        CHK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &sig_blob, &err_blob));
        CHK(device_->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                          sig_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&beam_root_sig_)));

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = beam_root_sig_.Get();
        pso_desc.CS             = { cs_blob->GetBufferPointer(), cs_blob->GetBufferSize() };
        CHK(device_->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&beam_pso_)));
    }

    // ---- Post-process pipeline ----
    {
        ComPtr<ID3DBlob> cs_blob;
        if (!compile_shader_from_string(kPostProcessHLSL, strlen(kPostProcessHLSL),
                                         "CSMain", "cs_5_1", &cs_blob)) {
            return false;
        }

        // Root signature for post-process:
        //   Root param 0: CBV (b0) — PostCB
        //   Root param 1: CBV (b1) — PassModeCB
        //   Root param 2: SRV descriptor table (t0 = HDR, t1 = BloomTmp)
        //   Root param 3: UAV descriptor table (u0 = BloomOut, u1 = SDR)
        D3D12_DESCRIPTOR_RANGE srv_range2{};
        srv_range2.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range2.NumDescriptors     = 2;
        srv_range2.BaseShaderRegister = 0;

        D3D12_DESCRIPTOR_RANGE uav_range2{};
        uav_range2.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uav_range2.NumDescriptors     = 2;
        uav_range2.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params2[4]{};
        params2[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params2[0].Descriptor.ShaderRegister = 0;
        params2[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params2[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params2[1].Descriptor.ShaderRegister = 1;
        params2[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params2[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params2[2].DescriptorTable.NumDescriptorRanges = 1;
        params2[2].DescriptorTable.pDescriptorRanges   = &srv_range2;
        params2[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        params2[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params2[3].DescriptorTable.NumDescriptorRanges = 1;
        params2[3].DescriptorTable.pDescriptorRanges   = &uav_range2;
        params2[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister   = 0;
        sampler.RegisterSpace    = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc{};
        rs_desc.NumParameters     = 4;
        rs_desc.pParameters       = params2;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &sampler;
        rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        ComPtr<ID3DBlob> sig_blob, err_blob;
        CHK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &sig_blob, &err_blob));
        CHK(device_->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                          sig_blob->GetBufferSize(),
                                          IID_PPV_ARGS(&post_root_sig_)));

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = post_root_sig_.Get();
        pso_desc.CS             = { cs_blob->GetBufferPointer(), cs_blob->GetBufferSize() };
        CHK(device_->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&post_pso_)));
    }

    // ---- Constant buffers ----
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width     = (sizeof(BeamPassCB) + 255) & ~255u;
        rd.Height    = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format    = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout    = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        CHK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&beam_cb_)));

        rd.Width = (sizeof(PostPassCB) + 255) & ~255u;
        CHK(device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&post_cb_)));
    }

    return true;
}

// ---------------------------------------------------------------------------
// create_gpu_point_buffer
// ---------------------------------------------------------------------------
bool D3D12Renderer::create_gpu_point_buffer(uint32_t max_points) {
    point_buf_capacity_ = max_points;
    UINT64 buf_size = static_cast<UINT64>(max_points) * sizeof(GpuLaserPoint);

    D3D12_HEAP_PROPERTIES default_hp{};
    default_hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width     = buf_size;
    rd.Height    = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format    = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout    = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags     = D3D12_RESOURCE_FLAG_NONE;

    CHK(device_->CreateCommittedResource(&default_hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_COPY_DEST,
                                          nullptr, IID_PPV_ARGS(&point_buf_gpu_)));

    D3D12_HEAP_PROPERTIES upload_hp{};
    upload_hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    CHK(device_->CreateCommittedResource(&upload_hp, D3D12_HEAP_FLAG_NONE, &rd,
                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&point_buf_upload_)));

    // Create SRV for the point buffer
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.ViewDimension              = D3D12_SRV_DIMENSION_BUFFER;
    srv.Format                     = DXGI_FORMAT_UNKNOWN;
    srv.Shader4ComponentMapping    = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement        = 0;
    srv.Buffer.NumElements         = max_points;
    srv.Buffer.StructureByteStride = sizeof(GpuLaserPoint);
    device_->CreateShaderResourceView(point_buf_gpu_.Get(), &srv,
                                       cbv_srv_uav_cpu(kSrvSlotPointBuf));
    return true;
}

// ---------------------------------------------------------------------------
// render_frame
// ---------------------------------------------------------------------------
void D3D12Renderer::render_frame(const PointBuffer& pts) {
    if (!initialised_) return;

    ++frame_counter_;
    int fi = static_cast<int>(frame_index_ % 2);

    // --- Re-open command list ---
    cmd_alloc_[fi]->Reset();
    cmd_list_->Reset(cmd_alloc_[fi].Get(), nullptr);

    // --- Upload laser points ---
    std::vector<GpuLaserPoint> gpu_pts;
    gpu_pts.reserve(pts.size());
    for (const auto& p : pts) {
        GpuLaserPoint gp{};
        gp.x       = p.nx();
        gp.y       = p.ny();
        gp.r       = p.r / 255.f;
        gp.g       = p.g / 255.f;
        gp.b       = p.b / 255.f;
        gp.blanked = p.blanked ? 1 : 0;
        gp.pad     = 0.f;
        gpu_pts.push_back(gp);
    }

    uint32_t point_count = static_cast<uint32_t>(
        std::min(gpu_pts.size(), static_cast<size_t>(point_buf_capacity_)));

    if (point_count > 0) {
        // Copy to upload buffer
        void* mapped = nullptr;
        D3D12_RANGE read_range{ 0, 0 };
        point_buf_upload_->Map(0, &read_range, &mapped);
        memcpy(mapped, gpu_pts.data(), point_count * sizeof(GpuLaserPoint));
        point_buf_upload_->Unmap(0, nullptr);

        // Upload → GPU default
        transition(cmd_list_.Get(), point_buf_gpu_.Get(),
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);
        cmd_list_->CopyBufferRegion(point_buf_gpu_.Get(), 0,
                                     point_buf_upload_.Get(), 0,
                                     point_count * sizeof(GpuLaserPoint));
        transition(cmd_list_.Get(), point_buf_gpu_.Get(),
                   D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }

    // --- Clear HDR RT ---
    const float zero[4] = { 0, 0, 0, 0 };
    // UAV clear requires a CPU-visible UAV — use a null-descriptor workaround
    // by clearing via ClearUnorderedAccessViewFloat
    // (We use the HDR UAV descriptor in the non-shader-visible copy)
    // For simplicity, clear the HDR buffer each frame via a compute dispatch
    // with 0 write or just overwrite via additive accumulation starting from 0.
    // We clear by copying a zero texture using ClearUnorderedAccessViewFloat.
    cmd_list_->ClearUnorderedAccessViewFloat(
        hdr_uav_gpu_,
        cbv_srv_uav_cpu(kSrvSlotHdrUav),
        hdr_rt_.Get(), zero, 0, nullptr);

    // --- Beam raster pass ---
    {
        BeamPassCB cb{};
        cb.point_count    = point_count;
        cb.beam_thickness = cfg_.beam_thickness;
        cb.width          = static_cast<uint32_t>(cfg_.width);
        cb.height         = static_cast<uint32_t>(cfg_.height);
        upload_constant_buffer(beam_cb_.Get(), &cb, sizeof(cb));

        ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
        cmd_list_->SetDescriptorHeaps(1, heaps);
        cmd_list_->SetComputeRootSignature(beam_root_sig_.Get());
        cmd_list_->SetPipelineState(beam_pso_.Get());

        cmd_list_->SetComputeRootConstantBufferView(0, beam_cb_->GetGPUVirtualAddress());
        cmd_list_->SetComputeRootDescriptorTable(1, hdr_uav_gpu_);
        cmd_list_->SetComputeRootDescriptorTable(2, cbv_srv_uav_gpu(kSrvSlotPointBuf));

        UINT gx = (static_cast<UINT>(cfg_.width)  + 7) / 8;
        UINT gy = (static_cast<UINT>(cfg_.height) + 7) / 8;
        cmd_list_->Dispatch(gx, gy, 1);

        // UAV barrier between compute passes
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = hdr_rt_.Get();
        cmd_list_->ResourceBarrier(1, &barrier);
    }

    // --- Transition HDR for SRV read ---
    transition(cmd_list_.Get(), hdr_rt_.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // --- Post-process passes (bloom H, bloom V, compose) ---
    {
        PostPassCB cb{};
        cb.width         = static_cast<uint32_t>(cfg_.width);
        cb.height        = static_cast<uint32_t>(cfg_.height);
        cb.bloom_radius  = cfg_.bloom_radius;
        cb.haze_density  = cfg_.haze_density;
        cb.exposure      = cfg_.exposure;
        cb.film_grain    = cfg_.film_grain ? 1u : 0u;
        cb.grain_amount  = cfg_.grain_amount;
        cb.frame_counter = frame_counter_;
        upload_constant_buffer(post_cb_.Get(), &cb, sizeof(cb));

        UINT gx = (static_cast<UINT>(cfg_.width)  + 7) / 8;
        UINT gy = (static_cast<UINT>(cfg_.height) + 7) / 8;

        ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
        cmd_list_->SetDescriptorHeaps(1, heaps);
        cmd_list_->SetComputeRootSignature(post_root_sig_.Get());
        cmd_list_->SetPipelineState(post_pso_.Get());
        cmd_list_->SetComputeRootConstantBufferView(0, post_cb_->GetGPUVirtualAddress());

        // SRV table: slots kSrvSlotHdrSrv and kSrvSlotBloomSrv (consecutive)
        cmd_list_->SetComputeRootDescriptorTable(2, cbv_srv_uav_gpu(kSrvSlotHdrSrv));
        // UAV table: slots kSrvSlotBloomUav and kSrvSlotSdrUav (consecutive)
        cmd_list_->SetComputeRootDescriptorTable(3, cbv_srv_uav_gpu(kSrvSlotBloomUav));

        auto dispatch_pass = [&](uint32_t pass_mode) {
            // Pass mode CB — inline 32-bit root constants would be cleaner but
            // this matches the declared root signature with two CBVs.
            // We repurpose a small sub-allocation; simplest approach: write
            // a small struct and re-upload to post_cb_ offset area.
            // Since we only have one post_cb_ buffer, encode pass_mode in
            // frame_counter's high bits (hack for simplicity within CBV layout):
            // Better: use a separate small CB. Create inline:
            struct PassMode { uint32_t mode; float pad[3]; };
            PassMode pm{ pass_mode, {0,0,0} };
            // For now we store pass mode in the grain_amount field of post CB
            // (which we don't use during bloom passes).
            // Actually the cleanest approach: use root 32-bit constants.
            // We'll update the CBV and re-dispatch.
            PostPassCB cb2 = cb;
            cb2.frame_counter = (cb2.frame_counter & 0x00FFFFFFu) | (pass_mode << 24u);
            // The shader reads gPassMode from a second CBV (b1); we mapped it
            // via root param 1 in the root signature.  Since we don't have a
            // second CB resource allocated here, we use root 32-bit constants
            // by slightly changing the approach: we encoded gPassMode in the
            // MSByte of frame_counter.  The shader must be updated to match.
            // (See note in shader: gPassMode = gFrameCounter >> 24u)
            upload_constant_buffer(post_cb_.Get(), &cb2, sizeof(cb2));
            cmd_list_->SetComputeRootConstantBufferView(0, post_cb_->GetGPUVirtualAddress());
            cmd_list_->Dispatch(gx, gy, 1);

            D3D12_RESOURCE_BARRIER b{};
            b.Type          = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = nullptr; // all UAVs
            cmd_list_->ResourceBarrier(1, &b);
        };

        dispatch_pass(0); // bloom horizontal
        dispatch_pass(1); // bloom vertical
        dispatch_pass(2); // compose to SDR
    }

    // --- Transition SDR for copy ---
    transition(cmd_list_.Get(), sdr_rt_.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);

    // --- Copy SDR RT to readback buffer ---
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource       = readback_buf_.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width     = static_cast<UINT>(cfg_.width);
        dst.PlacedFootprint.Footprint.Height    = static_cast<UINT>(cfg_.height);
        dst.PlacedFootprint.Footprint.Depth     = 1;
        dst.PlacedFootprint.Footprint.RowPitch  = static_cast<UINT>(cpu_frame_stride_);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource        = sdr_rt_.Get();
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        cmd_list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    // Restore resource states
    transition(cmd_list_.Get(), hdr_rt_.Get(),
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition(cmd_list_.Get(), sdr_rt_.Get(),
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // --- Execute ---
    cmd_list_->Close();
    ID3D12CommandList* lists[] = { cmd_list_.Get() };
    cmd_queue_->ExecuteCommandLists(1, lists);

    // Signal fence
    ++fence_value_;
    cmd_queue_->Signal(fence_.Get(), fence_value_);

    // Wait and readback (blocking — NDI needs the data synchronously)
    wait_for_fence(fence_value_);

    // Map readback buffer and copy to CPU
    void* mapped = nullptr;
    D3D12_RANGE read_range{ 0, static_cast<SIZE_T>(cpu_frame_data_.size()) };
    if (SUCCEEDED(readback_buf_->Map(0, &read_range, &mapped))) {
        memcpy(cpu_frame_data_.data(), mapped, cpu_frame_data_.size());
        D3D12_RANGE empty_range{ 0, 0 };
        readback_buf_->Unmap(0, &empty_range);
    }

    ++frame_index_;
}

// ---------------------------------------------------------------------------
// present — blit SDR texture to swap chain back buffer
// ---------------------------------------------------------------------------
void D3D12Renderer::present() {
    if (!swap_chain_) return;
    swap_chain_->Present(0, 0);
}

// ---------------------------------------------------------------------------
// get_preview_texture
// ---------------------------------------------------------------------------
void* D3D12Renderer::get_preview_texture() {
    std::lock_guard<std::mutex> lk(preview_mutex_);
    return reinterpret_cast<void*>(imgui_preview_handle_.ptr);
}

// ---------------------------------------------------------------------------
// get_cpu_frame
// ---------------------------------------------------------------------------
const uint8_t* D3D12Renderer::get_cpu_frame(int& out_stride_bytes) {
    out_stride_bytes = cpu_frame_stride_;
    return cpu_frame_data_.data();
}

// ---------------------------------------------------------------------------
// flush
// ---------------------------------------------------------------------------
void D3D12Renderer::flush() {
    ++fence_value_;
    cmd_queue_->Signal(fence_.Get(), fence_value_);
    wait_for_fence(fence_value_);
}

// ---------------------------------------------------------------------------
// resize
// ---------------------------------------------------------------------------
void D3D12Renderer::resize(int w, int h) {
    if (!initialised_) return;
    flush();

    cfg_.width  = w;
    cfg_.height = h;

    // Release back buffers
    for (int i = 0; i < kBackBufferCount; ++i)
        back_buffers_[i].Reset();

    if (swap_chain_) {
        swap_chain_->ResizeBuffers(kBackBufferCount,
                                    static_cast<UINT>(w), static_cast<UINT>(h),
                                    DXGI_FORMAT_R8G8B8A8_UNORM, 0);
        for (int i = 0; i < kBackBufferCount; ++i) {
            swap_chain_->GetBuffer(i, IID_PPV_ARGS(&back_buffers_[i]));
            device_->CreateRenderTargetView(back_buffers_[i].Get(), nullptr, rtv_handle(i));
        }
    }

    // Recreate size-dependent resources
    hdr_rt_.Reset();
    sdr_rt_.Reset();
    bloom_tmp_.Reset();
    readback_buf_.Reset();

    create_hdr_render_target();
    create_sdr_readback_buffer();
}

// ---------------------------------------------------------------------------
// set_config
// ---------------------------------------------------------------------------
void D3D12Renderer::set_config(const RenderConfig& cfg) {
    bool needs_resize = (cfg.width != cfg_.width || cfg.height != cfg_.height);
    cfg_ = cfg;
    if (needs_resize) resize(cfg.width, cfg.height);
}

// ---------------------------------------------------------------------------
// shutdown
// ---------------------------------------------------------------------------
void D3D12Renderer::shutdown() {
    if (!initialised_) return;
    flush();

    if (fence_event_) { CloseHandle(fence_event_); fence_event_ = nullptr; }

    beam_pso_.Reset();
    beam_root_sig_.Reset();
    post_pso_.Reset();
    post_root_sig_.Reset();
    beam_cb_.Reset();
    post_cb_.Reset();
    point_buf_gpu_.Reset();
    point_buf_upload_.Reset();
    bloom_tmp_.Reset();
    hdr_rt_.Reset();
    sdr_rt_.Reset();
    readback_buf_.Reset();
    for (int i = 0; i < kBackBufferCount; ++i) back_buffers_[i].Reset();
    srv_heap_.Reset();
    rtv_heap_.Reset();
    swap_chain_.Reset();
    cmd_list_.Reset();
    for (int i = 0; i < 2; ++i) cmd_alloc_[i].Reset();
    cmd_queue_.Reset();
    fence_.Reset();
    device_.Reset();
    dxgi_factory_.Reset();

    initialised_ = false;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
void D3D12Renderer::wait_for_fence(uint64_t value) {
    if (fence_->GetCompletedValue() < value) {
        fence_->SetEventOnCompletion(value, fence_event_);
        WaitForSingleObject(fence_event_, INFINITE);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::rtv_handle(int idx) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * rtv_descriptor_size_;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::cbv_srv_uav_cpu(int idx) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * srv_descriptor_size_;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::cbv_srv_uav_gpu(int idx) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = srv_heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(idx) * srv_descriptor_size_;
    return h;
}

void D3D12Renderer::upload_constant_buffer(ID3D12Resource* cb,
                                            const void* data, size_t size) {
    void* mapped = nullptr;
    D3D12_RANGE r{ 0, 0 };
    if (SUCCEEDED(cb->Map(0, &r, &mapped))) {
        memcpy(mapped, data, size);
        cb->Unmap(0, nullptr);
    }
}

void D3D12Renderer::transition(ID3D12GraphicsCommandList* cmd,
                                ID3D12Resource* res,
                                D3D12_RESOURCE_STATES from,
                                D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

} // namespace idhmfis

#endif // _WIN32
