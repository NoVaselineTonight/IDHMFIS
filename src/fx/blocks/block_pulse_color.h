#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Pulses the pattern colour between two colours over time.
// waveform: 0=sine, 1=square, 2=saw
class BlockPulseColor : public IFxBlock {
public:
    BlockPulseColor();

    const char* name()        const override { return "PulseColor"; }
    const char* category()    const override { return "Color"; }
    const char* description() const override {
        return "Interpolates pattern colour between two colours at a given rate (sine/square/saw).";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_color_a_r_, p_color_a_g_, p_color_a_b_;
    FxParam p_color_b_r_, p_color_b_g_, p_color_b_b_;
    FxParam p_speed_;      // Hz
    FxParam p_waveform_;   // 0=sine, 1=square, 2=saw
    FxParam p_sync_beat_;  // 0/1

    float anim_t_ = 0.f;
};

} // namespace idhmfis
