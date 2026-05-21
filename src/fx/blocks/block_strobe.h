#pragma once
#include "../fx_block.h"
#include "../strobe.h"

namespace idhmfis {

// Strobe block: wraps StrobeSubsystem and exposes its parameters as FxParams.
// Multiplies all point colours by strobe brightness, blanks when strobe is off.
class BlockStrobe : public IFxBlock {
public:
    BlockStrobe();

    const char* name()        const override { return "Strobe"; }
    const char* category()    const override { return "Time"; }
    const char* description() const override {
        return "Hardware-style strobe. Multiplies point brightness by strobe level each frame.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

    // Direct access to the subsystem for FLASH / trigger from UI
    StrobeSubsystem& strobe() { return strobe_; }

private:
    StrobeSubsystem strobe_;

    // FxParams mirror the strobe subsystem state
    FxParam p_enabled_;        // 0 or 1
    FxParam p_hz_;             // 0.1..30
    FxParam p_duty_cycle_;     // 0.01..0.99
    FxParam p_waveform_;       // 0..4
    FxParam p_phase_offset_;   // 0..1
    FxParam p_mode_;           // 0=Hz, 1=TempoNote
    FxParam p_tempo_note_;     // 1,2,4,8,16,32
    FxParam p_tempo_dotted_;   // 0 or 1
    FxParam p_tempo_triplet_;  // 0 or 1

    void sync_params_to_strobe();
};

} // namespace idhmfis
