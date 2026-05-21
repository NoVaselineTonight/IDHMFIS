#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Chromatic aberration: separates R, G, B into spatially offset sub-streams.
class BlockChromaShift : public IFxBlock {
public:
    BlockChromaShift();

    const char* name()        const override { return "ChromaShift"; }
    const char* category()    const override { return "Color"; }
    const char* description() const override {
        return "RGB chromatic aberration: splits colour channels into spatially offset copies.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_shift_amount_; // 0..0.1
    FxParam p_shift_angle_;  // 0..360 deg

    PointBuffer scratch_;
};

} // namespace idhmfis
