// gen_starburst.cpp — Starburst: N rays from center with per-ray oscillation.
//
// param_a: ray count   (0..1 -> 4..32)
// param_b: length mod  (0..1 -> 0=static, 1=full oscillation)
// param_c: rotation speed

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

namespace {

static constexpr float k2Pi = 6.28318530717958647692f;

class GenStarburst final : public IGenerator {
public:
    const char* name()         const override { return "starburst"; }
    const char* display_name() const override { return "Starburst"; }
    const char* param_a_label()const override { return "Ray Count"; }
    const char* param_b_label()const override { return "Oscillation"; }
    const char* param_c_label()const override { return "Rotation Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;

        int ray_count = 4 + static_cast<int>(p.param_a * 28.f); // 4..32
        float osc_amp  = p.param_b;
        float rot_spd  = p.param_c * 4.f;
        float anim     = static_cast<float>(t) * p.speed;
        float scale    = p.scale;

        int pts_per_ray = std::max(2, p.point_count / ray_count);

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        pts.reserve(ray_count * (pts_per_ray + 1));

        for (int i = 0; i < ray_count; ++i)
        {
            float base_angle = k2Pi * i / ray_count + anim * rot_spd;

            float osc_phase = k2Pi * i / ray_count + anim * 3.f;
            float ray_len   = scale * (1.f - osc_amp * 0.5f
                              + osc_amp * 0.5f * std::sin(osc_phase));
            ray_len = std::max(0.01f, ray_len);

            float frac = static_cast<float>(i) / ray_count;
            Color4 col = ca.lerp(cb, frac);

            pts.push_back(LaserPoint::from_norm(0.f, 0.f,
                col.r8(), col.g8(), col.b8(), true));

            for (int j = 0; j < pts_per_ray; ++j)
            {
                float u   = static_cast<float>(j) / (pts_per_ray - 1);
                float r   = u * ray_len;
                float x   = r * std::cos(base_angle);
                float y   = r * std::sin(base_angle);
                Color4 pc = col.lerp(p.color_b, u);
                pts.push_back(LaserPoint::from_norm(x, y,
                    pc.r8(), pc.g8(), pc.b8(), false));
            }
        }

        return pts;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_starburst()
{
    return std::make_unique<GenStarburst>();
}

} // namespace idhmfis
