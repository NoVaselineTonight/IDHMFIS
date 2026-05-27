// gen_waves.cpp — Sine wave / compound wave across the output plane.
//
// param_a: frequency (0..1 → 1..8 cycles)
// param_b: amplitude (0..1 → 0..1 normalised)
// param_c: phase offset (0..1 → 0..2π)
//
// Color cycles continuously from color_a to color_b.

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenWaves final : public IGenerator {
public:
    const char* name()         const override { return "waves"; }
    const char* display_name() const override { return "Wave"; }
    const char* param_a_label()const override { return "Frequency"; }
    const char* param_b_label()const override { return "Amplitude"; }
    const char* param_c_label()const override { return "Phase Offset"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        float freq  = 1.f + p.param_a * 7.f;    // 1..8 Hz spatial freq
        float amp   = p.param_b;                  // 0..1 vertical amplitude
        float phase = p.param_c * 2.f * 3.14159265f;
        float anim  = static_cast<float>(t) * p.speed * 2.f * 3.14159265f;
        float scale = p.scale;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        int n = std::max(2, p.point_count);

        for (int i = 0; i < n; ++i)
        {
            // X: -1 to +1 (horizontal sweep)
            float x = (static_cast<float>(i) / (n - 1)) * 2.f - 1.f;
            x *= scale;

            // Y: sine with animation
            float arg = freq * x * 3.14159265f + anim + phase;
            float y   = amp * scale * std::sin(arg);

            // Color lerp along the wave
            float frac = (x + 1.f) * 0.5f;
            Color4 col = ca.lerp(cb, frac);

            pts.push_back(LaserPoint::from_norm(x, y, col.r8(), col.g8(), col.b8()));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_waves()
{
    return std::make_unique<GenWaves>();
}

} // namespace idhmfis
