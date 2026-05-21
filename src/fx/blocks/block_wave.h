#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Wave geometry distortion: offsets each point by a waveform along one or both axes.
class BlockWave : public IFxBlock {
public:
    BlockWave();

    const char* name()        const override { return "Wave"; }
    const char* category()    const override { return "Geometry"; }
    const char* description() const override {
        return "Applies sinusoidal/sawtooth/triangle/square wave distortion to point positions.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_amplitude_;   // 0..2
    FxParam p_frequency_;   // 0.1..20
    FxParam p_phase_;       // 0..tau
    FxParam p_axis_;        // 0=X, 1=Y, 2=Both
    FxParam p_waveform_;    // 0=sine, 1=sawtooth, 2=triangle, 3=square

    // Accumulated phase for time-based animation
    float anim_phase_ = 0.f;
};

} // namespace idhmfis
