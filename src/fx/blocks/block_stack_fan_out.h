#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Collapses all objects toward a configurable stack point then fans them back
// out over a repeating cycle.
class BlockStackFanOut : public IFxBlock {
public:
    BlockStackFanOut();

    const char* name()        const override { return "StackFanOut"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Collapses objects to a stack point then fans them out over a cycle.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_speed_;       // 0.1..5 Hz
    FxParam p_fan_radius_;  // 0..1
    FxParam p_stack_x_;     // -1..1
    FxParam p_stack_y_;     // -1..1
    FxParam p_axis_;        // 0=X, 1=Y, 2=Both
    FxParam p_phase_;       // 0..360 deg

    float anim_phase_ = 0.f;
};

} // namespace idhmfis
