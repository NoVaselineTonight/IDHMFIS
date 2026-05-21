#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Hard bounding-box clip. Mode 0 = clamp-to-edge, mode 1 = blank points outside.
class BlockClamp : public IFxBlock {
public:
    BlockClamp();

    const char* name()        const override { return "Clamp"; }
    const char* category()    const override { return "Safety"; }
    const char* description() const override {
        return "Hard-clips output to a configurable bounding rectangle. "
               "Mode: 0 = clamp to edge, 1 = blank points outside the box.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_x_min_;  // -1..1
    FxParam p_x_max_;  // -1..1
    FxParam p_y_min_;  // -1..1
    FxParam p_y_max_;  // -1..1
    FxParam p_mode_;   // 0 = clamp-to-edge, 1 = blank
};

} // namespace idhmfis
