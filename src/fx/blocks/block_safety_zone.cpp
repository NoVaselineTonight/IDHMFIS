#include "block_safety_zone.h"
#include <cmath>

namespace idhmfis {

static constexpr float kScale = 32767.f;
static constexpr float kInv   = 1.f / kScale;

BlockSafetyZone::BlockSafetyZone() {
    p_cx_     = { "cx",     0.f, 0.f, -1.f, 1.f,  0.01f, "" };
    p_cy_     = { "cy",     0.f, 0.f, -1.f, 1.f,  0.01f, "" };
    p_radius_ = { "radius", 0.1f, 0.1f, 0.f, 2.f, 0.01f, "" };
}

std::vector<FxParam*> BlockSafetyZone::params() {
    return { &p_cx_, &p_cy_, &p_radius_ };
}

const std::vector<FxParam*> BlockSafetyZone::params() const {
    return { const_cast<FxParam*>(&p_cx_),
             const_cast<FxParam*>(&p_cy_),
             const_cast<FxParam*>(&p_radius_) };
}

void BlockSafetyZone::process(PointBuffer& buf, float /*dt*/, const ExprContext& /*ctx*/) {
    const float cx  = p_cx_.effective();
    const float cy  = p_cy_.effective();
    const float r2  = p_radius_.effective() * p_radius_.effective();

    for (LaserPoint& pt : buf) {
        float dx = static_cast<float>(pt.x) * kInv - cx;
        float dy = static_cast<float>(pt.y) * kInv - cy;
        if (dx * dx + dy * dy < r2)
            pt.blanked = true;
    }
}

} // namespace idhmfis
