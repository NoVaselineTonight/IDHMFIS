#pragma once
// timeline_types.h — Data model for the IDHMFIS Timeline + Timecode system.
// All types are plain-old-data-friendly for easy snapshotting and serialization.

#include "../input/timecode.h"
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Timecode display helper
// ─────────────────────────────────────────────────────────────────────────────
inline std::string tc_to_string(int64_t frames, SmpteRate fps)
{
    if (frames < 0) frames = 0;
    int fps_int = smpte_max_frames(fps);
    if (fps_int <= 0) fps_int = 25;
    int fr      = static_cast<int>(frames % fps_int);
    int64_t s   = frames / fps_int;
    int sec     = static_cast<int>(s % 60);
    int min     = static_cast<int>((s / 60) % 60);
    int hr      = static_cast<int>(s / 3600);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d:%02d", hr, min, sec, fr);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  AudioTrackDef — audio file attached to a timeline
// ─────────────────────────────────────────────────────────────────────────────
struct AudioTrackDef {
    std::string file_path;             // absolute path to audio file
    int64_t     offset_frames = 0;     // timeline frame where audio t=0 aligns
    float       volume        = 1.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineEventType
// ─────────────────────────────────────────────────────────────────────────────
enum class TimelineEventType : uint8_t {
    CueGo           = 0,
    PlaybackGo      = 1,
    PlaybackActivate= 2,
    PlaybackRelease = 3,
    SetLevel        = 4,
    Flash           = 5,
    Command         = 6,
    Marker          = 7,
    WaitForGo       = 8,
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineEvent — one timed action on a track
// ─────────────────────────────────────────────────────────────────────────────
struct TimelineEvent {
    int64_t              id            = 0;   // unique within the timeline
    int64_t              tc_position   = 0;   // frames from timeline start
    TimelineEventType    type          = TimelineEventType::Marker;
    std::string          target_id;           // playback/cue reference
    std::map<std::string, float> params;      // type-specific float params
    std::string          label;               // human-readable annotation
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineTrack — a lane of events inside a TimelineDef
// ─────────────────────────────────────────────────────────────────────────────
struct TimelineTrack {
    int                      id        = 0;
    std::string              name      = "Track";
    bool                     muted     = false;
    bool                     locked    = false;
    bool                     collapsed = false;
    std::vector<TimelineEvent> events; // sorted ascending by tc_position
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineState — playback state machine for one TimelineDef
// ─────────────────────────────────────────────────────────────────────────────
enum class TimelineState : uint8_t {
    Idle    = 0,
    Armed   = 1,
    Playing = 2,
    Paused  = 3,
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimecodeSourceStatus — color-coding for TC lock state in the UI
// ─────────────────────────────────────────────────────────────────────────────
enum class TimecodeSourceStatus : uint8_t {
    Disabled = 0,  // source not configured / slot is "Internal"
    Present  = 1,  // signal detected but timeline not chasing
    Active   = 2,  // actively chasing / playing
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimecodeSettings — per-slot configuration
// ─────────────────────────────────────────────────────────────────────────────
struct TimecodeSettings {
    TimecodeSource source             = TimecodeSource::Internal;
    SmpteRate      fps                = SmpteRate::Fps25;
    bool           enabled            = false;
    int            jump_detect_frames = 10;   // frames delta considered a jump
    int            freewheel_frames   = 5;    // frames to coast after signal loss
};

// ─────────────────────────────────────────────────────────────────────────────
//  ProjectTimecodeConfig — all TC slot settings for the project
// ─────────────────────────────────────────────────────────────────────────────
struct ProjectTimecodeConfig {
    // Slot names: "Default", "CH1", "CH2", "CH3", "CH4"
    std::map<std::string, TimecodeSettings> slots;

    ProjectTimecodeConfig()
    {
        // Populate default slots
        for (const char* name : {"Default", "CH1", "CH2", "CH3", "CH4"})
            slots[name] = TimecodeSettings{};
        // Default slot uses Internal source by default (free-running)
        slots["Default"].source  = TimecodeSource::Internal;
        slots["Default"].enabled = true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineDef — one timeline document (similar to a cue list)
// ─────────────────────────────────────────────────────────────────────────────
struct TimelineDef {
    std::string              id;               // UUID
    std::string              name             = "Timeline";
    SmpteRate                fps              = SmpteRate::Fps25;
    int64_t                  length_frames    = 0;      // 0 = unbounded
    int64_t                  time_offset_frames = 0;   // subtract from external TC
    std::string              tc_slot          = "Default"; // "Default","CH1"..
    bool                     link_mode        = true;   // true = use internal transport
    bool                     record_armed     = false;

    struct DmxTrigger {
        int     universe  = 0;
        int     channel   = 1;
        uint8_t threshold = 127;
        bool    enabled   = false;
    } dmx_trigger;

    std::vector<TimelineTrack> tracks;

    // ── Audio track ───────────────────────────────────────────────────────────
    std::optional<AudioTrackDef> audio_track;  // nullopt = no audio
    // Pre-computed peak envelope for waveform display.
    // One float per ~4 screen pixels at default zoom (recomputed on file change).
    std::vector<float> audio_peaks;
};

} // namespace idhmfis
