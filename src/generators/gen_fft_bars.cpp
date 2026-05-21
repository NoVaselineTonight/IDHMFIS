// gen_fft_bars.cpp — FFT frequency bar display.
//
// param_a: bar count (0..1 → 16..64)
// param_b: gain     (0..1 → 0.5..8x)
// param_c: mirror   (0..0.5 = single half, 0.5..1 = mirrored)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

// Audio bridge (defined in gen_oscilloscope.cpp)
extern AudioSnapshot g_last_audio_snap;

class GenFFTBars final : public IGenerator {
public:
    const char* name()         const override { return "fft_bars"; }
    const char* display_name() const override { return "FFT Bars"; }
    const char* param_a_label()const override { return "Bar Count"; }
    const char* param_b_label()const override { return "Gain"; }
    const char* param_c_label()const override { return "Mirror"; }

    PointBuffer generate(const GeneratorParams& p, double /*t*/) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        const AudioSnapshot& audio = g_last_audio_snap;

        int bar_count = 16 + static_cast<int>(p.param_a * 48.f); // 16..64
        bar_count = std::clamp(bar_count, 1, 64);

        float gain    = 0.5f + p.param_b * 7.5f;
        bool  mirror  = (p.param_c > 0.5f);
        float scale   = p.scale;

        // Points per bar (at least 3: travel, bottom, top)
        int pts_per_bar = std::max(3, p.point_count / (mirror ? bar_count * 2 : bar_count));

        static constexpr int kBins = AudioSnapshot::kFFTBins;

        Color4 ca = p.color_a; // bass color
        Color4 cb = p.color_b; // treble color

        auto draw_bar = [&](float bx, float width_half, int bar_idx)
        {
            float frac = static_cast<float>(bar_idx) / (bar_count - 1);

            // Use log-spaced bin mapping for perceptual frequency spacing
            float bin_f = std::pow(static_cast<float>(kBins), frac);
            int   bin   = std::clamp(static_cast<int>(bin_f), 0, kBins - 1);

            float bar_h = std::clamp(audio.fft[bin] * gain, 0.f, 1.f) * scale;

            float base_y = -scale;
            float top_y  = base_y + bar_h * 2.f;

            Color4 col = ca.lerp(cb, frac);

            // Blanked travel to bar bottom-left
            pts.push_back(LaserPoint::from_norm(
                bx - width_half, base_y, col.r8(), col.g8(), col.b8(), true));

            // Draw bar outline: bottom-left → top-left → top-right → bottom-right
            // (4 corners + closing = 5 pts; we use pts_per_bar for the left edge)
            for (int j = 0; j <= pts_per_bar; ++j)
            {
                float fy = base_y + (top_y - base_y) *
                           static_cast<float>(j) / pts_per_bar;
                pts.push_back(LaserPoint::from_norm(
                    bx - width_half, fy, col.r8(), col.g8(), col.b8(), false));
            }
            // Top right
            pts.push_back(LaserPoint::from_norm(
                bx + width_half, top_y, col.r8(), col.g8(), col.b8(), false));
        };

        float bar_width = (2.f * scale) / bar_count;
        float half_w    = bar_width * 0.45f;

        if (!mirror)
        {
            for (int b = 0; b < bar_count; ++b)
            {
                float bx = -scale + (b + 0.5f) * bar_width;
                draw_bar(bx, half_w, b);
            }
        }
        else
        {
            // Left half: low freq → center; Right half: low → edge
            for (int b = 0; b < bar_count; ++b)
            {
                float bx_l = -(b + 0.5f) * bar_width / 2.f;
                float bx_r =  (b + 0.5f) * bar_width / 2.f;
                draw_bar(bx_l * scale, half_w / 2.f, b);
                draw_bar(bx_r * scale, half_w / 2.f, b);
            }
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_fft_bars()
{
    return std::make_unique<GenFFTBars>();
}

} // namespace idhmfis
