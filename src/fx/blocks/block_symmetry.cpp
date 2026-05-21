#include "block_symmetry.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockSymmetry::BlockSymmetry() {
    p_folds_    = { "folds",    4.f, 0.f,  2.f, 32.f, 1.f,   "" };
    p_mode_     = { "mode",     0.f, 0.f,  0.f,  1.f, 1.f,   "" };
    p_center_x_ = { "center_x", 0.f, 0.f, -1.f,  1.f, 0.01f, "" };
    p_center_y_ = { "center_y", 0.f, 0.f, -1.f,  1.f, 0.01f, "" };
}

std::vector<FxParam*> BlockSymmetry::params() {
    return { &p_folds_, &p_mode_, &p_center_x_, &p_center_y_ };
}

const std::vector<FxParam*> BlockSymmetry::params() const {
    return { const_cast<FxParam*>(&p_folds_),
             const_cast<FxParam*>(&p_mode_),
             const_cast<FxParam*>(&p_center_x_),
             const_cast<FxParam*>(&p_center_y_) };
}

void BlockSymmetry::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const int   folds  = std::clamp(static_cast<int>(std::round(p_folds_.effective())), 2, 32);
    const bool  mirror = (p_mode_.effective() >= 0.5f);
    const float cx     = p_center_x_.effective();
    const float cy     = p_center_y_.effective();
    const float kInv   = 1.f / 32767.f;
    const float kScale = 32767.f;
    const float step   = static_cast<float>(2.0 * M_PI) / static_cast<float>(folds);

    const int src_count = static_cast<int>(buf.size());
    scratch_.clear();
    scratch_.reserve(static_cast<size_t>(src_count * folds));

    for (int i = 0; i < src_count; ++i) {
        const LaserPoint& src = buf[static_cast<size_t>(i)];
        float nx = static_cast<float>(src.x) * kInv - cx;
        float ny = static_cast<float>(src.y) * kInv - cy;

        for (int f = 0; f < folds; ++f) {
            float angle = step * static_cast<float>(f);
            bool  flip  = mirror && (f & 1);

            float sx = flip ? -nx : nx;
            float sy = ny;

            float ca = std::cos(angle);
            float sa = std::sin(angle);
            float rx = sx * ca - sy * sa + cx;
            float ry = sx * sa + sy * ca + cy;

            LaserPoint p = src;
            p.x = static_cast<int16_t>(std::clamp(rx, -1.f, 1.f) * kScale);
            p.y = static_cast<int16_t>(std::clamp(ry, -1.f, 1.f) * kScale);
            scratch_.push_back(p);
        }
    }

    buf.swap(scratch_);
}

} // namespace idhmfis
