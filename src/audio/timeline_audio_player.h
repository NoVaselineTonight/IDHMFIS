#pragma once
// timeline_audio_player.h — Thin wrapper around miniaudio for timeline
// synchronized audio playback.  One instance lives in ShowEngine::Impl and is
// driven by the engine command handlers (SetTimelineAudio, TimelinePlay, etc.).
//
// Thread-safety: all public methods must be called from the engine thread only.

#include <string>
#include <vector>

namespace idhmfis {

// Decode any audio file (WAV/MP3/FLAC/OGG) to a peak envelope.
// Returns one float per kBucket samples at kDecodeRate Hz mono.
// Returns empty if the file cannot be opened or decoded.
std::vector<float> audio_compute_peaks(const std::string& path);

// Number of peaks produced per second of audio by audio_compute_peaks().
// Use this to convert a frame position to a peak array index:
//   peak_idx = (frame / fps_hz) * kAudioPeakRateHz
// where fps_hz is the integer frames-per-second of the timeline (e.g. 25).
static constexpr double kAudioPeakRateHz = 22050.0 / 100.0;  // 220.5 peaks/s

class TimelineAudioPlayer {
public:
    TimelineAudioPlayer();
    ~TimelineAudioPlayer();

    // Load an audio file.  Returns true on success.
    // Unloads any previously loaded sound first.
    bool load(const std::string& path);

    // Unload the current sound and release resources.
    void unload();

    // Start (or restart) playback from the given position in seconds.
    void play_from(double seconds);

    // Pause playback (position is preserved).
    void pause();

    // Stop playback and reset position to 0.
    void stop();

    // Set playback volume (0.0 = silent, 1.0 = full).
    void set_volume(float v);

    // Returns true when a sound is loaded and currently playing.
    bool is_playing() const;

    // Returns the current playback position in seconds.
    double position_seconds() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace idhmfis
