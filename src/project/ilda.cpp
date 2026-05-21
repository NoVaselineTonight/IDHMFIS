// ILDA file format — import and export.
// Spec: ILDA Standard Document #ilda001 rev 12.
// Byte order: big-endian throughout.

#include "ilda.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Pangolin 64-colour standard palette (indices 0-63).
//  Colours 64-255 fall back to white (255,255,255).
//  The palette cycles: red → yellow → green → cyan → blue → magenta → red,
//  then neutrals (white, greys, black) in the final block.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint8_t kPangolin64[64][3] = {
    // 0-15  red → yellow
    {255,   0,   0}, {255,  16,   0}, {255,  32,   0}, {255,  48,   0},
    {255,  64,   0}, {255,  80,   0}, {255,  96,   0}, {255, 112,   0},
    {255, 128,   0}, {255, 144,   0}, {255, 160,   0}, {255, 176,   0},
    {255, 192,   0}, {255, 208,   0}, {255, 224,   0}, {255, 255,   0},
    // 16-31  yellow → green → cyan
    {128, 255,   0}, {  0, 255,   0}, {  0, 255,  32}, {  0, 255,  64},
    {  0, 255,  96}, {  0, 255, 128}, {  0, 255, 160}, {  0, 255, 192},
    {  0, 255, 224}, {  0, 255, 255}, {  0, 224, 255}, {  0, 192, 255},
    {  0, 160, 255}, {  0, 128, 255}, {  0,  96, 255}, {  0,  64, 255},
    // 32-47  cyan → blue → magenta
    {  0,  32, 255}, {  0,   0, 255}, { 32,   0, 255}, { 64,   0, 255},
    { 96,   0, 255}, {128,   0, 255}, {160,   0, 255}, {192,   0, 255},
    {224,   0, 255}, {255,   0, 255}, {255,   0, 224}, {255,   0, 192},
    {255,   0, 160}, {255,   0, 128}, {255,   0,  96}, {255,   0,  64},
    // 48-55  magenta → red (loop)
    {255,   0,  32}, {255,   0,   0}, {255,  32,  32}, {255,  64,  64},
    {255,  96,  96}, {255, 128, 128}, {255, 160, 160}, {255, 192, 192},
    // 56-63  neutrals
    {255, 255, 255}, {192, 192, 192}, {128, 128, 128}, { 64,  64,  64},
    { 32,  32,  32}, {  0,   0,   0}, {  0,   0,   0}, {  0,   0,   0},
};

