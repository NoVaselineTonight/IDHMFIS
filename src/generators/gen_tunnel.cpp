// gen_tunnel.cpp — Zoom tunnel of concentric rings receding to a vanishing point.
//
// param_a: ring count  (0..1 → 3..24)
// param_b: rotation per ring (0..1 → 0..2π)
// param_c: zoom speed  (extra animation rate)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenTunnel final : public IGenerator {
public:
    const char* name()         const override { return "tunnel"; }
    const char* display_name() const override { return "Tunnel"; }
    const char* param_a_label()const override { return "Ring Count"; }
    const char* param_b_label()const override { return "Ring Rotation"; }
    const char* param_c_label()const override { return "Zoom Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        int ring_count = std::max(3, static_cast<int>(p.param_a * 21.f) + 3); // 3..24
        float rot_per_ring = p.param_b * 2.f * 3.14159265f;
        float zoom_spd     = p.param_c * 2.f + 0.1f;
        float anim         = static_cast<float>(t) * p.speed * zoom_spd;
        float scale        = p.scale;

        int pts_per_ring = std::max(8, p.point_count / ring_count);

        static constexpr float kTwoPi = 2.f * 3.14159265f;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        for (int r = 0; r < ring_count; ++r)
        {
            // Rings zoom toward viewer: use exponential spacing.
            // Phase the zoom so rings scroll continuously.
            float frac = static_cast<float>(r) / ring_count;

            // Each ring's radius uses a logarithmic zoom offset animated by t.
            float ring_phase = frac + std::fmod(anim, 1.f);
            float ring_r = scale * std::exp(-ring_phase * 2.5f) * 0.95f;

            if (ring_r < 0.005f) continue;  // too small — skip

            float base_angle = rot_per_ring * r + anim * 0.3f;

            Color4 col = ca.lerp(cb, frac);

            // Blanked travel to ring start
            float sx = ring_r * std::cos(base_angle);
            float sy = ring_r * std::sin(base_angle);
            pts.push_back(LaserPoint::from_norm(sx, sy,
                col.r8(), col.g8(), col.b8(), true));

            for (int i = 0; i <= pts_per_ring; ++i)
            {
                float theta = base_angle + kTwoPi * static_cast<float>(i) / pts_per_ring;
                float x = ring_r * std::cos(theta);
                float y = ring_r * std::sin(theta);
                bool close = (i == pts_per_ring);  // closing segment = repeat first pt
                pts.push_back(LaserPoint::from_norm(x, y, col.r8(), col.g8(), col.b8(), false));
                (void)close;
            }
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_tunnel()
{
    return std::make_unique<GenTunnel>();
}

} // namespace idhmfis
