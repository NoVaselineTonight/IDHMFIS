#include "block_spiral_scan.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockSpiralScan::BlockSpiralScan() {
    p_turns_        = { "turns",        2.f,  0.f, 0.5f, 8.f,  0.1f,  "" };
    p_speed_        = { "speed",        0.5f, 0.f, 0.1f, 4.f,  0.01f, "Hz" };
    p_direction_    = { "direction",    0.f,  0.f, 0.f,  1.f,  1.f,   "" };
    p_start_radius_ = { "start_radius", 0.1f, 0.f, 0.f,  1.f,  0.01f, "" };
    p_end_radius_   = { "end_radius",   0.9f, 0.f, 0.f,  1.f,  0.01f, "" };
}

std::vector<FxParam*> BlockSpiralScan::params() {
    return { &p_turns_, &p_speed_, &p_direction_, &p_start_radius_, &p_end_radius_ };
}

const std::vector<FxParam*> BlockSpiralScan::params() const {
    return { const_cast<FxParam*>(&p_turns_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_direction_),
             const_cast<FxParam*>(&p_start_radius_),
             const_cast<FxParam*>(&p_end_radius_) };
}

void BlockSpiralScan::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed     = p_speed_.effective();
    anim_phase_ = std::fmod(anim_phase_ + speed * dt, 1.f);

    const float turns   = p_turns_.effective();
    const bool  inward  = (p_direction_.effective() < 0.5f);
    const float r0      = p_start_radius_.effective();
    const float r1      = p_end_radius_.effective();
    const float kScale  = 32767.f;
    const float kInv    = 1.f / kScale;
    const float twopi   = static_cast<float>(2.0 * M_PI);

    float t      = inward ? anim_phase_ : (1.f - anim_phase_);
    float angle  = t * turns * twopi;
    float radius = r0 + (r1 - r0) * t;
    float ca     = std::cos(angle);
    float sa     = std::sin(angle);

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        // Rotate and scale by the animated spiral radius
        float rx = (nx * ca - ny * sa) * radius;
        float ry = (nx * sa + ny * ca) * radius;

        pt.x = static_cast<int16_t>(std::clamp(rx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ry, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
