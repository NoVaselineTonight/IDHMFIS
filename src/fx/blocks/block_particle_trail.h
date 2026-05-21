#pragma once
#include "../fx_block.h"
#include <vector>

namespace idhmfis {

// Particle trail: outputs the pattern multiple times with decreasing opacity
// and time-offset positions, creating a motion trail.
class BlockParticleTrail : public IFxBlock {
public:
    BlockParticleTrail();

    const char* name()        const override { return "ParticleTrail"; }
    const char* category()    const override { return "Dots"; }
    const char* description() const override {
        return "Outputs ghost copies of the pattern with decreasing brightness to create a trail.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_trail_length_;   // 2..8
    FxParam p_trail_decay_;    // 0..1
    FxParam p_trail_offset_s_; // 0..0.5 seconds per ghost

    // Circular history buffer: stores the last kMaxHistory frames
    static constexpr int kMaxHistory = 8;
    PointBuffer history_[kMaxHistory];
    int  history_head_ = 0;
    int  history_fill_ = 0;

    PointBuffer scratch_;
};

} // namespace idhmfis
