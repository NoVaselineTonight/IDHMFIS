#include "block_rotate.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockRotate::BlockRotate() {
    p_angle_    = { "angle",    0.f, 0.f, static_cast<float>(-M_PI), static_cast<float>(M_PI), 0.01f, "rad" };
    p_center_x_ = { "center_x", 0.f, 0.f, -1.f, 1.f, 0.01f, "" };
    p_center_y_ = { "center_y", 0.f, 0.f, -1.f, 1.f, 0.01f, "" };
    p_speed_    = { "speed",    0.f, 0.f, -20.f, 20.f, 0.1f, "rad/s" };
}

std::vector<FxParam*> BlockRotate::params() {
    return { &p_angle_, &p_center_x_, &p_center_y_, &p_speed_ };
}

const std::vector<FxParam*> BlockRotate::params() const {
    return { const_cast<FxParam*>(&p_angle_),
             const_cast<FxParam*>(&p_center_x_),
             const_cast<FxParam*>(&p_center_y_),
             const_cast<FxParam*>(&p_speed_) };
}

void BlockRotate::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    // Advance auto-spin.
    // M-21: wrap anim_angle_ into (-2π, 2π) every tick to prevent float precision
    // loss that accumulates after ~50 hours of continuous rotation. sin/cos results
    // are unaffected because sin/cos are periodic with period 2π.
    anim_angle_ += p_speed_.effective() * dt;
    anim_angle_ = std::fmod(anim_angle_, static_cast<float>(2.0 * M_PI));

    const float total_angle = p_angle_.effective() + anim_angle_;
    const float cx = p_center_x_.effective();
    const float cy = p_center_y_.effective();
    const float ca = std::cos(total_angle);
    const float sa = std::sin(total_angle);
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv - cx;
        float ny = static_cast<float>(pt.y) * kInv - cy;

        float rx = nx * ca - ny * sa + cx;
        float ry = nx * sa + ny * ca + cy;

        pt.x = static_cast<int16_t>(std::clamp(rx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ry, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
