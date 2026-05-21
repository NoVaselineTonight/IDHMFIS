#pragma once
// laser_rasterizer.h — Software rasterizer: laser PointBuffer -> RGBA image
// Renders laser vectors as glowing beam lines on a black canvas.
// Output is suitable for NDI or D3D12 texture upload.

#include "../core/types.h"
#include <vector>
#include <cstdint>

namespace idhmfis {

struct RasterConfig {
    int   width        = 1920;
    int   height       = 1080;
    float beam_radius  = 2.5f;   // core beam radius in pixels
    float glow_radius  = 8.f;    // soft glow radius in pixels
    float glow_alpha   = 0.25f;  // glow opacity
    float beam_alpha   = 1.f;    // core beam opacity
    bool  additive     = true;   // additive blend (laser look)

    // ── Otaniemi Mode (video-projector optimisation) ──────────────────────────
    bool  otaniemi_enabled          = false;
    float otaniemi_line_thickness   = 4.0f;   // override beam_radius when enabled
    float otaniemi_brightness_boost = 1.3f;   // multiply final RGB values (clamped 255)
    float otaniemi_glow_radius      = 8.0f;   // override glow_radius when enabled
    bool  otaniemi_auto_fill        = true;   // flood-fill enclosed regions after draw
};

// Rasterize a PointBuffer into an RGBA8 image.
// pts: laser points (normalised -1..1 coords, blanked flag, RGB)
// pixels: output buffer, must be width*height*4 bytes (RGBA)
void rasterize_frame(const PointBuffer& pts,
                     uint8_t*           pixels,
                     const RasterConfig& cfg);

} // namespace idhmfis
