// gen_waterfall.cpp — Laser waterfall / cascade.
//
// Multiple vertical streams of light cascade downward from the top of the
// field.  Each stream is a short lit segment at a fixed X position, with a
// Y offset that increases over time (modulo field height) creating a smooth
// falling motion.  Streams are offset in phase from each other so the fall
// looks staggered and organic.
//
// param_a : stream_count  (0..1 -> 4..48 streams)
// param_b : stream_length (0..1 -> 0.04..0.6 of field height per stream)
// param_c : flicker_rate  (0=no flicker, 1=heavy flicker; implemented as
//                         per-stream brightness modulation via fast sine)
//
// speed   : fall speed (in field-heights per second; 0.2..3.0)
// scale   : overall vertical span of the waterfall (0.5..1.0 typical)
// color_a : top-of-stream colour (bright)
// color_b : bottom-of-stream colour (dim; simulates fade-out trail)
// rotation: X spread — how wide the streams are spread (0=centre cluster,
//           1=full field width)
// density : Y baseline (0=streams start at top, 1=shifted lower)
// pan/tilt: field offset

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

namespace {

// Deterministic pseudo-random for per-stream X positions and phase offsets.
static float stream_hash(int idx, int salt)
{
    unsigned u = (static_cast<unsigned>(idx) * 2246822519u)
               ^ (static_cast<unsigned>(salt) * 2654435761u);
    u ^= (u >> 15);
    u *= 1664525u;
    u ^= (u >> 13);
    return static_cast<float>(u & 0xFFFFu) / 65535.f;
}

class GenWaterfall final : public IGenerator {
public:
    const char* name()         const override { return "waterfall"; }
    const char* display_name() const override { return "Waterfall"; }
    const char* param_a_label()const override { return "Stream Count"; }
    const char* param_b_label()const override { return "Stream Length"; }
    const char* param_c_label()const override { return "Flicker"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;

        int streams = 4 + static_cast<int>(p.param_a * 44.f);  // 4..48
        float seg_len = 0.04f + p.param_b * 0.56f;             // 0.04..0.60
        float flicker = p.param_c;
        float fall_speed = p.speed;                             // field-heights/sec
        float x_spread   = p.rotation;                         // 0..1 -> x spread

        int pts_per_stream = std::max(3, p.point_count / streams);
        pts.reserve(static_cast<size_t>(streams * (pts_per_stream + 1)));

        float anim_t = static_cast<float>(t);
        float y_top  = p.scale;   // top of field (typically 1.0)

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        for (int s = 0; s < streams; ++s)
        {
            // Per-stream X position: uniformly distributed in [-spread, +spread]
            float x_norm = stream_hash(s, 0);  // 0..1
            float x_pos  = p.pan + x_spread * (x_norm * 2.f - 1.f);

            // Per-stream phase offset so streams don't all reach the bottom at once
            float phase_off = stream_hash(s, 1);

            // Fall phase: sawtooth from 0 to 1, period = 1/fall_speed seconds
            float fall_phase = std::fmod(anim_t * fall_speed + phase_off, 1.f);

            // Top of this stream in normalised y: starts at y_top, falls to -y_top
            // Field range is -1..1, span = 2 * y_top
            float y_fall_range = 2.f * y_top + seg_len;  // extra so it exits cleanly
            float stream_top   = y_top - fall_phase * y_fall_range + p.tilt;
            float stream_bot   = stream_top - seg_len;

            // Per-stream flicker: amplitude of brightness modulation
            float flicker_mod = 1.f;
            if (flicker > 0.01f) {
                float flicker_freq = 6.f + stream_hash(s, 2) * 14.f;  // 6..20 Hz
                float flicker_sin  = std::sin(anim_t * flicker_freq * 6.28318f
                                    + stream_hash(s, 3) * 6.28318f);
                flicker_mod = 1.f - flicker * 0.5f * (1.f - flicker_sin);
            }

            // Blanked travel to top of stream
            pts.push_back(LaserPoint::from_norm(x_pos, stream_top,
                ca.r8(), ca.g8(), ca.b8(), true));

            for (int j = 0; j < pts_per_stream; ++j)
            {
                float u  = static_cast<float>(j) / static_cast<float>(pts_per_stream - 1);
                float y  = stream_top - u * seg_len;

                // Colour: top = color_a (bright), bottom = color_b (dim)
                Color4 col = ca.lerp(cb, u);

                // Apply flicker brightness scaling
                uint8_t r8 = static_cast<uint8_t>(
                    std::clamp(static_cast<float>(col.r8()) * flicker_mod, 0.f, 255.f));
                uint8_t g8 = static_cast<uint8_t>(
                    std::clamp(static_cast<float>(col.g8()) * flicker_mod, 0.f, 255.f));
                uint8_t b8 = static_cast<uint8_t>(
                    std::clamp(static_cast<float>(col.b8()) * flicker_mod, 0.f, 255.f));

                pts.push_back(LaserPoint::from_norm(x_pos, y, r8, g8, b8, false));
            }

            (void)stream_bot;  // used conceptually; stream_top - seg_len
        }

        return pts;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_waterfall()
{
    return std::make_unique<GenWaterfall>();
}

} // namespace idhmfis
