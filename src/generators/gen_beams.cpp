// gen_beams.cpp — Parallel / fan beams radiating from center or in parallel.
//
// param_a: beam count (0..1 → 1..32)
// param_b: beam spacing (0..1 → tight to full-width)
// param_c: beam length (0..1 → 0=full length, else fraction)
//
// When param_a < 0.5: parallel beams (horizontal fan)
// When param_a >= 0.5: radial fan beams from center

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenBeams final : public IGenerator {
public:
    const char* name()         const override { return "beams"; }
    const char* display_name() const override { return "Beams"; }
    const char* param_a_label()const override { return "Beam Count"; }
    const char* param_b_label()const override { return "Spacing"; }
    const char* param_c_label()const override { return "Length"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        int beam_count = std::max(1, static_cast<int>(p.param_a * 32.f + 0.5f));
        if (beam_count > 32) beam_count = 32;

        float spacing  = p.param_b * 1.6f;   // 0 → 0, 1 → 1.6 (nearly full width)
        float length   = (p.param_c < 0.02f) ? 1.f : p.param_c;
        float anim     = static_cast<float>(t) * p.speed;

        // Points per beam (at least 2 per segment)
        int pts_per_beam = std::max(2, p.point_count / beam_count);

        bool fan_mode = (beam_count > 1);

        // Color gradient: color_a → color_b along beam length
        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        for (int b = 0; b < beam_count; ++b)
        {
            // Fan angle for this beam
            float beam_frac = (beam_count == 1) ? 0.5f :
                              static_cast<float>(b) / (beam_count - 1);

            // Beam center angle: fan from -spacing/2 to +spacing/2 radians
            float angle;
            if (fan_mode)
                angle = (beam_frac - 0.5f) * spacing * 3.14159265f
                      + anim * 0.2f;          // slow rotation
            else
                angle = 0.f;

            // Origin (root of beam)
            float ox = 0.f, oy = 0.f;

            // Direction vector
            float dx = std::sin(angle);
            float dy = std::cos(angle);

            // Beam color (per-beam lerp)
            Color4 col = ca.lerp(cb, beam_frac);

            // Add a blanked travel to the beam root
            LaserPoint travel;
            travel.x       = static_cast<int16_t>(ox * 32767.f);
            travel.y       = static_cast<int16_t>(oy * 32767.f);
            travel.blanked = true;
            pts.push_back(travel);

            for (int i = 0; i < pts_per_beam; ++i)
            {
                float frac  = static_cast<float>(i) / (pts_per_beam - 1);
                float bx    = ox + dx * frac * length;
                float by    = oy + dy * frac * length;

                // Per-point color lerp along the beam
                Color4 pc = col.lerp(cb, frac);

                LaserPoint lp = LaserPoint::from_norm(bx, by,
                    pc.r8(), pc.g8(), pc.b8(), false);
                pts.push_back(lp);
            }
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_beams()
{
    return std::make_unique<GenBeams>();
}

} // namespace idhmfis
