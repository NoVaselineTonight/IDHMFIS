#pragma once
// IDHMFIS — Custom ImGui widgets
// All widgets follow the ImGui return-value convention:
// returns true when the user has interacted and the value has changed.

#include "imgui.h"
#include "../core/types.h"
#include <string>
#include <vector>
#include <functional>

namespace idhmfis {
    struct UIState;
    struct CueInfo;
    struct Keyframe;
}

namespace idhmfis::widgets {

// ─────────────────────────────────────────────────────────────────────────────
//  FaderStack
//  Vertical stack of N linear DMX faders (0–255).
//  channels:  pointer to uint8_t array of length n
//  read_only: disables drag interaction
//  Returns true if any channel changed.
// ─────────────────────────────────────────────────────────────────────────────
bool FaderStack(const char* id,
                uint8_t*   channels,
                int        n,
                float      fader_width  = 28.f,
                float      fader_height = 120.f,
                bool       read_only    = false);

// ─────────────────────────────────────────────────────────────────────────────
//  XYPad
//  2D joystick pad. x, y ∈ [-1, 1].
//  size: total size of the pad widget.
// ─────────────────────────────────────────────────────────────────────────────
bool XYPad(const char* id, float* x, float* y,
           ImVec2 size = {120.f, 120.f});

// ─────────────────────────────────────────────────────────────────────────────
//  ColorWheel
//  HSV colour picker with outer hue ring + inner SV square + brightness bar.
//  colour: float[4] RGBA (modified in-place)
// ─────────────────────────────────────────────────────────────────────────────
bool ColorWheel(const char* id, float colour[4], float radius = 70.f);

// ─────────────────────────────────────────────────────────────────────────────
//  TransportBar
//  Draws play/pause/stop buttons, BPM display, master intensity slider,
//  status dots, fps counter, project name, and save indicator in one call.
//  All state pointers must be non-null.
// ─────────────────────────────────────────────────────────────────────────────
void TransportBar(bool*       playing,
                  bool*       paused,
                  float*      bpm,
                  float*      master_intensity,
                  bool        ndi_streaming,
                  bool        dac_connected,
                  float       engine_fps,
                  const char* project_name,
                  bool        project_dirty,
                  std::function<void()> on_play,
                  std::function<void()> on_pause,
                  std::function<void()> on_stop,
                  std::function<void()> on_save);

// ─────────────────────────────────────────────────────────────────────────────
//  CueCard
//  Single cue card in the cue library grid (120×80 px).
//  Returns true if the card was left-clicked (select).
//  on_context_menu: called when right-click menu item chosen.
// ─────────────────────────────────────────────────────────────────────────────
bool CueCard(const CueInfo& cue,
             bool           selected,
             std::function<void(const char* action)> on_context_menu = nullptr);

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineRuler
//  Horizontal bar ruler with beat markers. Draws into current window.
//  playhead_s: current time position (draws vertical line)
//  width: total ruler width in pixels
//  height: ruler height
//  pixels_per_second: zoom level
//  bpm: used to draw beat ticks
//  Returns new playhead time if user clicked to seek, else < 0.
// ─────────────────────────────────────────────────────────────────────────────
double TimelineRuler(const char* id,
                     double playhead_s,
                     double total_s,
                     float  width,
                     float  height,
                     float  pixels_per_second,
                     float  bpm);

// ─────────────────────────────────────────────────────────────────────────────
//  KeyframeTrack
//  Draws a horizontal track with keyframe dots. Drag to move.
//  keyframes: vector of Keyframe (modified in place)
//  Returns true if any keyframe moved.
// ─────────────────────────────────────────────────────────────────────────────
bool KeyframeTrack(const char*           id,
                   std::vector<Keyframe>& keyframes,
                   double                playhead_s,
                   float                 pixels_per_second,
                   float                 width,
                   float                 height = 24.f,
                   float                 value_min = 0.f,
                   float                 value_max = 1.f);

// ─────────────────────────────────────────────────────────────────────────────
//  NDIStatusDot
//  Animated pulsing dot. Green = streaming, grey = idle.
//  Call once per frame.
// ─────────────────────────────────────────────────────────────────────────────
void NDIStatusDot(bool streaming, float radius = 5.f);

// ─────────────────────────────────────────────────────────────────────────────
//  BeamThicknessSlider
//  Log-scale slider for beam width 0.1–20 px.
//  value: current thickness (modified in place, pixels)
// ─────────────────────────────────────────────────────────────────────────────
bool BeamThicknessSlider(const char* label, float* value, float width = 180.f);

// ─────────────────────────────────────────────────────────────────────────────
//  LatencyMeter
//  Rolling histogram of the last N ArtNet round-trip latency samples.
//  history: ring-buffer of floats (milliseconds)
//  n:       length of history array
//  idx:     current write index into ring buffer
// ─────────────────────────────────────────────────────────────────────────────
void LatencyMeter(const char* id,
                  const float* history,
                  int          n,
                  int          idx,
                  float        p99_ms,
                  ImVec2       size = {120.f, 32.f});

} // namespace idhmfis::widgets
