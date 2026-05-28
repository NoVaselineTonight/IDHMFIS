#include "block_scale.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockScale::BlockScale() {
    p_scale_x_  = { "scale_x",  1.f, 0.f, 0.01f, 4.f, 0.01f, "" };
    p_scale_y_  = { "scale_y",  1.f, 0.f, 0.01f, 4.f, 0.01f, "" };
    p_center_x_ = { "center_x", 0.f, 0.f, -1.f,  1.f, 0.01f, "" };
    p_center_y_ = { "center_y", 0.f, 0.f, -1.f,  1.f, 0.01f, "" };
}

std::vector<FxParam*> BlockScale::params() {
    return { &p_scale_x_, &p_scale_y_, &p_center_x_, &p_center_y_ };
}

const std::vector<FxParam*> BlockScale::params() const {
    return { const_cast<FxParam*>(&p_scale_x_),
             const_cast<FxParam*>(&p_scale_y_),
             const_cast<FxParam*>(&p_center_x_),
             const_cast<FxParam*>(&p_center_y_) };
}

void BlockScale::process(PointBuffer& buf, float /*dt*/, const ExprContext& /*ctx*/) {
    const float sx = p_scale_x_.effective();
    const float sy = p_scale_y_.effective();
    const float cx = p_center_x_.effective();
    const float cy = p_center_y_.effective();
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        float nx = (static_cast<float>(pt.x) * kInv - cx) * sx + cx;
        float ny = (static_cast<float>(pt.y) * kInv - cy) * sy + cy;

        if (!pt.blanked && (nx < -1.f || nx > 1.f || ny < -1.f || ny > 1.f))
            pt.blanked = true;

        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
