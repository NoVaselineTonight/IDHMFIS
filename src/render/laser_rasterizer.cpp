// laser_rasterizer.cpp — Software rasterizer: laser PointBuffer -> RGBA image

#include "laser_rasterizer.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <vector>

namespace idhmfis {

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------

// Squared distance from point (px, py) to the nearest point on segment
// (x0,y0)-(x1,y1).  Returns the actual distance (not squared).
static float dist_to_segment(float px, float py,
                              float x0, float y0,
                              float x1, float y1)
{
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    if (len2 < 1e-6f) {
        // Degenerate segment — treat as point
        float ex = px - x0;
        float ey = py - y0;
        return std::sqrt(ex * ex + ey * ey);
    }
    // Project onto segment, clamped to [0, 1]
    float t = ((px - x0) * dx + (py - y0) * dy) / len2;
    t = std::max(0.f, std::min(1.f, t));
    float cx = x0 + t * dx;
    float cy = y0 + t * dy;
    float ex = px - cx;
    float ey = py - cy;
    return std::sqrt(ex * ex + ey * ey);
}

// Additive blend a single channel: out = min(255, out + delta)
static inline void add_channel(uint8_t& channel, float delta)
{
    int v = static_cast<int>(channel) + static_cast<int>(delta + 0.5f);
    channel = static_cast<uint8_t>(v < 255 ? v : 255);
}

// ---------------------------------------------------------------------------
//  otaniemi_scanline_fill — flood-fill enclosed regions on the rendered bitmap.
//  For each row, find spans of lit pixels and fill the gaps between them so
//  that closed shapes appear solid rather than just outlined.
//  Only the horizontal scanline approach is used for speed.
// ---------------------------------------------------------------------------
static void otaniemi_scanline_fill(uint8_t* pixels, int W, int H)
{
    // Threshold: a pixel is considered "lit" if its luminance is above this.
    static constexpr int kLitThreshold = 20;

    for (int row = 0; row < H; ++row) {
        // Scan the row to find the leftmost and rightmost lit pixel.
        int first_lit = -1;
        int last_lit  = -1;

        for (int col = 0; col < W; ++col) {
            const uint8_t* pix = pixels + (row * W + col) * 4;
            int lum = static_cast<int>(pix[0]) + static_cast<int>(pix[1]) + static_cast<int>(pix[2]);
            if (lum > kLitThreshold) {
                if (first_lit < 0) first_lit = col;
                last_lit = col;
            }
        }

        if (first_lit < 0 || first_lit >= last_lit)
            continue;   // nothing to fill on this row

        // Accumulate the average color of the border pixels to use as fill color.
        // This keeps the fill the same hue as the shape outline.
        float sum_r = 0.f, sum_g = 0.f, sum_b = 0.f;
        int   count = 0;
        for (int col = first_lit; col <= last_lit; ++col) {
            const uint8_t* pix = pixels + (row * W + col) * 4;
            int lum = static_cast<int>(pix[0]) + static_cast<int>(pix[1]) + static_cast<int>(pix[2]);
            if (lum > kLitThreshold) {
                sum_r += static_cast<float>(pix[0]);
                sum_g += static_cast<float>(pix[1]);
                sum_b += static_cast<float>(pix[2]);
                ++count;
            }
        }
        if (count == 0) continue;

        // Dim fill: use 40% of the average border brightness so filled areas
        // look distinct from the bright outline.
        static constexpr float kFillScale = 0.40f;
        uint8_t fr = static_cast<uint8_t>(std::min(255.f, (sum_r / static_cast<float>(count)) * kFillScale));
        uint8_t fg = static_cast<uint8_t>(std::min(255.f, (sum_g / static_cast<float>(count)) * kFillScale));
        uint8_t fb = static_cast<uint8_t>(std::min(255.f, (sum_b / static_cast<float>(count)) * kFillScale));

        // Fill unlit pixels between first_lit and last_lit.
        for (int col = first_lit + 1; col < last_lit; ++col) {
            uint8_t* pix = pixels + (row * W + col) * 4;
            int lum = static_cast<int>(pix[0]) + static_cast<int>(pix[1]) + static_cast<int>(pix[2]);
            if (lum <= kLitThreshold) {
                pix[0] = fr;
                pix[1] = fg;
                pix[2] = fb;
                // pix[3] (alpha) is already 255
            }
        }
    }
}

// ---------------------------------------------------------------------------
//  rasterize_frame
// ---------------------------------------------------------------------------
void rasterize_frame(const PointBuffer& pts,
                     uint8_t*           pixels,
                     const RasterConfig& cfg)
{
    const int   W          = cfg.width;
    const int   H          = cfg.height;

    // Otaniemi mode overrides beam_radius and glow_radius when enabled.
    const float beam_r     = cfg.otaniemi_enabled
                                 ? cfg.otaniemi_line_thickness
                                 : cfg.beam_radius;
    const float glow_r     = cfg.otaniemi_enabled
                                 ? cfg.otaniemi_glow_radius
                                 : cfg.glow_radius;
    const float glow_alpha = cfg.glow_alpha;
    const float beam_alpha = cfg.beam_alpha;

    // Clear to black, fully opaque
    std::memset(pixels, 0, static_cast<size_t>(W) * static_cast<size_t>(H) * 4u);

    // Set alpha channel to 255 for every pixel.
    // BUG #65: Use ptrdiff_t so the loop variable and index arithmetic do not
    // overflow when W * H exceeds INT_MAX (resolutions above ~16K × 16K).
    for (ptrdiff_t i = 0; i < static_cast<ptrdiff_t>(W) * H; ++i)
        pixels[i * 4 + 3] = 255u;

    if (pts.size() < 2)
        return;

    const float max_search_r = glow_r + 1.f;

    for (size_t i = 1; i < pts.size(); ++i) {
        const LaserPoint& p0 = pts[i - 1];
        const LaserPoint& p1 = pts[i];

        // Skip blanked segments
        if (p0.blanked || p1.blanked)
            continue;

        // Convert normalised coords to pixel coords (Y-flipped for screen)
        float x0 = (p0.nx() * 0.5f + 0.5f) * static_cast<float>(W);
        float y0 = (-p0.ny() * 0.5f + 0.5f) * static_cast<float>(H);
        float x1 = (p1.nx() * 0.5f + 0.5f) * static_cast<float>(W);
        float y1 = (-p1.ny() * 0.5f + 0.5f) * static_cast<float>(H);

        // Bounding box of the segment, expanded by glow_radius
        int bx0 = std::max(0,     static_cast<int>(std::floor(std::min(x0, x1) - max_search_r)));
        int by0 = std::max(0,     static_cast<int>(std::floor(std::min(y0, y1) - max_search_r)));
        int bx1 = std::min(W - 1, static_cast<int>(std::ceil (std::max(x0, x1) + max_search_r)));
        int by1 = std::min(H - 1, static_cast<int>(std::ceil (std::max(y0, y1) + max_search_r)));

        for (int py = by0; py <= by1; ++py) {
            for (int px = bx0; px <= bx1; ++px) {
                float dist = dist_to_segment(static_cast<float>(px) + 0.5f,
                                             static_cast<float>(py) + 0.5f,
                                             x0, y0, x1, y1);

                if (dist >= glow_r)
                    continue;

                // Linearly interpolate color along segment
                float t = 0.f;
                {
                    float dx = x1 - x0;
                    float dy = y1 - y0;
                    float len2 = dx * dx + dy * dy;
                    if (len2 > 1e-6f) {
                        t = ((static_cast<float>(px) + 0.5f - x0) * dx +
                             (static_cast<float>(py) + 0.5f - y0) * dy) / len2;
                        t = std::max(0.f, std::min(1.f, t));
                    }
                }

                float cr = static_cast<float>(p0.r) + t * static_cast<float>(p1.r - p0.r);
                float cg = static_cast<float>(p0.g) + t * static_cast<float>(p1.g - p0.g);
                float cb = static_cast<float>(p0.b) + t * static_cast<float>(p1.b - p0.b);

                // BUG #65: Use ptrdiff_t to prevent int overflow at high resolutions.
                ptrdiff_t idx = (static_cast<ptrdiff_t>(py) * W + px) * 4;
                uint8_t* pix = pixels + idx;

                // Glow contribution: (1 - dist/glow_r)^2 * glow_alpha
                float glow_intensity = (1.f - dist / glow_r);
                glow_intensity = glow_intensity * glow_intensity * glow_alpha;

                add_channel(pix[0], glow_intensity * cr);
                add_channel(pix[1], glow_intensity * cg);
                add_channel(pix[2], glow_intensity * cb);

                // Core beam contribution
                if (dist < beam_r) {
                    float core_intensity = beam_alpha;
                    add_channel(pix[0], core_intensity * cr);
                    add_channel(pix[1], core_intensity * cg);
                    add_channel(pix[2], core_intensity * cb);
                }
            }
        }
    }

    // ── Otaniemi post-processing ──────────────────────────────────────────────
    if (cfg.otaniemi_enabled) {

        // Auto-fill: scanline-fill enclosed shapes before brightness boost.
        if (cfg.otaniemi_auto_fill) {
            otaniemi_scanline_fill(pixels, W, H);
        }

        // Brightness boost: multiply all non-black pixels by the boost factor,
        // clamped to 255.
        if (cfg.otaniemi_brightness_boost > 1.0f) {
            const float boost = cfg.otaniemi_brightness_boost;
            const int   n     = W * H;
            for (int i = 0; i < n; ++i) {
                uint8_t* pix = pixels + i * 4;
                // Only boost lit pixels (skip pure black to avoid brightening background noise)
                if (pix[0] | pix[1] | pix[2]) {
                    int r = static_cast<int>(static_cast<float>(pix[0]) * boost + 0.5f);
                    int g = static_cast<int>(static_cast<float>(pix[1]) * boost + 0.5f);
                    int b = static_cast<int>(static_cast<float>(pix[2]) * boost + 0.5f);
                    pix[0] = static_cast<uint8_t>(r < 255 ? r : 255);
                    pix[1] = static_cast<uint8_t>(g < 255 ? g : 255);
                    pix[2] = static_cast<uint8_t>(b < 255 ? b : 255);
                }
            }
        }
    }
}

} // namespace idhmfis
