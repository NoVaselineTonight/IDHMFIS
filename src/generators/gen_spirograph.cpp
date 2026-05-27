// gen_spirograph.cpp — Epitrochoid / hypotrochoid spirograph.
//
// Epitrochoid parametric:
//   x = (R + r) * cos(t) - d * cos((R+r)/r * t)
//   y = (R + r) * sin(t) - d * sin((R+r)/r * t)
//
// param_a: R/r ratio (0..1 → 1..10)
// param_b: d — distance from rolling circle center (0..1 → 0..r)
// param_c: trace speed  (extra t multiplier)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenSpirograph final : public IGenerator {
public:
    const char* name()         const override { return "spirograph"; }
    const char* display_name() const override { return "Spirograph"; }
    const char* param_a_label()const override { return "R/r Ratio"; }
    const char* param_b_label()const override { return "d (Trace)"; }
    const char* param_c_label()const override { return "Trace Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        // Ratio R/r — use a fixed r=1 and compute R
        float ratio = 1.f + p.param_a * 9.f;  // 1..10
        float R     = ratio;
        float r     = 1.f;
        float d     = p.param_b * r;           // 0..r

        // Total trace angle needed to close the curve:
        // Period = 2π * r / gcd(R,r) — for real values approximate as 2π * ratio
        static constexpr float kPi  = 3.14159265f;
        static constexpr float kTwo = 2.f;
        float period = kTwo * kPi * ratio;     // approximate closure for float ratios

        float trace_speed = p.param_c * 3.f + 0.5f;
        float anim_offset = static_cast<float>(t) * p.speed * trace_speed;
        float scale = p.scale / (R + r + 1.f); // normalise to unit circle

        int n = std::max(2, p.point_count);
        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        for (int i = 0; i < n; ++i)
        {
            float tau = (static_cast<float>(i) / n) * period + anim_offset;
            float x = ((R + r) * std::cos(tau) - d * std::cos((R + r) / r * tau)) * scale;
            float y = ((R + r) * std::sin(tau) - d * std::sin((R + r) / r * tau)) * scale;

            float frac = (n > 1) ? static_cast<float>(i) / (n - 1) : 0.f;
            Color4 col = ca.lerp(cb, frac);

            pts.push_back(LaserPoint::from_norm(x, y, col.r8(), col.g8(), col.b8(),
                i == 0));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_spirograph()
{
    return std::make_unique<GenSpirograph>();
}

} // namespace idhmfis
