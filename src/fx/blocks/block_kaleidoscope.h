#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Kaleidoscope: folds point-space into the first sector, then mirrors it
// into all other sectors by reflecting rather than just rotating.
class BlockKaleidoscope : public IFxBlock {
public:
    BlockKaleidoscope();

    const char* name()        const override { return "Kaleidoscope"; }
    const char* category()    const override { return "Symmetry"; }
    const char* description() const override {
        return "Folds space like a kaleidoscope. Points are mirrored into all sectors.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_segments_;      // 2..16
    FxParam p_angle_offset_;  // 0..360 deg
    FxParam p_flip_odd_;      // 0=off, 1=on
};

} // namespace idhmfis
