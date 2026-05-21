#include "block_chroma_shift.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockChromaShift::BlockChromaShift() {
    p_shift_amount_ = { "shift_amount", 0.03f, 0.f, 0.f, 0.1f,  0.001f, "" };
    p_shift_angle_  = { "shift_angle",  0.f,   0.f, 0.f, 360.f, 1.f,    "deg" };
}

std::vector<FxParam*> BlockChromaShift::params() {
    return { &p_shift_amount_, &p_shift_angle_ };
}

const std::vector<FxParam*> BlockChromaShift::params() const {
    return { const_cast<FxParam*>(&p_shift_amount_),
             const_cast<FxParam*>(&p_shift_angle_) };
}

void BlockChromaShift::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const float amount = p_shift_amount_.effective();
    const float angle  = p_shift_angle_.effective() * static_cast<float>(M_PI) / 180.f;
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    float dx = amount * std::cos(angle);
    float dy = amount * std::sin(angle);

    // For each lit point, emit three channel-only versions at offsets
    scratch_.clear();
    scratch_.reserve(buf.size() * 3u);

    for (const LaserPoint& pt : buf) {
        if (pt.blanked) {
            scratch_.push_back(pt);
            continue;
        }

        // Red channel: shifted in direction
        {
            LaserPoint r = pt;
            r.g = 0; r.b = 0;
            float nx = static_cast<float>(r.x) * kInv + dx;
            float ny = static_cast<float>(r.y) * kInv + dy;
            r.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
            r.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
            scratch_.push_back(r);
        }
        // Green channel: no shift
        {
            LaserPoint g = pt;
            g.r = 0; g.b = 0;
            scratch_.push_back(g);
        }
        // Blue channel: shifted opposite
        {
            LaserPoint b = pt;
            b.r = 0; b.g = 0;
            float nx = static_cast<float>(b.x) * kInv - dx;
            float ny = static_cast<float>(b.y) * kInv - dy;
            b.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
            b.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
            scratch_.push_back(b);
        }
    }

    buf.swap(scratch_);
}

} // namespace idhmfis
