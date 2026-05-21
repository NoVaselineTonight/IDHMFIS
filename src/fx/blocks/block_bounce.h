#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Physics bounce: offsets the pattern as if bouncing under gravity.
class BlockBounce : public IFxBlock {
public:
    BlockBounce();

    const char* name()        const override { return "Bounce"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Offsets the pattern as if bouncing under gravity with restitution.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_gravity_;      // 0..2
    FxParam p_restitution_;  // 0..1
    FxParam p_floor_y_;      // -1..1
    FxParam p_speed_x_;      // -2..2
    FxParam p_speed_y_;      // -2..2 (initial vertical velocity)

    float pos_x_  = 0.f;
    float pos_y_  = 0.f;
    float vel_x_  = 0.f;
    float vel_y_  = 0.f;
    bool  inited_ = false;
};

} // namespace idhmfis
