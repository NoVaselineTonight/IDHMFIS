// gen_lissajous.cpp — Classic Lissajous figure.
//
// Parametric: x = A * sin(a * τ + δ),  y = B * sin(b * τ)
// τ runs from 0 to 2π over the full point set.
//
// param_a: frequency ratio a  (0..1 → 1..8)
// param_b: frequency ratio b  (0..1 → 1..8)
// param_c: phase δ           (0..1 → 0..2π)
//
// The figure is animated by advancing a slow time offset on the phase.

#include "igenerator.h"
#include <cmath>

namespace idhmfis {

class GenLissajous final : public IGenerator {
public:
    const char* name()         const override { return "lissajous"; }
    const char* display_name() const override { return "Lissajous"; }
    const char* param_a_label()const override { return "Freq A"; }
    const char* param_b_label()const override { return "Freq B"; }
    const char* param_c_label()const override { return "Phase"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        float a     = 1.f + p.param_a * 7.f;  // 1..8
        float b     = 1.f + p.param_b * 7.f;  // 1..8
        float delta = p.param_c * 2.f * 3.14159265f
                    + static_cast<float>(t) * p.speed * 0.5f;
        float scale = p.scale;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;
        int    n  = p.point_count;

        // Full trace over 0..2π — complete the Lissajous period properly.
        // The total period of a Lissajous is LCM(a,b) * 2π / gcd,
        // but for a continuous visual we always trace 0..2π.
        static constexpr float kTwoPi = 2.f * 3.14159265f;

        for (int i = 0; i < n; ++i)
        {
            float tau = static_cast<float>(i) / n * kTwoPi;
            float x   = scale * std::sin(a * tau + delta);
            float y   = scale * std::sin(b * tau);

            float frac = static_cast<float>(i) / (n - 1);
            Color4 col = ca.lerp(cb, frac);

            pts.push_back(LaserPoint::from_norm(x, y, col.r8(), col.g8(), col.b8()));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_lissajous()
{
    return std::make_unique<GenLissajous>();
}

} // namespace idhmfis
