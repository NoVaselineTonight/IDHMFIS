#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Maps each point's luminance to a linear hue ramp between hue_start and hue_end.
class BlockColorRamp : public IFxBlock {
public:
    BlockColorRamp();

    const char* name()        const override { return "ColorRamp"; }
    const char* category()    const override { return "Color"; }
    const char* description() const override {
        return "Remaps each point's luminance to a hue gradient from hue_start to hue_end.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_hue_start_;   // 0..1
    FxParam p_hue_end_;     // 0..1
    FxParam p_saturation_;  // 0..1
    FxParam p_brightness_;  // 0..1
};

} // namespace idhmfis
