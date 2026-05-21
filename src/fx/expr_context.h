#pragma once
#include "../core/types.h"

namespace idhmfis {

// Read-only context snapshot passed to every block on every tick.
struct ExprContext {
    double t        = 0.0;  // show time (seconds)
    double beat     = 0.0;  // beat phase 0..1
    double bar      = 0.0;  // bar phase 0..1
    float  bpm      = 120.f;
    float  rms      = 0.f;
    float  peak     = 0.f;
    float  sub      = 0.f;  // audio sub band
    float  mid      = 0.f;
    float  hi       = 0.f;
    bool   beat_now = false;
    float  midi[128]{};     // MIDI CC values 0..1
    float  dmx[512]{};      // DMX channel values 0..1 (current universe)

    // Per-point expression context
    int    sample_i = 0;    // sample index (0..N-1)
    int    sample_n = 1;    // total sample count

    AudioSnapshot audio;
};

} // namespace idhmfis
