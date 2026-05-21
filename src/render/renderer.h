#pragma once
// renderer.h — Abstract renderer interface for IDHMFIS laser simulation.
// Factory function selects D3D12 on Windows, Vulkan on other platforms.

#include "../core/types.h"
#include <memory>

namespace idhmfis {

// ---------------------------------------------------------------------------
// RenderConfig — parameters controlling visual appearance and output format.
// All parameters can be hot-reloaded via set_config() without device reset.
// ---------------------------------------------------------------------------
struct RenderConfig {
    int   width          = 1920;
    int   height         = 1080;
    int   fps            = 60;
    float beam_thickness = 2.0f;   // pixels — controls Gaussian sigma
    float bloom_radius   = 8.0f;   // pixels — separable Gaussian blur radius
    float haze_density   = 0.4f;   // 0-1 — atmospheric fog opacity
    float exposure       = 1.0f;   // linear multiplier before tone-map
    bool  film_grain     = false;
    float grain_amount   = 0.02f;  // 0-1 normalised noise amplitude
    bool  output_bgra    = false;  // false = UYVY (NDI preferred), true = BGRA
};

// ---------------------------------------------------------------------------
// IRenderer — renderer interface.
// Lifecycle: init() → [resize/set_config]* → render_frame() + present() → shutdown()
// render_frame() and present() are called from the NDI worker thread.
// get_preview_texture() is called from the UI thread — synchronisation is the
// implementation's responsibility (double-buffer the SRV descriptor).
// ---------------------------------------------------------------------------
class IRenderer {
public:
    virtual ~IRenderer() = default;

    // Initialise GPU device, swap chain, and pipelines.
    // native_window_handle: HWND on Windows, NSWindow* on macOS.
    // Returns false if device creation or shader compilation fails.
    virtual bool init(void* native_window_handle, const RenderConfig& cfg) = 0;

    // Rebuild swap chain / render targets for a new client area.
    // Safe to call while rendering is paused.
    virtual void resize(int w, int h) = 0;

    // Hot-reload parameters.  Does NOT require pipeline rebuild unless
    // output_bgra changes (which requires a format switch).
    virtual void set_config(const RenderConfig& cfg) = 0;

    // Submit a point buffer for rasterisation.  This call drives:
    //   1. Upload GPU point buffer
    //   2. Beam raster compute pass (HDR accumulation)
    //   3. Post-process pass (bloom + haze + exposure + optional grain)
    //   4. Readback / copy to NDI staging buffer
    //   5. Signal UI preview texture ready
    virtual void render_frame(const PointBuffer& pts) = 0;

    // Blit the current back buffer to the window.  Must be called on the
    // thread that owns the window's swap chain.
    virtual void present() = 0;

    // Returns an ImTextureID (void*) pointing to the GPU-resident preview
    // texture (SRV handle on D3D12, VkDescriptorSet on Vulkan).
    // Returns nullptr before the first frame is rendered.
    virtual void* get_preview_texture() = 0;

    // Returns a pointer to the latest completed CPU-side pixel data.
    // Format is BGRA or UYVY depending on RenderConfig::output_bgra.
    // The pointer is valid until the next call to render_frame().
    virtual const uint8_t* get_cpu_frame(int& out_stride_bytes) = 0;

    // Block until all GPU work issued by render_frame() is complete.
    // Called before shutdown and before NDI reads the staging buffer.
    virtual void flush() = 0;

    // Destroy all GPU objects in reverse creation order.
    virtual void shutdown() = 0;
};

// ---------------------------------------------------------------------------
// Factory — creates D3D12 renderer on Windows, Vulkan elsewhere.
// Returns nullptr if the platform renderer cannot be created (e.g. no GPU).
// ---------------------------------------------------------------------------
std::unique_ptr<IRenderer> create_renderer(const RenderConfig& cfg);

} // namespace idhmfis
