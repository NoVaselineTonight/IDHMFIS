#pragma once
// Core data types shared across all IDHMFIS subsystems.
// This file must compile with zero warnings and zero includes beyond the STL.

#include <cassert>
#include <cstdint>
#include <cmath>
#include <vector>
#include <array>
#include <string>
#include <map>
#include <algorithm>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Laser point — the fundamental unit of all output
//  x, y: normalised ±32767 (ILDA coordinate space)
//  r, g, b: 0–255
//  blanked: beam off (travel move)
//  focus: 0–255 (optional, for focus-adjustable projectors)
// ─────────────────────────────────────────────────────────────────────────────
struct LaserPoint {
    int16_t  x        = 0;
    int16_t  y        = 0;
    uint8_t  r        = 0;
    uint8_t  g        = 0;
    uint8_t  b        = 0;
    bool     blanked  = false;
    uint8_t  focus    = 128;

    static constexpr int16_t kMin = -32767;
    static constexpr int16_t kMax =  32767;

    // Construct from normalised [-1, 1] floats
    static LaserPoint from_norm(float nx, float ny,
                                uint8_t r_, uint8_t g_, uint8_t b_,
                                bool blank = false) {
        LaserPoint p;
        p.x       = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * 32767.f);
        p.y       = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * 32767.f);
        p.r       = r_;
        p.g       = g_;
        p.b       = b_;
        p.blanked = blank;
        return p;
    }

    float nx() const { return static_cast<float>(x) / 32767.f; }
    float ny() const { return static_cast<float>(y) / 32767.f; }
};

// A complete frame of laser points destined for the DAC and NDI rasterizer
using PointBuffer = std::vector<LaserPoint>;

// ─────────────────────────────────────────────────────────────────────────────
//  Float colour (linear, 0–1)
// ─────────────────────────────────────────────────────────────────────────────
struct Color4 {
    float r = 0.f, g = 0.f, b = 0.f, a = 1.f;

    Color4() = default;
    Color4(float r_, float g_, float b_, float a_ = 1.f)
        : r(r_), g(g_), b(b_), a(a_) {}

    static Color4 from_hsv(float h, float s, float v, float a = 1.f) {
        float c = v * s;
        float hp = h / 60.f;
        float x = c * (1.f - std::fabs(std::fmod(hp, 2.f) - 1.f));
        float m = v - c;
        float r_, g_, b_;
        int i = static_cast<int>(hp);
        switch (i % 6) {
            case 0: r_=c; g_=x; b_=0; break;
            case 1: r_=x; g_=c; b_=0; break;
            case 2: r_=0; g_=c; b_=x; break;
            case 3: r_=0; g_=x; b_=c; break;
            case 4: r_=x; g_=0; b_=c; break;
            default:r_=c; g_=0; b_=x; break;
        }
        return { r_+m, g_+m, b_+m, a };
    }

    Color4 lerp(const Color4& o, float t) const {
        return { r + (o.r-r)*t, g + (o.g-g)*t,
                 b + (o.b-b)*t, a + (o.a-a)*t };
    }

    uint8_t r8() const { return static_cast<uint8_t>(std::clamp(r,0.f,1.f)*255.f); }
    uint8_t g8() const { return static_cast<uint8_t>(std::clamp(g,0.f,1.f)*255.f); }
    uint8_t b8() const { return static_cast<uint8_t>(std::clamp(b,0.f,1.f)*255.f); }
    uint8_t a8() const { return static_cast<uint8_t>(std::clamp(a,0.f,1.f)*255.f); }

