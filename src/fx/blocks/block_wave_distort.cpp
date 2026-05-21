#include "block_wave_distort.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockWaveDistort::BlockWaveDistort() {
    p_axis_      = { "axis",      0.f,  0.f, 0.f,  2.f,  1.f,   "" };
    p_amplitude_ = { "amplitude", 0.1f, 0.f, 0.f,  0.5f, 0.01f, "" };
    p_frequency_ = { "frequency", 3.f,  0.f, 1.f, 10.f,  0.1f,  "" };
    p_speed_     = { "speed",     1.f,  0.f, 0.f, 10.f,  0.1f,  "Hz" };
    p_phase_     = { "phase",     0.f,  0.f, 0.f, 360.f, 1.f,   "deg" };
}

std::vector<FxParam*> BlockWaveDistort::params() {
    return { &p_axis_, &p_amplitude_, &p_frequency_, &p_speed_, &p_phase_ };
}

const std::vector<FxParam*> BlockWaveDistort::params() const {
    return { const_cast<FxParam*>(&p_axis_),
             const_cast<FxParam*>(&p_amplitude_),
             const_cast<FxParam*>(&p_frequency_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_phase_) };
}

void BlockWaveDistort::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    anim_t_ += dt;

    const int   axis  = static_cast<int>(std::round(p_axis_.effective()));
    const float amp   = p_amplitude_.effective();
    const float freq  = p_frequency_.effective();
    const float speed = p_speed_.effective();
    const float phase = p_phase_.effective() * static_cast<float>(M_PI) / 180.f;
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;
    const float twopi  = static_cast<float>(2.0 * M_PI);

    float t_phase = twopi * speed * anim_t_ + phase;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        if (axis == 0 || axis == 2) {
            // Displace X based on Y coordinate
            nx += amp * std::sin(ny * freq * twopi + t_phase);
        }
        if (axis == 1 || axis == 2) {
            // Displace Y based on X coordinate
            ny += amp * std::sin(nx * freq * twopi + t_phase);
        }

        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
