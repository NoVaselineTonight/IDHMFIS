#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Mirrors points across X axis, Y axis, and/or the diagonal.
// Each enabled axis duplicates the existing points, reflected.
class BlockBilateralMirror : public IFxBlock {
public:
    BlockBilateralMirror();

    const char* name()        const override { return "BilateralMirror"; }
    const char* category()    const override { return "Symmetry"; }
    const char* description() const override {
        return "Duplicates points mirrored across X, Y, and/or diagonal axes.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_mirror_x_;        // 0/1
    FxParam p_mirror_y_;        // 0/1
    FxParam p_mirror_diagonal_; // 0/1

    PointBuffer scratch_;
};

} // namespace idhmfis