static void palette_lookup(uint8_t index,
                           const uint8_t custom_pal[768],
                           bool has_custom,
                           uint8_t& r, uint8_t& g, uint8_t& b)
{
    if (has_custom) {
        r = custom_pal[index * 3 + 0];
        g = custom_pal[index * 3 + 1];
        b = custom_pal[index * 3 + 2];
        return;
    }
    if (index < 64) {
        r = kPangolin64[index][0];
        g = kPangolin64[index][1];
        b = kPangolin64[index][2];
    } else {
        r = g = b = 255; // white for indices 64-255
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Low-level I/O helpers (big-endian)
// ─────────────────────────────────────────────────────────────────────────────
static bool read_bytes(std::ifstream& f, void* dst, std::streamsize n)
{
    f.read(static_cast<char*>(dst), n);
    return f.gcount() == n;
}

static uint16_t read_be16(std::ifstream& f)
{
    uint8_t b[2]{};
    f.read(reinterpret_cast<char*>(b), 2);
    return static_cast<uint16_t>((b[0] << 8) | b[1]);
}

static int16_t read_bes16(std::ifstream& f)
{
    return static_cast<int16_t>(read_be16(f));
}

static uint8_t read_u8(std::ifstream& f)
{
    uint8_t v = 0;
    f.read(reinterpret_cast<char*>(&v), 1);
    return v;
}

static void write_be16(std::vector<uint8_t>& buf, uint16_t v)
{
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void write_bes16(std::vector<uint8_t>& buf, int16_t v)
{
    write_be16(buf, static_cast<uint16_t>(v));
}

// ─────────────────────────────────────────────────────────────────────────────
//  ILDA 32-byte header
// ─────────────────────────────────────────────────────────────────────────────
struct IldaHeader {
    char     magic[4];       // "ILDA"
    uint8_t  reserved[3];
    uint8_t  format;
    char     name[8];
    char     company[8];
    uint16_t point_count;
    uint16_t frame_number;
    uint16_t total_frames;
    uint8_t  scanner;
    uint8_t  future;
};

static bool read_header(std::ifstream& f, IldaHeader& h)
{
    if (!read_bytes(f, h.magic, 4))      return false;
    if (!read_bytes(f, h.reserved, 3))   return false;
    h.format      = read_u8(f);
    if (!read_bytes(f, h.name, 8))       return false;
    if (!read_bytes(f, h.company, 8))    return false;
    h.point_count  = read_be16(f);
    h.frame_number = read_be16(f);
    h.total_frames = read_be16(f);
    h.scanner      = read_u8(f);
    h.future       = read_u8(f);
    return !f.fail();
}

static void write_header(std::vector<uint8_t>& buf,
                         uint8_t format,
                         const std::string& name,
                         const std::string& company,
                         uint16_t point_count,
                         uint16_t frame_number,
                         uint16_t total_frames,
                         uint8_t  scanner)
{
    // Magic
    buf.push_back('I'); buf.push_back('L');
    buf.push_back('D'); buf.push_back('A');
    // Reserved
    buf.push_back(0); buf.push_back(0); buf.push_back(0);
    // Format
    buf.push_back(format);
    // Name (8 bytes, null-padded)
    for (int i = 0; i < 8; ++i)
        buf.push_back(i < (int)name.size() ? static_cast<uint8_t>(name[i]) : 0);
    // Company (8 bytes, null-padded)
    for (int i = 0; i < 8; ++i)
        buf.push_back(i < (int)company.size() ? static_cast<uint8_t>(company[i]) : 0);
    // Counts
    write_be16(buf, point_count);
    write_be16(buf, frame_number);
    write_be16(buf, total_frames);
    buf.push_back(scanner);
    buf.push_back(0); // future
}

// ─────────────────────────────────────────────────────────────────────────────
//  Point parsers
// ─────────────────────────────────────────────────────────────────────────────
static PointBuffer parse_fmt0(std::ifstream& f, int count,
                               const uint8_t pal[768], bool has_custom)
{
    // Format 0: 3D indexed — X Y Z status color_index (6 bytes)
    PointBuffer pts;
    pts.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        read_bes16(f);              // Z — ignored for 2D output
        uint8_t status = read_u8(f);
        uint8_t ci     = read_u8(f);
        bool blanked   = (status & 0x40) != 0;
        uint8_t r, g, b;
        palette_lookup(ci, pal, has_custom, r, g, b);
        pts.push_back({ x, y, r, g, b, blanked });
    }
    return pts;
}

static PointBuffer parse_fmt1(std::ifstream& f, int count,
                               const uint8_t pal[768], bool has_custom)
{
    // Format 1: 2D indexed — X Y status color_index (4 bytes)
    PointBuffer pts;
    pts.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        uint8_t status = read_u8(f);
        uint8_t ci     = read_u8(f);
        bool blanked   = (status & 0x40) != 0;
        uint8_t r, g, b;
        palette_lookup(ci, pal, has_custom, r, g, b);
        pts.push_back({ x, y, r, g, b, blanked });
    }
    return pts;
}

static PointBuffer parse_fmt3(std::ifstream& f, int count)
{
    // Format 3: 3D true colour — X Y Z status B G R (8 bytes)
    PointBuffer pts;
    pts.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        read_bes16(f);              // Z
        uint8_t status = read_u8(f);
        uint8_t b = read_u8(f), g = read_u8(f), r = read_u8(f);
        bool blanked = (status & 0x40) != 0;
        pts.push_back({ x, y, r, g, b, blanked });
    }
    return pts;
}

