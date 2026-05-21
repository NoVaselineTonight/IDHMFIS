#include "block_bilateral_mirror.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockBilateralMirror::BlockBilateralMirror() {
    p_mirror_x_        = { "mirror_x",        1.f, 0.f, 0.f, 1.f, 1.f, "" };
    p_mirror_y_        = { "mirror_y",        0.f, 0.f, 0.f, 1.f, 1.f, "" };
    p_mirror_diagonal_ = { "mirror_diagonal", 0.f, 0.f, 0.f, 1.f, 1.f, "" };
}

std::vector<FxParam*> BlockBilateralMirror::params() {
    return { &p_mirror_x_, &p_mirror_y_, &p_mirror_diagonal_ };
}

const std::vector<FxParam*> BlockBilateralMirror::params() const {
    return { const_cast<FxParam*>(&p_mirror_x_),
             const_cast<FxParam*>(&p_mirror_y_),
             const_cast<FxParam*>(&p_mirror_diagonal_) };
}

void BlockBilateralMirror::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const bool do_x   = (p_mirror_x_.effective() >= 0.5f);
    const bool do_y   = (p_mirror_y_.effective() >= 0.5f);
    const bool do_d   = (p_mirror_diagonal_.effective() >= 0.5f);

    scratch_ = buf; // copy current points as base

    if (do_x) {
        const int n = static_cast<int>(scratch_.size());
        scratch_.reserve(static_cast<size_t>(n * 2));
        for (int i = 0; i < n; ++i) {
            LaserPoint p = scratch_[static_cast<size_t>(i)];
            p.x = static_cast<int16_t>(-p.x);
            scratch_.push_back(p);
        }
    }
    if (do_y) {
        const int n = static_cast<int>(scratch_.size());
        scratch_.reserve(static_cast<size_t>(n * 2));
        for (int i = 0; i < n; ++i) {
            LaserPoint p = scratch_[static_cast<size_t>(i)];
            p.y = static_cast<int16_t>(-p.y);
            scratch_.push_back(p);
        }
    }
    if (do_d) {
        const int n = static_cast<int>(scratch_.size());
        scratch_.reserve(static_cast<size_t>(n * 2));
        for (int i = 0; i < n; ++i) {
            LaserPoint p = scratch_[static_cast<size_t>(i)];
            int16_t tmp = p.x;
            p.x = p.y;
            p.y = tmp;
            scratch_.push_back(p);
        }
    }

    buf.swap(scratch_);
}

} // namespace idhmfis
