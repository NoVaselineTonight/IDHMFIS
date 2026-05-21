// gen_burst.cpp — Burst / explosion radiating lines from centre.
//
// N lines radiate from (0,0) outward to radius r.  r grows linearly from 0
// to max_radius then snaps back (sawtooth), or can pulse sinusoidally.
// The flash_on_beat mode (param_c > 0.5) makes each burst cycle very short
// (sharp flash rather than slow bloom), intended to be modulated by beat_now
// from the modulation matrix — at default it simply runs at a fast rate.
//
// param_a : line_count    (0..1 -> 4..64)
// param_b : expand_mode   (0=sawtooth, 1=triangle/breathe)
// param_c : flash_mode    (0=smooth expand, 1=sharp flash — narrow pulse)
//
// speed   : expand rate (Hz; controls how many burst cycles per second)
// scale   : max_radius (0..1 in normalised space; 1 = full field diagonal)
// color_a : burst colour at origin (inner)
// color_b : burst colour at tip (outer)
// rotation: angular offset of the whole burst (radians)
// density : angular randomisation — adds stochastic jitter to line angles
//           (0=perfectly even, 1=heavy scatter)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

namespace {

static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr float kPi    = 3.14159265358979323846f;

// Minimal seeded hash for deterministic "random" jitter — no rand(), no state.
static float pseudo_rand(int seed)
{
    unsigned u = static_cast<unsigned>(seed) * 2654435761u;
    u ^= (u >> 16);
    u *= 2246822519u;
    u ^= (u >> 13);
    return static_cast<float>(u & 0xFFFFu) / 65535.f;
}

class GenBurst final : public IGenerator {
public:
    const char* name()         const override { return "burst"; }
    const char* display_name() const override { return "Burst"; }
    const char* param_a_label()const override { return "Line Count"; }
    const char* param_b_label()const override { return "Expand Mode"; }
    const char* param_c_label()const override { return "Flash Mode"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;

        int line_count = 4 + static_cast<int>(p.param_a * 60.f);  // 4..64
        int pts_per_line = std::max(2, p.point_count / line_count);
        pts.reserve(static_cast<size_t>(line_count * (pts_per_line + 1)));

        float max_radius  = p.scale;
        float anim        = static_cast<float>(t) * p.speed;
        float jitter_amp  = p.density * 0.4f;  // up to ±0.4/N radians jitter

        // Expand phase: sawtooth or triangle
        bool triangle_mode = (p.param_b >= 0.5f);
        // Flash mode: use a narrow cosine window so radius spends most time near 0
        bool flash_mode = (p.param_c >= 0.5f);

        float phase = std::fmod(anim, 1.f);

        float radius_norm;
        if (flash_mode) {
            // Narrow gaussian-ish pulse: cos^8 window, peaks at phase=0.5
            float offset = std::fabs(phase - 0.5f) * 2.f;  // 0..1
            float cos_w  = std::cos(offset * kPi * 0.5f);
            radius_norm  = std::pow(cos_w, 8.f);
        } else if (triangle_mode) {
            // Triangle: grows and shrinks symmetrically
            radius_norm = (phase < 0.5f) ? phase * 2.f : 2.f - phase * 2.f;
        } else {
            // Sawtooth: fast snap, slow grow
            radius_norm = phase;
        }

        float current_radius = max_radius * radius_norm;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        for (int i = 0; i < line_count; ++i)
        {
            // Base angle: evenly distributed + jitter
            float base_angle = p.rotation
                + kTwoPi * static_cast<float>(i) / static_cast<float>(line_count)
                + jitter_amp * (pseudo_rand(i) * 2.f - 1.f);

            // Blank travel to origin
            pts.push_back(LaserPoint::from_norm(p.pan, p.tilt,
                ca.r8(), ca.g8(), ca.b8(), true));

            for (int j = 0; j < pts_per_line; ++j)
            {
                float u = static_cast<float>(j) / static_cast<float>(pts_per_line - 1);
                float r = u * current_radius;
                float x = p.pan  + r * std::cos(base_angle);
                float y = p.tilt + r * std::sin(base_angle);

                Color4 col = ca.lerp(cb, u);
                pts.push_back(LaserPoint::from_norm(x, y,
                    col.r8(), col.g8(), col.b8(), false));
            }
        }

        return pts;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_burst()
{
    return std::make_unique<GenBurst>();
}

} // namespace idhmfis
