#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Stagger: divides the point buffer into groups; alternates which groups are lit.
// mode 0=alternate (A/B flash), 1=chase (sequential lit group), 2=random
class BlockStagger : public IFxBlock {
public:
    BlockStagger();

    const char* name()        const override { return "Stagger"; }
    const char* category()    const override { return "Dots"; }
    const char* description() const override {
        return "Groups of points flash on/off in alternating, chasing, or random patterns.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_group_size_; // 1..16
    FxParam p_phase_;      // 0..1
    FxParam p_speed_;      // Hz
    FxParam p_mode_;       // 0=alternate,1=chase,2=random

    float anim_t_ = 0.f;
};

} // namespace idhmfis
