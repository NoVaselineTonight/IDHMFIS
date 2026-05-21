#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Audio envelope follower modulator.
// Reads sub/mid/hi/rms from ExprContext, applies 1-pole IIR attack/release,
// and stores the result in the "output" FxParam for the modulation matrix.
// Does NOT modify PointBuffer.
class BlockEnvelopeFollower : public IFxBlock {
public:
    BlockEnvelopeFollower();

    const char* name()        const override { return "EnvelopeFollower"; }
    const char* category()    const override { return "Modulator"; }
    const char* description() const override {
        return "Audio envelope follower. Output value is routed via the modulation matrix.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_band_;      // 0=sub, 1=mid, 2=hi, 3=rms
    FxParam p_attack_;    // 0.001..1.0 s
    FxParam p_release_;   // 0.001..2.0 s
    FxParam p_gain_;      // 0..4
    FxParam p_offset_;    // 0..1
    FxParam p_output_;    // read by modulation matrix

    float envelope_ = 0.f;
};

} // namespace idhmfis
