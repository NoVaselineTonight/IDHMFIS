#include "block_stack_fan_out.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockStackFanOut::BlockStackFanOut() {
    p_speed_      = { "speed",      1.f,  0.f,  0.1f,  5.f,  0.01f, "Hz"  };
    p_fan_radius_ = { "fan_radius", 0.5f, 0.f,  0.f,   1.f,  0.01f, ""    };
    p_stack_x_    = { "stack_x",   0.f,  0.f, -1.f,   1.f,  0.01f, ""    };
    p_stack_y_    = { "stack_y",   0.f,  0.f, -1.f,   1.f,  0.01f, ""    };
    p_axis_       = { "axis",      2.f,  0.f,  0.f,   2.f,  1.f,   ""    };
    p_phase_      = { "phase",     0.f,  0.f,  0.f, 360.f,  1.f,   "deg" };
}

std::vector<FxParam*> BlockStackFanOut::params() {
    return { &p_speed_, &p_fan_radius_, &p_stack_x_, &p_stack_y_,
             &p_axis_, &p_phase_ };
}

const std::vector<FxParam*> BlockStackFanOut::params() const {
    return { const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_fan_radius_),
             const_cast<FxParam*>(&p_stack_x_),
             const_cast<FxParam*>(&p_stack_y_),
             const_cast<FxParam*>(&p_axis_),
             const_cast<FxParam*>(&p_phase_) };
}

void BlockStackFanOut::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed  = p_speed_.effective();
    anim_phase_ += speed * static_cast<float>(2.0 * M_PI) * dt;

    const float fan_radius = p_fan_radius_.effective();
    const float stack_x    = p_stack_x_.effective();
    const float stack_y    = p_stack_y_.effective();
    const int   axis       = static_cast<int>(p_axis_.effective() + 0.5f);
    const float phase0     = p_phase_.effective() * static_cast<float>(M_PI) / 180.f;

    // fan_t ranges 0 (collapsed) to 1 (fanned out)
    const float fan_t = 0.5f * (1.f + std::sin(anim_phase_ + phase0));

    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        // Offset from stack point in natural (pre-process) space
        float ox = nx - stack_x;
        float oy = ny - stack_y;

        // Apply fan on selected axes
        float nx_out = nx;
        float ny_out = ny;

        if (axis == 0 || axis == 2) {
            nx_out = stack_x + ox * fan_t * (1.f + fan_radius);
        }
        if (axis == 1 || axis == 2) {
            ny_out = stack_y + oy * fan_t * (1.f + fan_radius);
        }

        pt.x = static_cast<int16_t>(std::clamp(nx_out, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny_out, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
