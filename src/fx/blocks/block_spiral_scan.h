#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Spiral scan: rotates while scaling in/out, creating a spiral sweep effect.
class BlockSpiralScan : public IFxBlock {
public:
    BlockSpiralScan();

    const char* name()        const override { return "SpiralScan"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Rotates and scales the pattern in a spiral sweep. direction=0 inward, 1 outward.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_turns_;        // 0.5..8
    FxParam p_speed_;        // Hz 0.1..4
    FxParam p_direction_;    // 0=in, 1=out
    FxParam p_start_radius_; // 0..1
    FxParam p_end_radius_;   // 0..1

    float anim_phase_ = 0.f;  // 0..1 sweep position
};

} // namespace idhmfis