static PointBuffer parse_fmt4(std::ifstream& f, int count)
{
    // Format 4: 2D true colour — X Y status B G R (6 bytes)
    PointBuffer pts;
    pts.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        int16_t x = read_bes16(f);
        int16_t y = read_bes16(f);
        uint8_t status = read_u8(f);
        uint8_t b = read_u8(f), g = read_u8(f), r = read_u8(f);
        bool blanked = (status & 0x40) != 0;
        pts.push_back({ x, y, r, g, b, blanked });
    }
    return pts;
}

static PointBuffer parse_fmt5(std::ifstream& f, int count)
{
    // Format 5: alias for format 4 (2D true colour)
    return parse_fmt4(f, count);
}

// ─────────────────────────────────────────────────────────────────────────────
//  RDP point reduction
// ─────────────────────────────────────────────────────────────────────────────
static float point_to_segment_dist2(const LaserPoint& p,
                                     const LaserPoint& a,
                                     const LaserPoint& b)
{
    float dx = static_cast<float>(b.x - a.x);
    float dy = static_cast<float>(b.y - a.y);
    float len2 = dx * dx + dy * dy;
    if (len2 < 1.f) {
        float ex = static_cast<float>(p.x - a.x);
        float ey = static_cast<float>(p.y - a.y);
        return ex * ex + ey * ey;
    }
    float t = ((static_cast<float>(p.x - a.x)) * dx +
               (static_cast<float>(p.y - a.y)) * dy) / len2;
    t = std::clamp(t, 0.f, 1.f);
    float px = static_cast<float>(a.x) + t * dx - static_cast<float>(p.x);
    float py = static_cast<float>(a.y) + t * dy - static_cast<float>(p.y);
    return px * px + py * py;
}

static void rdp(const std::vector<LaserPoint>& pts,
                int first, int last,
                float eps2,
                std::vector<bool>& keep)
{
    if (last <= first + 1) return;

    float max_d2 = 0.f;
    int   max_i  = first;
    for (int i = first + 1; i < last; ++i) {
        if (pts[i].blanked) { keep[i] = true; continue; } // blanks always kept
        float d2 = point_to_segment_dist2(pts[i], pts[first], pts[last]);
        if (d2 > max_d2) { max_d2 = d2; max_i = i; }
    }

    if (max_d2 > eps2) {
        keep[max_i] = true;
        rdp(pts, first, max_i, eps2, keep);
        rdp(pts, max_i, last,  eps2, keep);
    }
}

