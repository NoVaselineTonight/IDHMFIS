#include "block_clamp.h"
#include <algorithm>

namespace idhmfis {

static constexpr float kScale = 32767.f;
static constexpr float kInv   = 1.f / kScale;

BlockClamp::BlockClamp() {
    p_x_min_ = { "x_min", -1.f, -1.f, -1.f, 1.f, 0.01f, "" };
    p_x_max_ = { "x_max",  1.f,  1.f, -1.f, 1.f, 0.01f, "" };
    p_y_min_ = { "y_min", -1.f, -1.f, -1.f, 1.f, 0.01f, "" };
    p_y_max_ = { "y_max",  1.f,  1.f, -1.f, 1.f, 0.01f, "" };
    p_mode_  = { "mode",   0.f,  0.f,  0.f, 1.f, 1.f,   "" };
}

std::vector<FxParam*> BlockClamp::params() {
    return { &p_x_min_, &p_x_max_, &p_y_min_, &p_y_max_, &p_mode_ };
}

const std::vector<FxParam*> BlockClamp::params() const {
    return { const_cast<FxParam*>(&p_x_min_),
             const_cast<FxParam*>(&p_x_max_),
             const_cast<FxParam*>(&p_y_min_),
             const_cast<FxParam*>(&p_y_max_),
             const_cast<FxParam*>(&p_mode_) };
}

void BlockClamp::process(PointBuffer& buf, float /*dt*/, const ExprContext& /*ctx*/) {
    const float xmin  = p_x_min_.effective();
    const float xmax  = p_x_max_.effective();
    const float ymin  = p_y_min_.effective();
    const float ymax  = p_y_max_.effective();
    const bool  blank = p_mode_.effective() >= 0.5f;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        bool outside = (nx < xmin || nx > xmax || ny < ymin || ny > ymax);
        if (outside) {
            if (blank) {
                pt.blanked = true;
            } else {
                pt.x = static_cast<int16_t>(std::clamp(nx, xmin, xmax) * kScale);
                pt.y = static_cast<int16_t>(std::clamp(ny, ymin, ymax) * kScale);
            }
        }
    }
}

} // namespace idhmfis
