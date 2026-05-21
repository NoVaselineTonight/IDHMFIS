#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Maps each point's position to a color gradient.
// map_axis: 0=X, 1=Y, 2=distance from centre, 3=point index
class BlockGradientMap : public IFxBlock {
public:
    BlockGradientMap();

    const char* name()        const override { return "GradientMap"; }
    const char* category()    const override { return "Color"; }
    const char* description() const override {
        return "Overwrites each point's colour from a gradient based on position or index.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_start_r_, p_start_g_, p_start_b_;
    FxParam p_end_r_,   p_end_g_,   p_end_b_;
    FxParam p_map_axis_; // 0=X,1=Y,2=dist,3=index
    FxParam p_speed_;    // Hz (animated shift)

    float anim_offset_ = 0.f;
};

} // namespace idhmfis
