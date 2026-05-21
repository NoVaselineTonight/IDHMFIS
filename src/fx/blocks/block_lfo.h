#pragma once
#include "../fx_block.h"

namespace idhmfis {

// LFO modulator block.
// Does NOT modify PointBuffer. Advances internal phase each tick and writes
// the current output value to the "output" FxParam so the ModulationMatrix
// can route it to any destination.
//
// Waveforms: 0=sine, 1=triangle, 2=sawtooth(rising), 3=ramp(falling),
//            4=square, 5=S&H, 6=noise(per-sample random)
class BlockLfo : public IFxBlock {
public:
    BlockLfo();

    const char* name()        const override { return "LFO"; }
    const char* category()    const override { return "Modulator"; }
    const char* description() const override {
        return "Low-frequency oscillator. Output value is routed via the modulation matrix.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_waveform_;     // 0..6
    FxParam p_rate_hz_;      // 0.01..20
    FxParam p_amplitude_;    // 0..1
    FxParam p_offset_;       // 0..1
    FxParam p_phase_;        // 0..1 (initial phase offset)
    FxParam p_tempo_sync_;   // 0 or 1
    FxParam p_tempo_note_;   // 1,2,4,8,16,32
    FxParam p_output_;       // read by modulation matrix

    float   internal_phase_  = 0.f;
    float   sh_value_        = 0.f;   // sample and hold stored value
    float   noise_value_     = 0.f;   // per-sample noise value
};

} // namespace idhmfis
