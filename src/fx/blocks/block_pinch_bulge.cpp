#include "block_pinch_bulge.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockPinchBulge::BlockPinchBulge() {
    p_amount_   = { "amount",   0.3f, 0.f, -1.f,  1.f, 0.01f, "" };
    p_center_x_ = { "center_x", 0.f,  0.f, -1.f,  1.f, 0.01f, "" };
    p_center_y_ = { "center_y", 0.f,  0.f, -1.f,  1.f, 0.01f, "" };
    p_radius_   = { "radius",   1.f,  0.f,  0.f,  2.f, 0.01f, "" };
}

std::vector<FxParam*> BlockPinchBulge::params() {
    return { &p_amount_, &p_center_x_, &p_center_y_, &p_radius_ };
}

const std::vector<FxParam*> BlockPinchBulge::params() const {
    return { const_cast<FxParam*>(&p_amount_),
             const_cast<FxParam*>(&p_center_x_),
             const_cast<FxParam*>(&p_center_y_),
             const_cast<FxParam*>(&p_radius_) };
}

void BlockPinchBulge::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const float amount = p_amount_.effective();
    const float cx     = p_center_x_.effective();
    const float cy     = p_center_y_.effective();
    const float radius = std::max(p_radius_.effective(), 0.001f);
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv - cx;
        float ny = static_cast<float>(pt.y) * kInv - cy;
        float dist = std::hypot(nx, ny);

        if (dist < 1e-6f || dist > radius) {
            // Outside radius: no distortion
            continue;
        }

        // Normalised distance within the lens region
        float nd    = dist / radius;      // 0..1
        // Pinch/bulge factor: for bulge (amount>0), points near centre push outward
        // Uses a smooth power curve
        float power = 1.f - amount * (1.f - nd * nd);
        power = std::max(power, 0.01f);
        float new_dist = dist * power;

        float scale = new_dist / dist;
        nx = nx * scale + cx;
        ny = ny * scale + cy;

        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
