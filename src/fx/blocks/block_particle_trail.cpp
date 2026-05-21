#include "block_particle_trail.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockParticleTrail::BlockParticleTrail() {
    p_trail_length_   = { "trail_length",   4.f,  0.f, 2.f, 8.f,  1.f,   "" };
    p_trail_decay_    = { "trail_decay",    0.6f, 0.f, 0.f, 1.f,  0.01f, "" };
    p_trail_offset_s_ = { "trail_offset_s", 0.05f,0.f, 0.f, 0.5f, 0.01f, "s" };
}

std::vector<FxParam*> BlockParticleTrail::params() {
    return { &p_trail_length_, &p_trail_decay_, &p_trail_offset_s_ };
}

const std::vector<FxParam*> BlockParticleTrail::params() const {
    return { const_cast<FxParam*>(&p_trail_length_),
             const_cast<FxParam*>(&p_trail_decay_),
             const_cast<FxParam*>(&p_trail_offset_s_) };
}

void BlockParticleTrail::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const int  length = std::clamp(static_cast<int>(std::round(p_trail_length_.effective())), 2, kMaxHistory);
    const float decay  = p_trail_decay_.effective();

    // Push current frame into history
    history_[static_cast<size_t>(history_head_)] = buf;
    history_head_ = (history_head_ + 1) % kMaxHistory;
    if (history_fill_ < kMaxHistory) ++history_fill_;

    // Build output: current frame + ghost copies
    scratch_ = buf; // layer 0 = current, full brightness

    int avail = std::min(history_fill_, length) - 1; // ghost layers
    for (int g = 1; g <= avail; ++g) {
        int   src_idx = (history_head_ - 1 - g + kMaxHistory) % kMaxHistory;
        float alpha   = std::pow(decay, static_cast<float>(g));
        const PointBuffer& ghost = history_[static_cast<size_t>(src_idx)];

        for (const LaserPoint& gpt : ghost) {
            if (gpt.blanked) continue;
            LaserPoint p = gpt;
            p.r = static_cast<uint8_t>(static_cast<float>(p.r) * alpha);
            p.g = static_cast<uint8_t>(static_cast<float>(p.g) * alpha);
            p.b = static_cast<uint8_t>(static_cast<float>(p.b) * alpha);
            scratch_.push_back(p);
        }
    }

    buf.swap(scratch_);
}

} // namespace idhmfis
