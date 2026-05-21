#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Independent X/Y scale around a configurable centre.
class BlockScale : public IFxBlock {
public:
    BlockScale();

    const char* name()        const override { return "Scale"; }
    const char* category()    const override { return "Geometry"; }
    const char* description() const override {
        return "Scales point positions relative to a centre point, with independent X/Y factors.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_scale_x_;   // 0.01..4
    FxParam p_scale_y_;   // 0.01..4
    FxParam p_center_x_;  // -1..1
    FxParam p_center_y_;  // -1..1
};

} // namespace idhmfis
