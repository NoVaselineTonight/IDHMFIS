#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Shifts hue of all points in HSV space, with optional time-based animation.
// mode 0: hue shift indexed by position along the path (0..1 per point).
// mode 1: uniform hue shift driven by time.
class BlockColorCycle : public IFxBlock {
public:
    BlockColorCycle();

    const char* name()        const override { return "ColorCycle"; }
    const char* category()    const override { return "Color"; }
    const char* description() const override {
        return "Animates hue shift across points. Mode 0 = by path position, mode 1 = by time.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_hue_offset_;    // 0..1
    FxParam p_saturation_;    // 0..1
    FxParam p_brightness_;    // 0..1
    FxParam p_cycle_speed_;   // 0..4 (cycles/s)
    FxParam p_mode_;          // 0=by_path, 1=by_time

    float anim_hue_ = 0.f;   // accumulated hue offset driven by cycle_speed
};

} // namespace idhmfis
