#include "block_lfo.h"
#include <cmath>
#include <cstdlib>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockLfo::BlockLfo() {
    p_waveform_   = { "waveform",   0.f,  0.f, 0.f,  6.f,  1.f,   "" };
    p_rate_hz_    = { "rate_hz",    1.f,  0.f, 0.01f, 20.f, 0.01f, "Hz" };
    p_amplitude_  = { "amplitude",  1.f,  0.f, 0.f,  1.f,  0.01f, "" };
    p_offset_     = { "offset",     0.f,  0.f, 0.f,  1.f,  0.01f, "" };
    p_phase_      = { "phase",      0.f,  0.f, 0.f,  1.f,  0.01f, "" };
    p_tempo_sync_ = { "tempo_sync", 0.f,  0.f, 0.f,  1.f,  1.f,   "" };
    p_tempo_note_ = { "tempo_note", 8.f,  0.f, 1.f,  32.f, 1.f,   "" };
    p_output_     = { "output",     0.f,  0.f, 0.f,  1.f,  0.001f, "" };

    sh_value_    = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    noise_value_ = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
}

std::vector<FxParam*> BlockLfo::params() {
    return { &p_waveform_, &p_rate_hz_, &p_amplitude_, &p_offset_,
             &p_phase_, &p_tempo_sync_, &p_tempo_note_, &p_output_ };
}

const std::vector<FxParam*> BlockLfo::params() const {
    return { const_cast<FxParam*>(&p_waveform_),
             const_cast<FxParam*>(&p_rate_hz_),
             const_cast<FxParam*>(&p_amplitude_),
             const_cast<FxParam*>(&p_offset_),
             const_cast<FxParam*>(&p_phase_),
             const_cast<FxParam*>(&p_tempo_sync_),
             const_cast<FxParam*>(&p_tempo_note_),
             const_cast<FxParam*>(&p_output_) };
}

void BlockLfo::process(PointBuffer& buf, float dt, const ExprContext& ctx) {
    (void)buf;  // LFO does not modify the buffer

    // Compute effective rate
    float rate = 0.f;
    if (p_tempo_sync_.effective() >= 0.5f) {
        float beat_hz  = ctx.bpm / 60.f;
        int   note     = std::max(1, static_cast<int>(std::round(p_tempo_note_.effective())));
        rate = beat_hz * 4.f / static_cast<float>(note);
    } else {
        rate = p_rate_hz_.effective();
    }

    float advance = dt * rate;
    float prev_phase = internal_phase_;
    internal_phase_  = std::fmod(internal_phase_ + advance, 1.f);
    if (internal_phase_ < 0.f) internal_phase_ += 1.f;

    bool phase_reset = (internal_phase_ < prev_phase);

    // Effective phase with static offset applied
    float p = std::fmod(internal_phase_ + p_phase_.effective(), 1.f);
    if (p < 0.f) p += 1.f;

    const int wf = static_cast<int>(std::round(p_waveform_.effective()));

    float raw = 0.f;
    switch (wf) {
    case 0:  // sine: 0..1 (centred at 0.5)
        raw = 0.5f + 0.5f * std::sin(p * static_cast<float>(M_PI * 2.0));
        break;

    case 1:  // triangle
        if (p < 0.5f)      raw = p * 2.f;
        else               raw = 2.f - p * 2.f;
        break;

    case 2:  // sawtooth rising
        raw = p;
        break;

    case 3:  // ramp (sawtooth falling)
        raw = 1.f - p;
        break;

    case 4:  // square
        raw = (p < 0.5f) ? 1.f : 0.f;
        break;

    case 5:  // S&H — only sample new value on phase reset
        if (phase_reset) {
            sh_value_ = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        }
        raw = sh_value_;
        break;

    case 6:  // noise — new value every tick
        noise_value_ = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
        raw = noise_value_;
        break;

    default:
        raw = 0.f;
        break;
    }

    float out_val = p_offset_.effective() + p_amplitude_.effective() * raw;
    out_val = std::clamp(out_val, p_output_.min_val, p_output_.max_val);
    p_output_.base_value = out_val;
}

} // namespace idhmfis
