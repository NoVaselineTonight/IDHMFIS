#include "block_wave.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockWave::BlockWave() {
    p_amplitude_ = { "amplitude", 0.5f, 0.f, 0.f, 2.f,  0.01f, "" };
    p_frequency_ = { "frequency", 2.f,  0.f, 0.1f, 20.f, 0.1f, "Hz" };
    p_phase_     = { "phase",     0.f,  0.f, 0.f,  static_cast<float>(M_PI * 2.0), 0.01f, "rad" };
    p_axis_      = { "axis",      0.f,  0.f, 0.f,  2.f,  1.f,  "" };
    p_waveform_  = { "waveform",  0.f,  0.f, 0.f,  3.f,  1.f,  "" };
}

std::vector<FxParam*> BlockWave::params() {
    return { &p_amplitude_, &p_frequency_, &p_phase_, &p_axis_, &p_waveform_ };
}

const std::vector<FxParam*> BlockWave::params() const {
    return { const_cast<FxParam*>(&p_amplitude_),
             const_cast<FxParam*>(&p_frequency_),
             const_cast<FxParam*>(&p_phase_),
             const_cast<FxParam*>(&p_axis_),
             const_cast<FxParam*>(&p_waveform_) };
}

static float wave_sample(float waveform_idx, float phase) {
    int w = static_cast<int>(std::round(waveform_idx));
    float p = std::fmod(phase, static_cast<float>(M_PI * 2.0));
    if (p < 0.f) p += static_cast<float>(M_PI * 2.0);

    switch (w) {
    case 1: {
        // Sawtooth: 1 at 0, -1 at pi, 1 at 2pi (linear ramp then reset)
        float t = p / static_cast<float>(M_PI * 2.0);
        return 1.f - 2.f * t;
    }
    case 2: {
        // Triangle
        float t = p / static_cast<float>(M_PI * 2.0);
        if (t < 0.5f) return 4.f * t - 1.f;
        return 3.f - 4.f * t;
    }
    case 3: {
        // Square
        return (p < static_cast<float>(M_PI)) ? 1.f : -1.f;
    }
    default:
        return std::sin(phase);
    }
}

void BlockWave::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float amp   = p_amplitude_.effective();
    const float freq  = p_frequency_.effective();
    const float phase = p_phase_.effective();
    const float axis  = p_axis_.effective();
    const int   ax    = static_cast<int>(std::round(axis));
    const float wf    = p_waveform_.effective();

    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        // Phase along the x-axis for the wave (position + time based)
        float arg = static_cast<float>(M_PI * 2.0) * freq * nx + phase + anim_phase_;
        float displacement = amp * wave_sample(wf, arg);

        if (ax == 0 || ax == 2) {
            // Offset X
            float nx2 = std::clamp(nx + displacement, -1.f, 1.f);
            pt.x = static_cast<int16_t>(nx2 * kScale);
        }
        if (ax == 1 || ax == 2) {
            // Offset Y
            float ny2 = std::clamp(ny + displacement, -1.f, 1.f);
            pt.y = static_cast<int16_t>(ny2 * kScale);
        }
    }

    // Keep anim_phase_ ticking so callers can hook into it if needed
    anim_phase_ = std::fmod(anim_phase_ + dt * freq, static_cast<float>(M_PI * 2.0));
}

} // namespace idhmfis
