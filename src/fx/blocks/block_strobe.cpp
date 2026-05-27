#include "block_strobe.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockStrobe::BlockStrobe() {
    p_enabled_       = { "enabled",       0.f,  0.f, 0.f,  1.f,  1.f,   "" };
    p_hz_            = { "hz",            5.f,  0.f, 0.1f, 30.f, 0.1f,  "Hz" };
    p_duty_cycle_    = { "duty_cycle",    0.5f, 0.f, 0.01f, 0.99f, 0.01f, "" };
    p_waveform_      = { "waveform",      0.f,  0.f, 0.f,  4.f,  1.f,   "" };
    p_phase_offset_  = { "phase_offset",  0.f,  0.f, 0.f,  1.f,  0.01f, "" };
    p_mode_          = { "mode",          0.f,  0.f, 0.f,  1.f,  1.f,   "" };
    p_tempo_note_    = { "tempo_note",    8.f,  0.f, 1.f,  32.f, 1.f,   "" };
    p_tempo_dotted_  = { "tempo_dotted",  0.f,  0.f, 0.f,  1.f,  1.f,   "" };
    p_tempo_triplet_ = { "tempo_triplet", 0.f,  0.f, 0.f,  1.f,  1.f,   "" };
}

std::vector<FxParam*> BlockStrobe::params() {
    return { &p_enabled_, &p_hz_, &p_duty_cycle_, &p_waveform_,
             &p_phase_offset_, &p_mode_, &p_tempo_note_,
             &p_tempo_dotted_, &p_tempo_triplet_ };
}

const std::vector<FxParam*> BlockStrobe::params() const {
    return { const_cast<FxParam*>(&p_enabled_),
             const_cast<FxParam*>(&p_hz_),
             const_cast<FxParam*>(&p_duty_cycle_),
             const_cast<FxParam*>(&p_waveform_),
             const_cast<FxParam*>(&p_phase_offset_),
             const_cast<FxParam*>(&p_mode_),
             const_cast<FxParam*>(&p_tempo_note_),
             const_cast<FxParam*>(&p_tempo_dotted_),
             const_cast<FxParam*>(&p_tempo_triplet_) };
}

void BlockStrobe::sync_params_to_strobe() {
    strobe_.enabled       = (p_enabled_.effective() >= 0.5f);
    strobe_.hz            = p_hz_.effective();
    strobe_.duty_cycle    = p_duty_cycle_.effective();
    strobe_.waveform      = static_cast<StrobeWaveform>(
                                std::clamp(static_cast<int>(std::round(p_waveform_.effective())), 0, 4));
    strobe_.phase_offset  = p_phase_offset_.effective();
    strobe_.mode          = (p_mode_.effective() >= 0.5f) ? StrobeMode::TempoNote : StrobeMode::Hz;
    strobe_.tempo_note    = std::max(1, static_cast<int>(std::round(p_tempo_note_.effective())));
    strobe_.tempo_dotted  = (p_tempo_dotted_.effective() >= 0.5f);
    strobe_.tempo_triplet = (p_tempo_triplet_.effective() >= 0.5f);
}

void BlockStrobe::process(PointBuffer& buf, float dt, const ExprContext& ctx) {
    sync_params_to_strobe();
    strobe_.tick(dt, ctx.bpm, ctx.beat_now);

    if (!strobe_.enabled) return;  // passthrough

    const float level = strobe_.current_level();
    const bool  on    = strobe_.is_on();

    for (LaserPoint& pt : buf) {
        if (!on) {
            pt.r = 0;
            pt.g = 0;
            pt.b = 0;
        } else {
            // L-11: clamp to [0, 255] before casting — modulation can push level > 1.0,
            // making the product exceed 255 and wrap to a wrong uint8_t value.
            pt.r = static_cast<uint8_t>(std::clamp(static_cast<float>(pt.r) * level, 0.f, 255.f));
            pt.g = static_cast<uint8_t>(std::clamp(static_cast<float>(pt.g) * level, 0.f, 255.f));
            pt.b = static_cast<uint8_t>(std::clamp(static_cast<float>(pt.b) * level, 0.f, 255.f));
        }
    }
}

} // namespace idhmfis
