// gen_spiral.cpp — Archimedean spiral (radius grows linearly with angle).
//
// param_a : turns       (0..1 → 0.5..16)
// param_b : start_radius (0..1 → 0..0.5)
// param_c : end_radius  (0..1 → 0.1..1.0)
// speed   : rotation_speed Hz (whole spiral rotates)
// density : expand_speed — when > 0 the spiral expands/contracts (0..1 → 0..2 Hz)
// pan/tilt: centre x/y
// color_a / color_b: colour gradient from centre to outer edge
// scale   : overall scale

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenSpiral final : public IGenerator {
public:
    const char* name()          const override { return "spiral"; }
    const char* display_name()  const override { return "Spiral"; }
    const char* param_a_label() const override { return "Turns"; }
    const char* param_b_label() const override { return "Start Radius"; }
    const char* param_c_label() const override { return "End Radius"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        int n = std::max(2, p.point_count);
        pts.reserve(n);

        float turns      = 0.5f + p.param_a * 15.5f;      // 0.5..16
        float r_start    = p.param_b * 0.5f * p.scale;     // 0..0.5
        float r_end      = (0.1f + p.param_c * 0.9f) * p.scale; // 0.1..1.0
        float rot_offset = static_cast<float>(t) * p.speed * 2.f * 3.14159265f;

        // Expand/contract animation via density
        float expand_phase = 0.f;
        if (p.density > 0.001f) {
            float expand_speed = p.density * 2.f;  // 0..2 Hz
            expand_phase = static_cast<float>(t) * expand_speed * 2.f * 3.14159265f;
            // Animate r_end slightly (breathe effect)
            float breathe = std::sin(expand_phase) * 0.1f * p.scale;
            r_end = std::max(0.05f, r_end + breathe);
        }

        float cx = p.pan;
        float cy = p.tilt;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        static constexpr float kTwoPi = 2.f * 3.14159265f;
        float total_angle = turns * kTwoPi;

        for (int i = 0; i < n; ++i) {
            float frac  = static_cast<float>(i) / (n - 1);
            float angle = rot_offset + frac * total_angle;
            float rad   = r_start + (r_end - r_start) * frac;

            float x = cx + rad * std::cos(angle);
            float y = cy + rad * std::sin(angle);

            Color4 col = ca.lerp(cb, frac);
            bool blank = (i == 0);
            pts.push_back(LaserPoint::from_norm(x, y, col.r8(), col.g8(), col.b8(), blank));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_spiral()
{
    return std::make_unique<GenSpiral>();
}

} // namespace idhmfis
