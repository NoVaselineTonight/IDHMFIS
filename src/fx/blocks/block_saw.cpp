#include "block_saw.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockSaw::BlockSaw() {
    p_axis_      = { "axis",      0.f,  0.f,  0.f,  1.f,  1.f,   ""    };
    p_speed_     = { "speed",     1.f,  0.f,  0.1f, 10.f, 0.1f,  "Hz"  };
    p_amplitude_ = { "amplitude", 0.5f, 0.f,  0.f,  1.f,  0.01f, ""    };
    p_offset_    = { "offset",    0.f,  0.f, -1.f,  1.f,  0.01f, ""    };
    p_phase_     = { "phase",     0.f,  0.f,  0.f, 360.f, 1.f,   "deg" };
}

std::vector<FxParam*> BlockSaw::params() {
    return { &p_axis_, &p_speed_, &p_amplitude_, &p_offset_, &p_phase_ };
}

const std::vector<FxParam*> BlockSaw::params() const {
    return { const_cast<FxParam*>(&p_axis_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_amplitude_),
             const_cast<FxParam*>(&p_offset_),
             const_cast<FxParam*>(&p_phase_) };
}

void BlockSaw::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed = p_speed_.effective();
    anim_phase_ += speed * static_cast<float>(2.0 * M_PI) * dt;

    const int   axis      = static_cast<int>(p_axis_.effective() + 0.5f);
    const float amplitude = p_amplitude_.effective();
    const float offset    = p_offset_.effective();
    const float phase0    = p_phase_.effective() * static_cast<float>(M_PI) / 180.f;

    // Normalise phase to 0..1 sawtooth: t=0 -> +amplitude, t=1 -> -amplitude
    const float raw_phase = anim_phase_ + phase0;
    const float t = std::fmod(raw_phase / static_cast<float>(2.0 * M_PI), 1.f);
    // Ensure t is in [0, 1) even for negative raw_phase
    const float t_norm = t < 0.f ? t + 1.f : t;

    // Sawtooth value: ramps from +amplitude down to -amplitude
    const float saw = amplitude * (1.f - 2.f * t_norm);

    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        if (axis == 0) {
            // Pan / X axis
            float nx = static_cast<float>(pt.x) * kInv + saw + offset;
            pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        } else {
            // Tilt / Y axis
            float ny = static_cast<float>(pt.y) * kInv + saw + offset;
            pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
        }
    }
}

} // namespace idhmfis
