#include "block_gradient_map.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockGradientMap::BlockGradientMap() {
    p_start_r_  = { "start_r",  1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_start_g_  = { "start_g",  0.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_start_b_  = { "start_b",  1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_end_r_    = { "end_r",    0.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_end_g_    = { "end_g",    1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_end_b_    = { "end_b",    1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_map_axis_ = { "map_axis", 0.f, 0.f, 0.f, 3.f, 1.f,   "" };
    p_speed_    = { "speed",    0.f, 0.f, 0.f, 4.f, 0.01f, "Hz" };
}

std::vector<FxParam*> BlockGradientMap::params() {
    return { &p_start_r_, &p_start_g_, &p_start_b_,
             &p_end_r_,   &p_end_g_,   &p_end_b_,
             &p_map_axis_, &p_speed_ };
}

const std::vector<FxParam*> BlockGradientMap::params() const {
    return { const_cast<FxParam*>(&p_start_r_),
             const_cast<FxParam*>(&p_start_g_),
             const_cast<FxParam*>(&p_start_b_),
             const_cast<FxParam*>(&p_end_r_),
             const_cast<FxParam*>(&p_end_g_),
             const_cast<FxParam*>(&p_end_b_),
             const_cast<FxParam*>(&p_map_axis_),
             const_cast<FxParam*>(&p_speed_) };
}

void BlockGradientMap::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    anim_offset_ = std::fmod(anim_offset_ + p_speed_.effective() * dt, 1.f);

    const float sr = p_start_r_.effective();
    const float sg = p_start_g_.effective();
    const float sb = p_start_b_.effective();
    const float er = p_end_r_.effective();
    const float eg = p_end_g_.effective();
    const float eb = p_end_b_.effective();
    const int   axis = static_cast<int>(std::round(p_map_axis_.effective()));
    const float kInv = 1.f / 32767.f;
    const int   n    = static_cast<int>(buf.size());

    for (int i = 0; i < n; ++i) {
        LaserPoint& pt = buf[static_cast<size_t>(i)];
        if (pt.blanked) continue;

        float t = 0.f;
        switch (axis) {
        case 0: t = (static_cast<float>(pt.x) * kInv + 1.f) * 0.5f; break;
        case 1: t = (static_cast<float>(pt.y) * kInv + 1.f) * 0.5f; break;
        case 2: {
            float nx = static_cast<float>(pt.x) * kInv;
            float ny = static_cast<float>(pt.y) * kInv;
            t = std::min(std::hypot(nx, ny), 1.f);
            break;
        }
        case 3: t = (n > 1) ? static_cast<float>(i) / static_cast<float>(n - 1) : 0.f; break;
        default: break;
        }

        // Apply animated offset and wrap
        t = std::fmod(t + anim_offset_, 1.f);
        t = std::clamp(t, 0.f, 1.f);

        pt.r = static_cast<uint8_t>(std::clamp(sr + (er - sr) * t, 0.f, 1.f) * 255.f);
        pt.g = static_cast<uint8_t>(std::clamp(sg + (eg - sg) * t, 0.f, 1.f) * 255.f);
        pt.b = static_cast<uint8_t>(std::clamp(sb + (eb - sb) * t, 0.f, 1.f) * 255.f);
    }
}

} // namespace idhmfis
