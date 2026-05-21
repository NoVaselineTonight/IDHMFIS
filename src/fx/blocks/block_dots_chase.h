#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Chasing dots: selects N evenly-spaced points along the path and lights only those.
// All other points are blanked. The selection position advances over time.
class BlockDotsChase : public IFxBlock {
public:
    BlockDotsChase();

    const char* name()        const override { return "DotsChase"; }
    const char* category()    const override { return "Dots"; }
    const char* description() const override {
        return "N dots chase along the path. Remaining points are blanked.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_dot_count_;  // 1..32
    FxParam p_spacing_;    // 0..1 (fractional gap between dots)
    FxParam p_speed_;      // Hz -4..4
    FxParam p_dot_size_;   // 1..10 (points around each dot that stay lit)
    FxParam p_fade_tail_;  // 0/1

    float anim_offset_ = 0.f;  // 0..1 phase
};

} // namespace idhmfis
