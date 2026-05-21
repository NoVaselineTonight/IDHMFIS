// gen_sine_wave.cpp — Sine wave as a lit line across the frame.
//
// param_a : frequency  (0..1 → 1..16 cycles across width)
// param_b : amplitude  (0..1 → 0.05..1.0)
// param_c : phase_speed fraction (0..1 → 0..4 Hz phase animation rate)
// speed   : overall animation speed multiplier
// pan     : cx (horizontal centre offset)
// tilt    : cy (vertical centre offset)
// scale   : width of the wave (0..1 → 0.1..2.0 half-width × 2)
// color_a / color_b: gradient from left to right
// point_count: number of samples

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenSineWave final : public IGenerator {
public:
    const char* name()          const override { return "sine_wave"; }
    const char* display_name()  const override { return "Sine Wave"; }
    const char* param_a_label() const override { return "Frequency"; }
    const char* param_b_label() const override { return "Amplitude"; }
    const char* param_c_label() const override { return "Phase Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        int n = std::max(2, p.point_count);
        pts.reserve(n);

        // Frequency: 1..16 cycles
        float freq = 1.f + p.param_a * 15.f;
        // Amplitude: 0.05..1.0
        float amp  = 0.05f + p.param_b * 0.95f;
        // Phase speed: 0..4 Hz
        float phase_speed = p.param_c * 4.f;
        float phase = static_cast<float>(t) * phase_speed * p.speed * 2.f * 3.14159265f;

        // Width: scale 0..1 → half_width 0.05..1.0, so total span = 0.1..2.0
        float half_width = std::max(0.05f, p.scale);

        float cx = p.pan;
        float cy = p.tilt;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        static constexpr float kPi = 3.14159265f;

        for (int i = 0; i < n; ++i) {
            float frac = (n > 1) ? static_cast<float>(i) / (n - 1) : 0.5f;
            // x_norm: -1..1 across the width
            float x_norm = frac * 2.f - 1.f;
            float x = cx + x_norm * half_width;
            float y = cy + amp * std::sin(freq * kPi * x_norm + phase);

            Color4 col = ca.lerp(cb, frac);
            bool blank = (i == 0);
            pts.push_back(LaserPoint::from_norm(x, y, col.r8(), col.g8(), col.b8(), blank));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_sine_wave()
{
    return std::make_unique<GenSineWave>();
}

} // namespace idhmfis
