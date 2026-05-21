#pragma once
#include <cstdlib>

namespace idhmfis {

enum class StrobeWaveform {
    Square,
    RampUp,
    RampDown,
    Random,
    GatedEnvelope
};

enum class StrobeMode {
    Hz,
    TempoNote
};

// StrobeSubsystem: independent strobe state machine.
// Tempo note divisors: 1=whole, 2=half, 4=quarter, 8=eighth, 16=sixteenth, etc.
class StrobeSubsystem {
public:
    bool           enabled        = false;
    StrobeMode     mode           = StrobeMode::Hz;
    float          hz             = 5.f;      // 0.1..30 Hz
    int            tempo_note     = 8;        // divisor
    bool           tempo_dotted   = false;
    bool           tempo_triplet  = false;
    float          duty_cycle     = 0.5f;    // 0.01..0.99
    StrobeWaveform waveform       = StrobeWaveform::Square;
    float          phase_offset   = 0.f;     // 0..1

    // Advance internal phase by dt * effective_hz; refresh random value on phase wrap.
    void  tick(float dt, float bpm, bool beat_now);

    // Returns 0..1 brightness multiplier for current phase.
    float current_level() const;

    // Returns true if brightness > 0.
    bool  is_on() const;

    // Force one frame fully on (FLASH key behaviour).
    void  trigger_macro();

private:
    float phase_    = 0.f;
    bool  macro_    = false;
    float rand_val_ = 1.f;

    float effective_hz(float bpm) const;
};

} // namespace idhmfis
