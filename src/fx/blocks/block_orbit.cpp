#include "block_orbit.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockOrbit::BlockOrbit() {
    p_radius_        = { "radius",        0.3f, 0.f, 0.f,   1.f, 0.01f, "" };
    p_speed_         = { "speed",         1.f,  0.f, -10.f, 10.f, 0.1f, "Hz" };
    p_phase_         = { "phase",         0.f,  0.f,  0.f, 360.f, 1.f,  "deg" };
    p_ellipse_ratio_ = { "ellipse_ratio", 1.f,  0.f,  0.f,   1.f, 0.01f, "" };
}

std::vector<FxParam*> BlockOrbit::params() {
    return { &p_radius_, &p_speed_, &p_phase_, &p_ellipse_ratio_ };
}

const std::vector<FxParam*> BlockOrbit::params() const {
    return { const_cast<FxParam*>(&p_radius_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_phase_),
             const_cast<FxParam*>(&p_ellipse_ratio_) };
}

void BlockOrbit::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed = p_speed_.effective();
    anim_phase_ += speed * static_cast<float>(2.0 * M_PI) * dt;
    // Wrap to [-2pi, 2pi] to preserve float precision over long shows
    anim_phase_ = std::fmod(anim_phase_, static_cast<float>(2.0 * M_PI));

    const float radius = p_radius_.effective();
    const float phase0 = p_phase_.effective() * static_cast<float>(M_PI) / 180.f;
    const float ratio  = p_ellipse_ratio_.effective();
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    float total = anim_phase_ + phase0;
    float ox = radius * std::cos(total);
    float oy = radius * ratio * std::sin(total);

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv + ox;
        float ny = static_cast<float>(pt.y) * kInv + oy;
        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
