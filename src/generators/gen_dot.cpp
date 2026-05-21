// gen_dot.cpp — Single dwell point (bright spot for DAC dwell).
//
// Emits the same XY position multiple times so the DAC beam dwells on it,
// producing a bright spot. Essential for punctuation in laser shows.
//
// param_a : cx   (0..1 → -1..1, default 0.5 → 0.0)
// param_b : cy   (0..1 → -1..1, default 0.5 → 0.0)
// param_c : dwell count (0..1 → 1..64 repetitions)
// scale   : spot size (jitter radius, adds a tiny visible circle for NDI preview)
// color_a : beam colour

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenDot final : public IGenerator {
public:
    const char* name()          const override { return "dot"; }
    const char* display_name()  const override { return "Dot"; }
    const char* param_a_label() const override { return "X Position"; }
    const char* param_b_label() const override { return "Y Position"; }
    const char* param_c_label() const override { return "Dwell"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        // Dwell: 1..64 repetitions
        int dwell = std::max(1, static_cast<int>(p.param_c * 63.f + 1.5f));
        dwell = std::min(dwell, 64);

        PointBuffer pts;
        pts.reserve(dwell);

        // Position: param 0..1 → -1..1
        float cx = (p.param_a * 2.f - 1.f);
        float cy = (p.param_b * 2.f - 1.f);

        // Apply pan/tilt offset
        cx += p.pan;
        cy += p.tilt;

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        // Scale adds a tiny jitter circle for NDI preview visibility; at DAC it's still dwell
        float jitter = p.scale * 0.01f;

        (void)t; // dot is static

        for (int i = 0; i < dwell; ++i) {
            // For NDI preview add a tiny circle; for DAC this doesn't matter since
            // all points land within < 1 ILDA step if scale ≈ 0
            float angle = static_cast<float>(i) / dwell * 2.f * 3.14159265f;
            float x = cx + jitter * std::cos(angle);
            float y = cy + jitter * std::sin(angle);
            // Blank only on first point (travel to position)
            bool blank = (i == 0);
            pts.push_back(LaserPoint::from_norm(x, y, r, g, b, blank));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_dot()
{
    return std::make_unique<GenDot>();
}

} // namespace idhmfis
