#pragma once
// Cue definition — a single show event containing a generator configuration
// and all its automation data.

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include "../core/types.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Generator type selector
// ─────────────────────────────────────────────────────────────────────────────
enum class GeneratorType {
    Beams,
    Waves,
    Lissajous,
    Tunnel,
    TextScroller,
    Oscilloscope,
    FFTBars,
    Spirograph,
    ParticleField,
    GeometricMorph,
    Ribbon,
    Grid,
    Starburst,
    FanSweep,
    ConeSweep,
    ILDASequence,
    Custom
};

// ─────────────────────────────────────────────────────────────────────────────
//  Single keyframe on a parameter automation track
// ─────────────────────────────────────────────────────────────────────────────
struct KeyframePoint {
    double time  = 0.0;   // seconds within the cue

    float  value = 0.f;

    enum class Interp {
        Linear,
        Step,
        CubicBezier
    } interp = Interp::Linear;

    // Bezier tangent handles (used only when interp == CubicBezier).
    // Stored as tangent-out (cp1) and tangent-in (cp2) in normalised value space.
    float cp1 = 0.f;
    float cp2 = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  A single automation lane driving one parameter
// ─────────────────────────────────────────────────────────────────────────────
struct ParamTrack {
    std::string              param_name;
    std::vector<KeyframePoint> keyframes;

    // --- DMX mapping ---
    bool dmx_mapped  = false;
    int  dmx_channel = 0;    // 1-512
    int  dmx_universe = 0;

    // --- MIDI mapping ---
    bool midi_mapped = false;
    int  midi_cc     = -1;   // -1 = unmapped

    // --- OSC mapping ---
    bool        osc_mapped = false;
    std::string osc_path;

    // --- Audio mapping ---
    // audio_source: "rms", "peak", "bpm", "sub", "mid", "high", "fft:<bin>"
    bool        audio_mapped = false;
    std::string audio_source;

    // Evaluate the track at time t (seconds).
    // Returns the interpolated keyframe value. If no keyframes, returns 0.
    float evaluate(double t) const {
        if (keyframes.empty()) return 0.f;
        if (keyframes.size() == 1) return keyframes[0].value;

        // Clamp to range
        if (t <= keyframes.front().time) return keyframes.front().value;
        if (t >= keyframes.back().time)  return keyframes.back().value;

        // Binary search for the surrounding pair
        auto it = std::lower_bound(keyframes.begin(), keyframes.end(), t,
            [](const KeyframePoint& kf, double time) {
                return kf.time < time;
            });

        if (it == keyframes.begin()) return it->value;

        const KeyframePoint& next = *it;
        const KeyframePoint& prev = *std::prev(it);

        double seg_len = next.time - prev.time;
        if (seg_len <= 0.0) return prev.value;

        float alpha = static_cast<float>((t - prev.time) / seg_len);

        switch (prev.interp) {
            case KeyframePoint::Interp::Step:
                return prev.value;

            case KeyframePoint::Interp::CubicBezier: {
                // Hermite / cubic bezier using tangent handles.
                // cp1 is tangent-out of prev, cp2 is tangent-in of next.
                float t1 = alpha;
                float t2 = t1 * t1;
                float t3 = t2 * t1;
                float h00 =  2.f*t3 - 3.f*t2 + 1.f;
                float h10 =      t3 - 2.f*t2 + t1;
                float h01 = -2.f*t3 + 3.f*t2;
                float h11 =      t3 -     t2;
                float dur = static_cast<float>(seg_len);
                return h00 * prev.value
                     + h10 * dur * prev.cp1
                     + h01 * next.value
                     + h11 * dur * next.cp2;
            }

            case KeyframePoint::Interp::Linear:
            default:
                return prev.value + (next.value - prev.value) * alpha;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Cue — the core show object
// ─────────────────────────────────────────────────────────────────────────────
struct Cue {
    std::string   id;            // UUID v4, stable across sessions
    std::string   name;
    GeneratorType generator = GeneratorType::Beams;
    GeneratorParams params;
    std::string   ilda_path;     // used when generator == ILDASequence

    double   duration  = 8.0;   // seconds; 0 means loop forever
    bool     loop      = true;
    uint32_t color_tag = 0xFF4488FFu; // RGBA, UI colour swatch

    std::vector<ParamTrack> automation;

    // ------------------------------------------------------------------
    // Build a modified copy of this cue with live overrides applied.
    // DMX channels are mapped through automation tracks whose dmx_mapped
    // flag is set. Audio-mapped tracks sample the AudioSnapshot fields.
    // Keyframe values are also applied at time t so generators receive the
    // fully-animated parameter block.
    // ------------------------------------------------------------------
    Cue with_live_params(const DmxUniverse& dmx,
                         const AudioSnapshot& audio,
                         double t,
                         const std::unordered_map<int,float>& midi_cc_map = {},
                         const std::unordered_map<std::string,float>& osc_map = {}) const;
};

} // namespace idhmfis
