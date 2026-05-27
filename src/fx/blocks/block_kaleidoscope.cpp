#include "block_kaleidoscope.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockKaleidoscope::BlockKaleidoscope() {
    p_segments_     = { "segments",     6.f,  0.f,  2.f, 16.f, 1.f,   "" };
    p_angle_offset_ = { "angle_offset", 0.f,  0.f,  0.f, 360.f, 1.f, "deg" };
    p_flip_odd_     = { "flip_odd",     0.f,  0.f,  0.f,  1.f, 1.f,   "" };
}

std::vector<FxParam*> BlockKaleidoscope::params() {
    return { &p_segments_, &p_angle_offset_, &p_flip_odd_ };
}

const std::vector<FxParam*> BlockKaleidoscope::params() const {
    return { const_cast<FxParam*>(&p_segments_),
             const_cast<FxParam*>(&p_angle_offset_),
             const_cast<FxParam*>(&p_flip_odd_) };
}

void BlockKaleidoscope::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const int   segs   = std::clamp(static_cast<int>(std::round(p_segments_.effective())), 2, 16);
    const float aoffs  = p_angle_offset_.effective() * static_cast<float>(M_PI) / 180.f;
    const bool  flip   = (p_flip_odd_.effective() >= 0.5f);
    const float kInv   = 1.f / 32767.f;
    const float kScale = 32767.f;
    const float sector = static_cast<float>(2.0 * M_PI) / static_cast<float>(segs);
    const float half   = sector * 0.5f;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        float angle  = std::atan2(ny, nx) + aoffs;
        float radius = std::hypot(nx, ny);

        // Determine which sector the original angle falls into (for flip_odd parity)
        float raw_angle = std::atan2(ny, nx) + aoffs;
        // Wrap raw_angle into [0, 2pi) before dividing into sectors
        float full_circle = static_cast<float>(2.0 * M_PI);
        raw_angle = std::fmod(raw_angle, full_circle);
        if (raw_angle < 0.f) raw_angle += full_circle;
        int sector_idx = static_cast<int>(raw_angle / sector);
        bool odd = (sector_idx & 1) != 0;

        // Wrap angle into [0, sector)
        angle = std::fmod(angle, sector);
        if (angle < 0.f) angle += sector;

        if (angle > half) {
            angle = sector - angle;
        }
        if (flip && odd) {
            angle = -angle;
        }

        nx = radius * std::cos(angle);
        ny = radius * std::sin(angle);

        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
