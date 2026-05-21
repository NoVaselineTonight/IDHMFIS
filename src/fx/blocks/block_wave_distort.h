#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Sinusoidal mesh distortion.
// axis: 0=X, 1=Y, 2=both
class BlockWaveDistort : public IFxBlock {
public:
    BlockWaveDistort();

    const char* name()        const override { return "WaveDistort"; }
    const char* category()    const override { return "Distortion"; }
    const char* description() const override {
        return "Displaces each point by sin(coord * freq + t * speed) * amplitude.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_axis_;       // 0=X,1=Y,2=both
    FxParam p_amplitude_;  // 0..0.5
    FxParam p_frequency_;  // 1..10 cycles
    FxParam p_speed_;      // Hz
    FxParam p_phase_;      // 0..360 deg

    float anim_t_ = 0.f;
};

} // namespace idhmfis
