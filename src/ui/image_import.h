#pragma once
// image_import.h — Raster-to-laser vectorizer for the IDHMFIS frame editor.
// Converts PNG/BMP/JPG images into laser PointBuffers via scanline tracing.

#include "../core/types.h"

#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  ImageImportConfig — controls how the raster image is converted to vectors
// ─────────────────────────────────────────────────────────────────────────────
struct ImageImportConfig {
    float threshold    = 0.5f;   // luminance threshold (0..1): pixels above this are lit
    int   blur_radius  = 1;      // pre-blur radius (0=none, 1=3x3 box blur)
    float scale        = 0.8f;   // output scale in normalised ±1 space
    float col_r        = 1.f;    // laser output colour red   (0..1)
    float col_g        = 1.f;    // laser output colour green (0..1)
    float col_b        = 1.f;    // laser output colour blue  (0..1)
    bool  invert       = false;  // invert luminance before thresholding
    int   max_points   = 4000;   // hard cap on output point count
};

// ─────────────────────────────────────────────────────────────────────────────
//  vectorize_image — load an image and convert it to a PointBuffer.
//  Returns an empty buffer on failure and writes to *error_out if provided.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer vectorize_image(const std::string& path,
                             const ImageImportConfig& cfg,
                             std::string* error_out = nullptr);

// ─────────────────────────────────────────────────────────────────────────────
//  ImageImportState — per-session state for the Import Image tool panel
// ─────────────────────────────────────────────────────────────────────────────
struct ImageImportState {
    char        loaded_path[512] = {};   // path selected by the user
    ImageImportConfig cfg;
    PointBuffer preview_pts;             // vectorized preview (updated on demand)
    bool        dirty     = true;        // true when preview needs re-vectorizing
    std::string error_msg;               // last error from vectorize_image
    int         preview_pt_count = 0;   // point count of last successful vectorize
    bool        open      = true;        // tracks window open state (close button)
};

// ─────────────────────────────────────────────────────────────────────────────
//  draw_image_import_panel — floating ImGui window shown when the Img tool is
//  active.  Returns true when the user clicks [Place in Frame].
// ─────────────────────────────────────────────────────────────────────────────
bool draw_image_import_panel(ImageImportState& st);

} // namespace idhmfis
