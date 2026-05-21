// ILDA file parser. Formats 0, 1, 4, 5.
// Spec: ILDA Standard Document #ilda001 rev12
#include "ilda_sequence.h"
#include <fstream>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace idhmfis {

static uint16_t read_be16(std::ifstream& f) {
    uint8_t b[2]; f.read((char*)b, 2);
    return (uint16_t)((b[0] << 8) | b[1]);
}
static int16_t read_bes16(std::ifstream& f) {
    return (int16_t)read_be16(f);
}
static uint8_t read_u8(std::ifstream& f) {
    uint8_t v; f.read((char*)&v, 1); return v;
}

struct ILDAHeader {
    char     magic[4];  // "ILDA"
    uint8_t  reserved[3];
    uint8_t  format;    // 0/1/4/5
    char     name[8];
    char     company[8];
    uint16_t point_count;
    uint16_t frame_number;
    uint16_t total_frames;
    uint8_t  scanner;
    uint8_t  future;
};

bool ILDASequence::load(const std::string& path) {
    frames_.clear(); error_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) { error_ = "Cannot open file: " + path; return false; }
    if (!parse_ilda(f)) return false;
    return !frames_.empty();
}

bool ILDASequence::parse_ilda(std::ifstream& f) {
    while (!f.eof()) {
        ILDAHeader hdr{};
        f.read(hdr.magic, 4);
        if (f.gcount() < 4) break;
        if (std::memcmp(hdr.magic, "ILDA", 4) != 0) {
            error_ = "Invalid ILDA magic bytes";
            return false;
        }
        f.read((char*)hdr.reserved, 3);
        hdr.format = read_u8(f);
        f.read(hdr.name, 8);
        f.read(hdr.company, 8);
        hdr.point_count  = read_be16(f);
        hdr.frame_number = read_be16(f);
        hdr.total_frames = read_be16(f);
        hdr.scanner = read_u8(f);
        hdr.future  = read_u8(f);

        if (hdr.point_count == 0) break;  // terminator section

        PointBuffer pts;
        switch (hdr.format) {
            case 0: pts = read_fmt_0(f, hdr.point_count); break;
            case 1: pts = read_fmt_1(f, hdr.point_count); break;
            case 4: pts = read_fmt_4(f, hdr.point_count); break;
            case 5: pts = read_fmt_5(f, hdr.point_count); break;
            default:
                error_ = "Unsupported ILDA format: " + std::to_string(hdr.format);
                return false;
        }
        if (f.fail()) { error_ = "Read error in ILDA file"; return false; }

        ILDAFrame frame;
        frame.name = std::string(hdr.name, 8);
        frame.points = std::move(pts);
        frames_.push_back(std::move(frame));
    }
    return true;
}

PointBuffer ILDASequence::read_fmt_0(std::ifstream& f, int count) {
    // Format 0: 3D with indexed color
    // Per point: X(s16 BE), Y(s16 BE), Z(s16 BE), status(u8), color_index(u8)
    // status bit 6 = blanked, bit 7 = last point
    static const uint8_t palette[][3] = {
        {255,  0,  0},{255, 16,  0},{255, 32,  0},{255, 48,  0},
        {255, 64,  0},{255, 80,  0},{255, 96,  0},{255,112,  0},
        {255,128,  0},{255,144,  0},{255,160,  0},{255,176,  0},
        {255,192,  0},{255,208,  0},{255,224,  0},{255,255,  0},
        {128,255,  0},{  0,255,  0},{  0,255, 64},{  0,255,128},
        {  0,128,255},{  0,  0,255},{  0,  0,128},{128,  0,255},
        {255,  0,255},{255,  0,128},{255,255,255},{128,128,128},
        {  0,  0,  0},{  0,  0,  0},{  0,  0,  0},{  0,  0,  0},
    };
    static constexpr int kPalSize = (int)(sizeof(palette)/sizeof(palette[0]));

    PointBuffer pts;
    pts.reserve(count);
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        read_bes16(f);  // Z ignored for 2D output
        uint8_t status = read_u8(f);
        uint8_t ci     = read_u8(f);
        bool blanked   = (status >> 6) & 1;
        ci = ci < kPalSize ? ci : 0;
        pts.push_back({ x, y, palette[ci][0], palette[ci][1], palette[ci][2], blanked });
    }
    return pts;
}

PointBuffer ILDASequence::read_fmt_1(std::ifstream& f, int count) {
    // Format 1: 2D with indexed color
    // Per point: X(s16 BE), Y(s16 BE), status(u8), color_index(u8)
    static const uint8_t palette[][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255},
        {0,255,255},{255,128,0},{255,255,255},{128,128,128}
    };
    static constexpr int kPalSize = (int)(sizeof(palette)/sizeof(palette[0]));

    PointBuffer pts; pts.reserve(count);
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        uint8_t status = read_u8(f);
        uint8_t ci     = read_u8(f);
        bool blanked   = (status >> 6) & 1;
        ci = ci < kPalSize ? ci : 0;
        pts.push_back({ x, y, palette[ci][0], palette[ci][1], palette[ci][2], blanked });
    }
    return pts;
}

PointBuffer ILDASequence::read_fmt_4(std::ifstream& f, int count) {
    // Format 4: 3D true-color
    // Per point: X(s16 BE), Y(s16 BE), Z(s16 BE), status(u8), B, G, R (8-bit each)
    PointBuffer pts; pts.reserve(count);
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        read_bes16(f);   // Z
        uint8_t status = read_u8(f);
        uint8_t b = read_u8(f), g = read_u8(f), r = read_u8(f);
        bool blanked = (status >> 6) & 1;
        pts.push_back({ x, y, r, g, b, blanked });
    }
    return pts;
}

PointBuffer ILDASequence::read_fmt_5(std::ifstream& f, int count) {
    // Format 5: 2D true-color
    // Per point: X(s16 BE), Y(s16 BE), status(u8), B, G, R
    PointBuffer pts; pts.reserve(count);
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        uint8_t status = read_u8(f);
        uint8_t b = read_u8(f), g = read_u8(f), r = read_u8(f);
        bool blanked = (status >> 6) & 1;
        pts.push_back({ x, y, r, g, b, blanked });
    }
    return pts;
}

PointBuffer ILDASequence::frame_at(double t, float fps) const {
    if (frames_.empty()) return {};
    int idx = static_cast<int>(t * fps);
    idx %= static_cast<int>(frames_.size());
    if (idx < 0) idx += static_cast<int>(frames_.size());
    return frames_[idx].points;
}

} // namespace idhmfis
