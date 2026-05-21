#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Applies a sawtooth wave to the X or Y position of all points: ramps linearly
// from +amplitude to -amplitude then snaps back (hard reset).
class BlockSaw : public IFxBlock {
public:
    BlockSaw();

    const char* name()        const override { return "Saw"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Sawtooth pan/tilt: ramps from side A to side B then snaps back.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_axis_;       // 0=Pan/X, 1=Tilt/Y
    FxParam p_speed_;      // 0.1..10 Hz
    FxParam p_amplitude_;  // 0..1
    FxParam p_offset_;     // -1..1
    FxParam p_phase_;      // 0..360 deg

    float anim_phase_ = 0.f;
};

} // namespace idhmfis
