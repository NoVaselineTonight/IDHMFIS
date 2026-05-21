#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Circular exclusion zone: blanks any point whose normalised distance from
// (cx, cy) is less than radius.  Stack multiple instances for multi-zone rigs.
class BlockSafetyZone : public IFxBlock {
public:
    BlockSafetyZone();

    const char* name()        const override { return "SafetyZone"; }
    const char* category()    const override { return "Safety"; }
    const char* description() const override {
        return "Blanks points inside a circular exclusion zone. "
               "Stack blocks for multi-zone setups (up to 8).";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_cx_;      // centre X  -1..1
    FxParam p_cy_;      // centre Y  -1..1
    FxParam p_radius_;  // radius in normalised units  0..2
};

} // namespace idhmfis
