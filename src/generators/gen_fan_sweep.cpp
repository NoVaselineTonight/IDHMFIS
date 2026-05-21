// gen_fan_sweep.cpp -- Fan sweep: N beams sweeping back and forth in a fan arc.
//
// param_a: beam count    (0..1 -> 1..16)
// param_b: sweep angle   (0..1 -> 10..170 degrees)
// param_c: sweep speed   (0..1 -> 0.5..8 Hz)

#include "igenerator.h"
#include <cmath>
#include <algorithm>
#include <memory>

namespace idhmfis {

namespace {

static constexpr float kPi  = 3.14159265358979323846f;
static constexpr float k2Pi = 6.28318530717958647692f;

class GenFanSweep final : public IGenerator {
public:
    const char* name()         const override { return "fan_sweep"; }
    const char* display_name() const override { return "Fan Sweep"; }
    const char* param_a_label()const override { return "Beam Count"; }
    const char* param_b_label()const override { return "Sweep Angle"; }
    const char* param_c_label()const override { return "Sweep Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;

        int   beam_count  = 1 + static_cast<int>(p.param_a * 15.f); // 1..16
        float sweep_deg   = p.param_b * 160.f + 10.f;
        float sweep_rad   = sweep_deg * kPi / 180.f;
        float sweep_speed = 0.5f + p.param_c * 7.5f;
        float anim        = static_cast<float>(t) * p.speed;
        float scale       = p.scale;

        int pts_per_beam = std::max(2, p.point_count / beam_count);

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        pts.reserve(beam_count * (pts_per_beam + 1));

        // Triangle-wave sweep: -0.5..+0.5 * sweep_rad
        float phase      = std::fmod(anim * sweep_speed, 1.f);
        float sweep_frac = (phase < 0.5f) ? phase * 2.f : 2.f - phase * 2.f;
        float master_angle = (sweep_frac - 0.5f) * sweep_rad;
        float center_angle = kPi / 2.f; // straight up

        for (int b = 0; b < beam_count; ++b)
        {
            float beam_frac = (beam_count == 1) ? 0.5f :
                              static_cast<float>(b) / (beam_count - 1);

            float spread = sweep_rad * 0.25f;
            float beam_offset = (beam_frac - 0.5f) * spread;
            float angle = center_angle + master_angle + beam_offset;

            Color4 col = ca.lerp(cb, beam_frac);

            pts.push_back(LaserPoint::from_norm(0.f, 0.f,
                col.r8(), col.g8(), col.b8(), true));

            for (int j = 0; j < pts_per_beam; ++j)
            {
                float u = static_cast<float>(j) / (pts_per_beam - 1);
                float r = u * scale;
                float x = r * std::cos(angle);
                float y = r * std::sin(angle);
                Color4 pc = col.lerp(p.color_b, u);
                pts.push_back(LaserPoint::from_norm(x, y,
                    pc.r8(), pc.g8(), pc.b8(), false));
            }
        }

        return pts;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_fan_sweep()
{
    return std::make_unique<GenFanSweep>();
}

} // namespace idhmfis