static PointBuffer rdp_reduce(const PointBuffer& in, int max_points)
{
    if (max_points <= 0 || (int)in.size() <= max_points) return in;

    // Binary-search for epsilon that gives approx max_points
    float lo = 0.f, hi = 65535.f * 65535.f;
    PointBuffer best = in;

    for (int iter = 0; iter < 32; ++iter) {
        float mid = (lo + hi) * 0.5f;
        std::vector<bool> keep(in.size(), false);
        keep.front() = true;
        keep.back()  = true;
        rdp(in, 0, static_cast<int>(in.size()) - 1, mid, keep);

        int cnt = 0;
        for (bool k : keep) if (k) ++cnt;

        if (cnt <= max_points) {
            // Collect result
            PointBuffer candidate;
            candidate.reserve(static_cast<size_t>(cnt));
            for (size_t i = 0; i < in.size(); ++i)
                if (keep[i]) candidate.push_back(in[i]);
            best = candidate;
            hi = mid;
        } else {
            lo = mid;
        }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
//  encode_frame_f4 — public API
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> encode_frame_f4(const PointBuffer& buf,
                                      int frame_num,
                                      int total_frames,
                                      const std::string& name,
                                      int projector)
{
    std::vector<uint8_t> out;
    out.reserve(32 + buf.size() * 6);

    write_header(out,
                 /*format*/ 4,
                 name, /*company*/ "",
                 static_cast<uint16_t>(buf.size()),
                 static_cast<uint16_t>(frame_num),
                 static_cast<uint16_t>(total_frames),
                 static_cast<uint8_t>(projector));

    for (size_t i = 0; i < buf.size(); ++i) {
        const LaserPoint& p = buf[i];
        write_bes16(out, p.x);
        write_bes16(out, p.y);

        uint8_t status = 0;
        if (p.blanked)              status |= 0x40; // blanked
        if (i == buf.size() - 1)    status |= 0x80; // last point flag
        out.push_back(status);
        // BGR order
        out.push_back(p.b);
        out.push_back(p.g);
        out.push_back(p.r);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ilda_import — public API
// ─────────────────────────────────────────────────────────────────────────────
IldaFile ilda_import(const std::string& path, std::string& error_msg)
{
    IldaFile result;
    result.source_path = path;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error_msg = "Cannot open file: " + path;
        return result;
    }

    // Default palette (Pangolin 64)
    uint8_t pal[768]{};
    for (int i = 0; i < 64; ++i) {
        pal[i * 3 + 0] = kPangolin64[i][0];
        pal[i * 3 + 1] = kPangolin64[i][1];
        pal[i * 3 + 2] = kPangolin64[i][2];
    }
    for (int i = 64; i < 256; ++i) {
        pal[i * 3 + 0] = 255;
        pal[i * 3 + 1] = 255;
        pal[i * 3 + 2] = 255;
    }
    bool has_custom = false;

    while (f.good() && !f.eof()) {
        IldaHeader hdr{};
        if (!read_header(f, hdr)) break; // EOF or short read

        if (std::memcmp(hdr.magic, "ILDA", 4) != 0) {
            error_msg = "Invalid ILDA magic bytes";
            result.frames.clear();
            return result;
        }

        // EOF marker: point_count == 0 and format is 0 or 1
        if (hdr.point_count == 0 && (hdr.format == 0 || hdr.format == 1)) break;

        // Format 2: custom palette frame (no point data)
        if (hdr.format == 2) {
            // 256 × 3 bytes RGB
            if (!read_bytes(f, pal, 768)) {
                error_msg = "Truncated palette frame";
                result.frames.clear();
                return result;
            }
            std::copy(std::begin(pal), std::end(pal), result.palette.begin());
            result.has_custom_palette = true;
            has_custom = true;
            continue;
        }

        PointBuffer pts;
        int count = static_cast<int>(hdr.point_count);

        switch (hdr.format) {
            case 0: pts = parse_fmt0(f, count, pal, has_custom); break;
            case 1: pts = parse_fmt1(f, count, pal, has_custom); break;
            case 3: pts = parse_fmt3(f, count); break;
            case 4: pts = parse_fmt4(f, count); break;
            case 5: pts = parse_fmt5(f, count); break;
            default:
                error_msg = "Unsupported ILDA format: " + std::to_string(hdr.format);
                result.frames.clear();
                return result;
        }

        if (f.fail()) {
            error_msg = "Read error in ILDA file";
            result.frames.clear();
            return result;
        }

        IldaFrame frame;
        frame.format    = hdr.format;
        frame.name      = std::string(hdr.name,    8);
        frame.company   = std::string(hdr.company, 8);
        frame.projector = hdr.scanner;
        frame.points    = std::move(pts);
        result.frames.push_back(std::move(frame));
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ilda_export — public API
// ─────────────────────────────────────────────────────────────────────────────
bool ilda_export(const std::string& path,
                 const std::vector<PointBuffer>& frames,
                 int max_points_per_frame,
                 std::string* error_msg)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error_msg) *error_msg = "Cannot open file for writing: " + path;
        return false;
    }

    const int total = static_cast<int>(frames.size());

    for (int i = 0; i < total; ++i) {
        PointBuffer frame = frames[i];
        if (max_points_per_frame > 0)
            frame = rdp_reduce(frame, max_points_per_frame);

        std::vector<uint8_t> encoded = encode_frame_f4(frame, i, total);
        f.write(reinterpret_cast<const char*>(encoded.data()),
                static_cast<std::streamsize>(encoded.size()));
        if (f.fail()) {
            if (error_msg) *error_msg = "Write error";
            return false;
        }
    }

    // Write EOF marker: format 0 header with point_count = 0
    std::vector<uint8_t> eof_hdr;
    write_header(eof_hdr, 0, "", "", 0, 0, static_cast<uint16_t>(total), 0);
    f.write(reinterpret_cast<const char*>(eof_hdr.data()),
            static_cast<std::streamsize>(eof_hdr.size()));

    return !f.fail();
}

} // namespace idhmfis
