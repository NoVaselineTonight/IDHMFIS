#pragma once
// ILDA file format support — import and export.
// Supports ILDA formats 0 (3D indexed), 1 (2D indexed), 2 (palette),
// 3 (3D true color), 4 (2D true color), 5 (2D true color alias).
// All coordinates are stored as int16 in ILDA space (-32767..32767).

#include "../core/types.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  A single frame loaded from an ILDA file
// ─────────────────────────────────────────────────────────────────────────────
struct IldaFrame {
    int         format    = 0;       // 0,1,3,4,5
    std::string name;
    std::string company;
    int         projector = 0;
    PointBuffer points;              // coords in native int16 ILDA space
};

// ─────────────────────────────────────────────────────────────────────────────
//  An ILDA file (sequence of frames, optional custom palette)
// ─────────────────────────────────────────────────────────────────────────────
struct IldaFile {
    std::vector<IldaFrame>     frames;
    std::string                source_path;
    std::array<uint8_t, 768>   palette{};   // 256 × RGB (from format-2 header)
    bool                       has_custom_palette = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Import: reads all frames from an ILDA file.
//  Returns an empty IldaFile on error; error description written to error_msg.
// ─────────────────────────────────────────────────────────────────────────────
IldaFile ilda_import(const std::string& path, std::string& error_msg);

// ─────────────────────────────────────────────────────────────────────────────
//  Export: writes frames to an ILDA file (format 4 — 2D true colour).
//  Applies RDP point reduction per frame if max_points_per_frame > 0.
//  Returns false on error; writes reason to *error_msg if non-null.
// ─────────────────────────────────────────────────────────────────────────────
bool ilda_export(const std::string& path,
                 const std::vector<PointBuffer>& frames,
                 int         max_points_per_frame = 0,
                 std::string* error_msg           = nullptr);

// ─────────────────────────────────────────────────────────────────────────────
//  Encode a single PointBuffer as ILDA format 4 binary data
//  (header + point records).  Suitable for streaming into a file.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> encode_frame_f4(const PointBuffer& buf,
                                      int         frame_num,
                                      int         total_frames,
                                      const std::string& name      = "",
                                      int         projector        = 0);

} // namespace idhmfis
