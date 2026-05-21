#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Lens distortion: pinch (negative amount) or bulge (positive amount).
class BlockPinchBulge : public IFxBlock {
public:
    BlockPinchBulge();

    const char* name()        const override { return "PinchBulge"; }
    const char* category()    const override { return "Distortion"; }
    const char* description() const override {
        return "Lens distortion. amount>0 = bulge out, amount<0 = pinch in.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_amount_;    // -1..1
    FxParam p_center_x_;  // -1..1
    FxParam p_center_y_;  // -1..1
    FxParam p_radius_;    // 0..2
};

} // namespace idhmfis
