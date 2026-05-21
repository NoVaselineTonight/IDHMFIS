// gen_vortex.cpp — Vortex / hypnotic tunnel generator.
//
// Concentric rings arranged along a receding perspective path, each ring
// rotated by an increasing twist angle so the whole shape spirals inward
// like a vortex.  The rings zoom forward (grow) then snap back to the back
// of the tunnel each cycle (sawtooth), giving the classic "flying into a
// tunnel" illusion at high speed values.
//
// param_a : ring_count   (0..1 → 3..24 rings)
// param_b : ring_twist   (0..1 → 0..4π  twist increment per ring; makes it spiral)
// param_c : ring_sides   (0..1 → 3..16  polygon sides; 0.99=circle)
//
// speed   : flythrough speed (zoom rate Hz)
// scale   : outer ring radius (0..1 normalised, 1=full field)
// density : radial distortion — pulls ring edges toward or away from axis
// rotation: static twist angle offset (radians)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {
namespace {

static constexpr float kTwoPi = 6.28318530717958647692f;

class GenVortex final : public IGenerator {
public:
    const char* name()          const override { return "vortex"; }
    const char* display_name()  const override { return "Vortex"; }
    const char* param_a_label() const override { return "Ring Count"; }
    const char* param_b_label() const override { return "Twist"; }
    const char* param_c_label() const override { return "Ring Sides"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer out;

        int ring_count = 3 + static_cast<int>(p.param_a * 21.f);  // 3..24
        float twist    = p.param_b * kTwoPi * 2.f;                // 0..4π
        int sides      = 3 + static_cast<int>(p.param_c * 13.f);  // 3..16

        // Phase: sawtooth zoom
        float phase = static_cast<float>(std::fmod(t * static_cast<double>(p.speed), 1.0));

        int pts_per_ring = std::max(sides, p.point_count / ring_count);

        for (int r = 0; r < ring_count; ++r)
        {
            // t in [0,1] for this ring's position in the tunnel
            float ring_t = static_cast<float>(r) / static_cast<float>(ring_count - 1);

            // Perspective: rings shrink as they recede
            // Phase-offset so tunnel zooms forward
            float depth = std::fmod(ring_t + phase, 1.f);
            // Radius shrinks with depth (perspective)
            float radius = p.scale * (1.f - depth * 0.9f);
            // Fade colour: near = color_b, far = color_a
            Color4 col = p.color_b.lerp(p.color_a, depth);

            // Rotation angle for this ring
            float ring_angle = p.rotation + static_cast<float>(r) * twist + phase * kTwoPi;

            // Centre offset (can add density-based wobble)
            float wobble_r = p.density * 0.12f * std::sin(ring_t * kTwoPi * 3.f + static_cast<float>(t) * 2.f);
            float cx = p.pan  + wobble_r * std::cos(ring_angle);
            float cy = p.tilt + wobble_r * std::sin(ring_angle);

            // Blank travel to first vertex
            float x0 = cx + radius * std::cos(ring_angle);
            float y0 = cy + radius * std::sin(ring_angle);
            out.push_back(LaserPoint::from_norm(x0, y0,
                col.r8(), col.g8(), col.b8(), true));

            // Draw polygon / circle ring
            for (int s = 0; s <= pts_per_ring; ++s)
            {
                float a = ring_angle + kTwoPi * static_cast<float>(s) / static_cast<float>(sides);
                float x = cx + radius * std::cos(a);
                float y = cy + radius * std::sin(a);
                out.push_back(LaserPoint::from_norm(x, y,
                    col.r8(), col.g8(), col.b8(), false));
            }
        }

        return out;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_vortex()
{
    return std::make_unique<GenVortex>();
}

} // namespace idhmfis
