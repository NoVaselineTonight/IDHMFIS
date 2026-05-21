#include "block_scanner_line.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockScannerLine::BlockScannerLine() {
    p_axis_    = { "axis",    0.f, 0.f, 0.f, 1.f,  1.f,   "" };
    p_speed_   = { "speed",   1.f, 0.f, 0.1f, 10.f, 0.1f, "Hz" };
    p_width_   = { "width",   0.05f, 0.f, 0.f, 0.2f, 0.005f, "" };
    p_color_r_ = { "color_r", 1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_g_ = { "color_g", 1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_b_ = { "color_b", 1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_blend_   = { "blend",   0.f, 0.f, 0.f, 1.f, 1.f,   "" };
}

std::vector<FxParam*> BlockScannerLine::params() {
    return { &p_axis_, &p_speed_, &p_width_,
             &p_color_r_, &p_color_g_, &p_color_b_, &p_blend_ };
}

const std::vector<FxParam*> BlockScannerLine::params() const {
    return { const_cast<FxParam*>(&p_axis_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_width_),
             const_cast<FxParam*>(&p_color_r_),
             const_cast<FxParam*>(&p_color_g_),
             const_cast<FxParam*>(&p_color_b_),
             const_cast<FxParam*>(&p_blend_) };
}

void BlockScannerLine::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed  = p_speed_.effective();
    const float width  = p_width_.effective();
    const bool  horiz  = (p_axis_.effective() < 0.5f);
    const bool  gate   = (p_blend_.effective() >= 0.5f);
    const uint8_t cr   = static_cast<uint8_t>(std::clamp(p_color_r_.effective(), 0.f, 1.f) * 255.f);
    const uint8_t cg   = static_cast<uint8_t>(std::clamp(p_color_g_.effective(), 0.f, 1.f) * 255.f);
    const uint8_t cb   = static_cast<uint8_t>(std::clamp(p_color_b_.effective(), 0.f, 1.f) * 255.f);
    const float kInv   = 1.f / 32767.f;

    // Advance scanner position (triangle wave)
    anim_pos_ += anim_dir_ * speed * 2.f * dt; // 2.f because range is 2 (-1..1)
    if (anim_pos_ >  1.f) { anim_pos_ =  2.f - anim_pos_; anim_dir_ = -1.f; }
    if (anim_pos_ < -1.f) { anim_pos_ = -2.f - anim_pos_; anim_dir_ =  1.f; }

    for (LaserPoint& pt : buf) {
        float coord = horiz ? (static_cast<float>(pt.y) * kInv)
                            : (static_cast<float>(pt.x) * kInv);
        float dist  = std::fabs(coord - anim_pos_);
        bool  on_line = (dist <= width);

        if (on_line) {
            if (!gate) {
                // add: boost colour toward line colour
                pt.r = static_cast<uint8_t>(std::min(255, static_cast<int>(pt.r) + cr));
                pt.g = static_cast<uint8_t>(std::min(255, static_cast<int>(pt.g) + cg));
                pt.b = static_cast<uint8_t>(std::min(255, static_cast<int>(pt.b) + cb));
            }
            // gate: keep as-is (already visible)
        } else {
            if (gate) {
                pt.blanked = true;
            }
        }
    }
}

} // namespace idhmfis
