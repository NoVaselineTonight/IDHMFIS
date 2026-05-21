#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Lissajous-style pendulum translation.
// With freq_x=1, freq_y=2 and phase_offset=90 you get a figure-8.
class BlockPendulum : public IFxBlock {
public:
    BlockPendulum();

    const char* name()        const override { return "Pendulum"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Lissajous-style translation. freq_x=1, freq_y=2 gives a figure-8.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_amplitude_x_;   // 0..1
    FxParam p_amplitude_y_;   // 0..1
    FxParam p_freq_x_;        // Hz 0..10
    FxParam p_freq_y_;        // Hz 0..10
    FxParam p_phase_offset_;  // 0..360 deg

    float anim_t_ = 0.f;
};

} // namespace idhmfis
