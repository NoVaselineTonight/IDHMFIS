#pragma once
#include "../fx_block.h"
#include <vector>

namespace idhmfis {

// N-fold radial symmetry: copies each lit point N times around the centre.
// mode 0 = copy (rotate-only), mode 1 = mirror (flip alternate copies).
class BlockSymmetry : public IFxBlock {
public:
    BlockSymmetry();

    const char* name()        const override { return "Symmetry"; }
    const char* category()    const override { return "Symmetry"; }
    const char* description() const override {
        return "N-fold radial symmetry. Rotates each point N times around centre. "
               "mode=0 copies, mode=1 mirrors alternate folds.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_folds_;     // 2..32  default 4
    FxParam p_mode_;      // 0=copy, 1=mirror
    FxParam p_center_x_;  // -1..1
    FxParam p_center_y_;  // -1..1

    PointBuffer scratch_;  // reused every tick; no per-tick allocation after first
};

} // namespace idhmfis