    bool operator==(const Color4& o) const {
        return r==o.r && g==o.g && b==o.b && a==o.a;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Generator parameter block
//  All generators share this parameter schema.
//  Additional semantics are generator-defined.
// ─────────────────────────────────────────────────────────────────────────────
struct GeneratorParams {
    float    speed       = 1.f;     // animation rate multiplier
    float    scale       = 1.f;     // spatial scale
    float    density     = 0.5f;    // point density / complexity
    float    param_a     = 0.5f;    // generator-specific A
    float    param_b     = 0.5f;    // generator-specific B
    float    param_c     = 0.5f;    // generator-specific C
    Color4   color_a     = { 1.f, 0.2f, 0.9f };
    Color4   color_b     = { 0.1f, 0.6f, 1.f };
    float    rotation    = 0.f;     // radians
    float    pan         = 0.f;     // normalised offset -1..1
    float    tilt        = 0.f;
    float    zoom        = 1.f;     // output zoom
    float    intensity   = 1.f;     // 0..1 master level
    bool     blanked     = false;   // hard blackout
    int      point_count = 512;     // target output points
    // Named params for generators that declare more than 3 (see GeneratorParamDef).
    // The keys are the GeneratorParamDef::name strings; param_a/b/c live above.
    std::map<std::string, float> extra_params;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Show engine → render bus handoff
// ─────────────────────────────────────────────────────────────────────────────
struct RenderFrame {
    PointBuffer  points;
    uint64_t     sequence  = 0;     // monotonic frame counter
    double       timestamp = 0.0;   // seconds since app start
    uint32_t     point_rate = 30000; // target DAC pps
};

// ─────────────────────────────────────────────────────────────────────────────
//  DMX universe snapshot (512 channels)
// ─────────────────────────────────────────────────────────────────────────────
struct DmxUniverse {
    std::array<uint8_t, 512> ch{};
    uint64_t sequence = 0;

    uint8_t  operator[](int idx) const {
        return (idx >= 1 && idx <= 512) ? ch[idx-1] : 0;
    }
    uint8_t& operator[](int idx) {
        assert(idx >= 1 && idx <= 512 && "DmxUniverse channel index out of range (must be 1-512)");
        if (idx >= 1 && idx <= 512) return ch[idx-1];
        static uint8_t dummy = 0;
        dummy = 0;
        return dummy;
    }
    float norm(int idx) const { return operator[](idx) / 255.f; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Audio analysis snapshot
// ─────────────────────────────────────────────────────────────────────────────
struct AudioSnapshot {
    float             rms       = 0.f;
    float             peak      = 0.f;
    float             bpm       = 120.f;
    bool              beat_now  = false;
    float             sub_band  = 0.f;   // 20–200 Hz envelope
    float             mid_band  = 0.f;   // 200–2000 Hz
    float             high_band = 0.f;   // 2000–20000 Hz
    static constexpr int kFFTBins = 512;
    std::array<float, kFFTBins> fft{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  Output patch — describes one physical output stream
// ─────────────────────────────────────────────────────────────────────────────
enum class OutputStreamType : int {
    Laser = 0,   // DAC / ILDA hardware
    NDI   = 1,   // NDI video stream
    HDMI  = 2,   // borderless SDL window on a display
};

struct OutputSafetyConfig {
    bool  enabled = false;
    // Border crops (0..0.5 from each edge, normalised)
    float border_left   = 0.f;
    float border_right  = 0.f;
    float border_top    = 0.f;
    float border_bottom = 0.f;
    float border_tilt   = 0.f;  // degrees -45..45
    // Per-output block zones (same semantics as global zones)
    struct BlockZone {
        bool        enabled   = true;
        std::string name      = "Zone";
        float       cx        = 0.f, cy = 0.f;  // centre, normalised -1..1
        float       hw        = 0.1f, hh = 0.1f; // half-extents
        float       angle_deg = 0.f;
    };
    std::vector<BlockZone> zones;
};

struct OutputTransform {
    float offset_x  =  0.f;
    float offset_y  =  0.f;
    float scale_x   =  1.f;
    float scale_y   =  1.f;
    float rotation  =  0.f;  // degrees
    bool  flip_x    = false;
    bool  flip_y    = false;
};

struct OutputStreamConfig {
    int                 id      = 0;
    std::string         name    = "Output";
    OutputStreamType    type    = OutputStreamType::Laser;
    bool                enabled = true;

    // ── Laser-specific ─────────────────────────────────────────────────────
    // "auto" scans for the best available DAC; otherwise matches type string
    // and optional address (IP for EtherDream/IDN).
    std::string dac_type    = "auto";
    std::string dac_address = "";
    int         point_rate  = 30000;  // kDefaultPointRate — literal avoids forward-ref

    // ── NDI-specific ────────────────────────────────────────────────────────
    std::string ndi_name        = "IDHMFIS";
    int         ndi_width       = 1920;
    int         ndi_height      = 1080;
    int         ndi_fps_N       = 60;
    int         ndi_fps_D       = 1;
    bool        ndi_clock_video = true;

    // ── Per-output transform ────────────────────────────────────────────────
    OutputTransform transform;

    // ── Per-output safety zones ─────────────────────────────────────────────
    OutputSafetyConfig safety;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Project / app-wide constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int   kDefaultPointRate  = 30000;
static constexpr int   kMinPointRate      =  8000;
static constexpr int   kMaxPointRate      = 60000;
static constexpr int   kDefaultNDIWidth   = 1920;
static constexpr int   kDefaultNDIHeight  = 1080;
static constexpr int   kDefaultNDIFPS     = 60;
static constexpr int   kMaxUniverses      = 128;
static constexpr int   kDefaultUniverse   = 0;
static constexpr int   kArtNetPort        = 6454;
static constexpr int   kUndoBufferDepth   = 200;

} // namespace idhmfis
