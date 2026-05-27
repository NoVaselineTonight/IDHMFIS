// gen_oscilloscope.cpp — Audio-reactive oscilloscope / vectorscope.
//
// param_a: gain     (0..1 → 0.1..10x)
// param_b: mode     (0..0.33 = waveform, 0.33..0.66 = spectrum, 0.66..1 = vectorscope)
// param_c: unused
//
// In waveform mode: draws the FFT bin values as a horizontal waveform.
// In spectrum mode: draws bar-height spectrum vertically.
// In vectorscope mode: plots fft[i] vs fft[i+N/2] in X-Y.

#include "igenerator.h"
#include "../core/types.h"
#include <cmath>
#include <algorithm>
#include <mutex>

namespace idhmfis {

// Global audio snapshot bridge.
// Updated by ShowEngine::build_frame() immediately before calling generators.
// Audio-reactive generators read this without locking (acceptable for live viz).
AudioSnapshot g_last_audio_snap;
extern std::mutex g_audio_snap_mtx;

class GenOscilloscope final : public IGenerator {
public:
    const char* name()         const override { return "oscilloscope"; }
    const char* display_name() const override { return "Oscilloscope"; }
    const char* param_a_label()const override { return "Gain"; }
    const char* param_b_label()const override { return "Mode"; }
    const char* param_c_label()const override { return "Brightness"; }

    PointBuffer generate(const GeneratorParams& p, double /*t*/) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        const AudioSnapshot& audio = g_last_audio_snap;
        float gain  = 0.1f + p.param_a * 9.9f;
        float scale = p.scale;

        // Mode selection
        int mode = 0; // waveform
        if      (p.param_b > 0.66f) mode = 2; // vectorscope
        else if (p.param_b > 0.33f) mode = 1; // spectrum

        int n = std::max(2, p.point_count);
        static constexpr int kBins = AudioSnapshot::kFFTBins;

        if (mode == 0)
        {
            // Waveform: map FFT bins as pseudo-waveform across horizontal axis
            for (int i = 0; i < n; ++i)
            {
                float x = (static_cast<float>(i) / (n - 1)) * 2.f - 1.f;
                x *= scale;
                int bin = static_cast<int>(static_cast<float>(i) / n * kBins);
                bin = std::clamp(bin, 0, kBins - 1);
                float y = audio.fft[bin] * gain * scale;
                y = std::clamp(y, -1.f, 1.f);

                Color4 col = p.color_a.lerp(p.color_b,
                    static_cast<float>(i) / (n - 1));

                pts.push_back(LaserPoint::from_norm(x, y,
                    col.r8(), col.g8(), col.b8(), i == 0));
            }
        }
        else if (mode == 1)
        {
            // Spectrum bars: n bars, each is a vertical line segment
            int bar_count = std::max(4, n / 16);
            int pts_per_bar = std::max(2, n / bar_count);

            for (int b = 0; b < bar_count; ++b)
            {
                float bx = (static_cast<float>(b) / (bar_count - 1)) * 2.f - 1.f;
                bx *= scale;

                int bin = static_cast<int>(static_cast<float>(b) / bar_count * kBins);
                bin = std::clamp(bin, 0, kBins - 1);
                float bar_h = audio.fft[bin] * gain * scale;
                bar_h = std::clamp(bar_h, 0.f, 1.f);

                float base_y = -scale;
                float top_y  = base_y + bar_h * 2.f * scale;

                Color4 col = p.color_a.lerp(p.color_b,
                    static_cast<float>(b) / (bar_count - 1));

                // Blanked travel to bar base
                pts.push_back(LaserPoint::from_norm(bx, base_y,
                    col.r8(), col.g8(), col.b8(), true));

                for (int j = 0; j < pts_per_bar; ++j)
                {
                    float fy = base_y + (top_y - base_y) *
                               static_cast<float>(j) / (pts_per_bar - 1);
                    pts.push_back(LaserPoint::from_norm(bx, fy,
                        col.r8(), col.g8(), col.b8(), false));
                }
            }
        }
        else
        {
            // Vectorscope: X = left channel (first half bins), Y = right (second half)
            static constexpr int half = kBins / 2;
            for (int i = 0; i < n; ++i)
            {
                float frac = static_cast<float>(i) / n;
                int   bi   = static_cast<int>(frac * half);
                bi = std::clamp(bi, 0, half - 1);

                float x = audio.fft[bi]        * gain * scale * 2.f - scale;
                float y = audio.fft[bi + half]  * gain * scale * 2.f - scale;
                x = std::clamp(x, -1.f, 1.f);
                y = std::clamp(y, -1.f, 1.f);

                Color4 col = p.color_a.lerp(p.color_b, frac);
                pts.push_back(LaserPoint::from_norm(x, y,
                    col.r8(), col.g8(), col.b8(), i == 0));
            }
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_oscilloscope()
{
    return std::make_unique<GenOscilloscope>();
}

} // namespace idhmfis
