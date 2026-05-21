#include "strobe.h"
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace idhmfis {

float StrobeSubsystem::effective_hz(float bpm) const {
    if (mode == StrobeMode::Hz) {
        return std::clamp(hz, 0.1f, 30.f);
    }

    // TempoNote: BPM / divisor, with dotted (+50%) and triplet (*2/3) modifiers
    float beat_hz = bpm / 60.f;   // beats per second (quarter note reference)
    float note_hz = beat_hz * 4.f / static_cast<float>(tempo_note > 0 ? tempo_note : 1);
    if (tempo_dotted)   note_hz *= 1.5f;
    if (tempo_triplet)  note_hz *= (2.f / 3.f);
    return std::clamp(note_hz, 0.01f, 500.f);
}

void StrobeSubsystem::tick(float dt, float bpm, bool /*beat_now*/) {
    if (!enabled) {
        phase_ = 0.f;
        macro_ = false;
        return;
    }

    // Clear one-shot macro flag after it has been exposed for one tick
    macro_ = false;

    float eff_hz  = effective_hz(bpm);
    float advance = dt * eff_hz;
    float prev    = phase_;
    phase_ = std::fmod(phase_ + advance, 1.f);
    if (phase_ < 0.f) phase_ += 1.f;

    // Detect phase wrap for S&H / Random waveform
    if (waveform == StrobeWaveform::Random && phase_ < prev) {
        rand_val_ = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX);
    }
}

float StrobeSubsystem::current_level() const {
    if (macro_) return 1.f;
    if (!enabled) return 1.f;  // passthrough when strobe off

    // Apply phase offset
    float p = std::fmod(phase_ + phase_offset, 1.f);
    if (p < 0.f) p += 1.f;

    float dc = std::clamp(duty_cycle, 0.01f, 0.99f);

    switch (waveform) {
    case StrobeWaveform::Square:
        return (p < dc) ? 1.f : 0.f;

    case StrobeWaveform::RampUp:
        // Within duty window, ramp 0->1; outside = 0
        if (p < dc) return p / dc;
        return 0.f;

    case StrobeWaveform::RampDown:
        // Within duty window, ramp 1->0; outside = 0
        if (p < dc) return 1.f - (p / dc);
        return 0.f;

    case StrobeWaveform::Random:
        return (p < dc) ? rand_val_ : 0.f;

    case StrobeWaveform::GatedEnvelope:
        // Smooth half-sine envelope gated by duty cycle
        if (p < dc) {
            float t = p / dc;  // 0..1 within gate
            return std::sin(t * 3.14159265f);
        }
        return 0.f;

    default:
        break;
    }
    return 1.f;
}

bool StrobeSubsystem::is_on() const {
    return current_level() > 0.f;
}

void StrobeSubsystem::trigger_macro() {
    macro_ = true;
}

} // namespace idhmfis
