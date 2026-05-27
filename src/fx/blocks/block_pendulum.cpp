#include "block_pendulum.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockPendulum::BlockPendulum() {
    p_amplitude_x_  = { "amplitude_x",  0.4f, 0.f,  0.f,  1.f,  0.01f, "" };
    p_amplitude_y_  = { "amplitude_y",  0.4f, 0.f,  0.f,  1.f,  0.01f, "" };
    p_freq_x_       = { "freq_x",       1.f,  0.f,  0.f, 10.f,  0.1f,  "Hz" };
    p_freq_y_       = { "freq_y",       2.f,  0.f,  0.f, 10.f,  0.1f,  "Hz" };
    p_phase_offset_ = { "phase_offset", 90.f, 0.f,  0.f, 360.f, 1.f,   "deg" };
}

std::vector<FxParam*> BlockPendulum::params() {
    return { &p_amplitude_x_, &p_amplitude_y_, &p_freq_x_, &p_freq_y_, &p_phase_offset_ };
}

const std::vector<FxParam*> BlockPendulum::params() const {
    return { const_cast<FxParam*>(&p_amplitude_x_),
             const_cast<FxParam*>(&p_amplitude_y_),
             const_cast<FxParam*>(&p_freq_x_),
             const_cast<FxParam*>(&p_freq_y_),
             const_cast<FxParam*>(&p_phase_offset_) };
}

void BlockPendulum::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    anim_t_ += dt;
    if (anim_t_ > 65536.f) anim_t_ -= 65536.f;

    const float ax    = p_amplitude_x_.effective();
    const float ay    = p_amplitude_y_.effective();
    const float fx    = p_freq_x_.effective();
    const float fy    = p_freq_y_.effective();
    const float phi   = p_phase_offset_.effective() * static_cast<float>(M_PI) / 180.f;
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;
    const float twopi  = static_cast<float>(2.0 * M_PI);

    float ox = ax * std::sin(twopi * fx * anim_t_);
    float oy = ay * std::sin(twopi * fy * anim_t_ + phi);

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv + ox;
        float ny = static_cast<float>(pt.y) * kInv + oy;
        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
