#include "block_mirror.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockMirror::BlockMirror() {
    p_mode_      = { "mode",      0.f, 0.f, 0.f, 4.f,  1.f, "" };
    p_n_sectors_ = { "n_sectors", 6.f, 0.f, 2.f, 16.f, 1.f, "" };
}

std::vector<FxParam*> BlockMirror::params() {
    return { &p_mode_, &p_n_sectors_ };
}

const std::vector<FxParam*> BlockMirror::params() const {
    return { const_cast<FxParam*>(&p_mode_),
             const_cast<FxParam*>(&p_n_sectors_) };
}

void BlockMirror::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const int   mode = static_cast<int>(std::round(p_mode_.effective()));
    const int   nsec = std::clamp(static_cast<int>(std::round(p_n_sectors_.effective())), 2, 16);
    const float kScale = 32767.f;
    const float kInv   = 1.f / kScale;

    for (LaserPoint& pt : buf) {
        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        switch (mode) {
        case 0:
            // Mirror X: fold negative side to positive
            nx = std::fabs(nx);
            break;

        case 1:
            // Mirror Y
            ny = std::fabs(ny);
            break;

        case 2:
            // Mirror both axes
            nx = std::fabs(nx);
            ny = std::fabs(ny);
            break;

        case 3:
            // Diagonal fold: fold below the y=x line
            if (ny < nx) {
                float tmp = nx;
                nx = ny;
                ny = tmp;
            }
            break;

        case 4: {
            // Kaleidoscope: fold into the first sector of nsec sectors
            float angle   = std::atan2(ny, nx);
            float radius  = std::hypot(nx, ny);
            float sector  = static_cast<float>(M_PI * 2.0) / static_cast<float>(nsec);

            // Wrap angle into [0, sector)
            angle = std::fmod(angle, sector);
            if (angle < 0.f) angle += sector;

            // Fold so second half mirrors back
            float half = sector * 0.5f;
            if (angle > half) angle = sector - angle;

            nx = radius * std::cos(angle);
            ny = radius * std::sin(angle);
            break;
        }

        default:
            break;
        }

        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
