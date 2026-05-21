#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Mirror / kaleidoscope geometry effect.
// mode: 0=X, 1=Y, 2=Both, 3=Diagonal, 4=KaleidoscopeN
class BlockMirror : public IFxBlock {
public:
    BlockMirror();

    const char* name()        const override { return "Mirror"; }
    const char* category()    const override { return "Geometry"; }
    const char* description() const override {
        return "Folds/reflects points. Kaleidoscope mode radially repeats into N sectors.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_mode_;       // 0=X,1=Y,2=Both,3=Diagonal,4=KaleidoscopeN
    FxParam p_n_sectors_;  // 2..16 (used in Kaleidoscope mode)
};

} // namespace idhmfis
