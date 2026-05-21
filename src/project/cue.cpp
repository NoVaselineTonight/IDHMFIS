// cue.cpp — Implementation of Cue::with_live_params
#include "cue.h"
#include <cstring>
#include <unordered_map>

namespace idhmfis {

// Helper: resolve audio_source string to a float from AudioSnapshot
static float resolve_audio(const std::string& source, const AudioSnapshot& audio) {
    if (source == "rms")    return audio.rms;
    if (source == "peak")   return audio.peak;
    if (source == "bpm")    return audio.bpm / 300.f; // normalise (max 300 BPM)
    if (source == "sub")    return audio.sub_band;
    if (source == "mid")    return audio.mid_band;
    if (source == "high")   return audio.high_band;

    // fft:<bin>  e.g. "fft:128"
    if (source.rfind("fft:", 0) == 0) {
        try {
            int bin = std::stoi(source.substr(4));
            if (bin >= 0 && bin < AudioSnapshot::kFFTBins)
                return audio.fft[static_cast<size_t>(bin)];
        } catch (...) {}
    }
    return 0.f;
}

// Helper: write a float into the appropriate GeneratorParams field by name.
// Returns true if the name was recognised.
static bool apply_param(GeneratorParams& p, const std::string& name, float v) {
    if (name == "speed")      { p.speed     = v; return true; }
    if (name == "scale")      { p.scale     = v; return true; }
    if (name == "density")    { p.density   = v; return true; }
    if (name == "param_a")    { p.param_a   = v; return true; }
    if (name == "param_b")    { p.param_b   = v; return true; }
    if (name == "param_c")    { p.param_c   = v; return true; }
    if (name == "rotation")   { p.rotation  = v; return true; }
    if (name == "pan")        { p.pan       = v; return true; }
    if (name == "tilt")       { p.tilt      = v; return true; }
    if (name == "zoom")       { p.zoom      = v; return true; }
    if (name == "intensity")  { p.intensity = v; return true; }
    if (name == "color_a.r")  { p.color_a.r = v; return true; }
    if (name == "color_a.g")  { p.color_a.g = v; return true; }
    if (name == "color_a.b")  { p.color_a.b = v; return true; }
    if (name == "color_a.a")  { p.color_a.a = v; return true; }
    if (name == "color_b.r")  { p.color_b.r = v; return true; }
    if (name == "color_b.g")  { p.color_b.g = v; return true; }
    if (name == "color_b.b")  { p.color_b.b = v; return true; }
    if (name == "color_b.a")  { p.color_b.a = v; return true; }
    if (name == "blanked")    { p.blanked   = (v >= 0.5f); return true; }
    if (name == "point_count"){ p.point_count = static_cast<int>(v); return true; }
    return false;
}

Cue Cue::with_live_params(const DmxUniverse& dmx,
                           const AudioSnapshot& audio,
                           double t,
                           const std::unordered_map<int,float>& midi_cc_map,
                           const std::unordered_map<std::string,float>& osc_map) const {
    Cue out = *this; // shallow copy — params will be overwritten below

    for (const ParamTrack& track : automation) {
        float val = track.evaluate(t);

        // Keyframe value is the baseline; live inputs may override it.
        // Priority: audio > DMX/MIDI > keyframe animation.

        if (track.dmx_mapped &&
            track.dmx_channel >= 1 && track.dmx_channel <= 512 &&
            track.dmx_universe == 0) // only universe 0 accessible from snapshot
        {
            val = dmx.norm(track.dmx_channel);
        }

        if (track.audio_mapped && !track.audio_source.empty()) {
            val = resolve_audio(track.audio_source, audio);
        }

        if (track.midi_mapped && track.midi_cc >= 0) {
            auto it = midi_cc_map.find(track.midi_cc);
            if (it != midi_cc_map.end())
                val = it->second;
        }

        if (track.osc_mapped && !track.osc_path.empty()) {
            auto it = osc_map.find(track.osc_path);
            if (it != osc_map.end())
                val = it->second;
        }

        apply_param(out.params, track.param_name, val);
    }

    return out;
}

} // namespace idhmfis
