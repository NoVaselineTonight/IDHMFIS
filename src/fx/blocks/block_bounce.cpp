#include "block_bounce.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockBounce::BlockBounce() {
    p_gravity_     = { "gravity",     0.5f, 0.f,  0.f,  2.f,  0.01f, "" };
    p_restitution_ = { "restitution", 0.8f, 0.f,  0.f,  1.f,  0.01f, "" };
    p_floor_y_     = { "floor_y",    -0.8f, 0.f, -1.f,  1.f,  0.01f, "" };
    p_speed_x_     = { "speed_x",    0.3f,  0.f, -2.f,  2.f,  0.01f, "" };
    p_speed_y_     = { "speed_y",    1.0f,  0.f, -2.f,  2.f,  0.01f, "" };
}

std::vector<FxParam*> BlockBounce::params() {
    return { &p_gravity_, &p_restitution_, &p_floor_y_, &p_speed_x_, &p_speed_y_ };
}

const std::vector<FxParam*> BlockBounce::params() const {
    return { const_cast<FxParam*>(&p_gravity_),
             const_cast<FxParam*>(&p_restitution_),
             const_cast<FxParam*>(&p_floor_y_),
             const_cast<FxParam*>(&p_speed_x_),
             const_cast<FxParam*>(&p_speed_y_) };
}

void BlockBounce::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float gravity     = p_gravity_.effective();
    const float restitution = p_restitution_.effective();
    const float floor_y     = p_floor_y_.effective();

    if (!inited_) {
        vel_x_  = p_speed_x_.effective();
        vel_y_  = p_speed_y_.effective();
        pos_x_  = 0.f;
        pos_y_  = 0.f;
        inited_ = true;
    }

    // Integrate
    vel_y_ -= gravity * dt;
    pos_x_ += vel_x_ * dt;
    pos_y_ += vel_y_ * dt;

    // Bounce off floor
    if (pos_y_ < floor_y) {
        pos_y_ = floor_y;
        vel_y_ = -vel_y_ * restitution;
    }

    // Wrap horizontally
    if (pos_x_ >  1.f) { pos_x_ -= 2.f; }
    if (pos_x_ < -1.f) { pos_x_ += 2.f; }

    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;
    const int16_t ox = static_cast<int16_t>(std::clamp(pos_x_, -1.f, 1.f) * kScale);
    const int16_t oy = static_cast<int16_t>(std::clamp(pos_y_, -1.f, 1.f) * kScale);

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv + pos_x_;
        float ny = static_cast<float>(pt.y) * kInv + pos_y_;
        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
    (void)ox; (void)oy;
}

} // namespace idhmfis
