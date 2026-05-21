#pragma once
// cuelist_types.h — MagicQ-class cue list type definitions (spec §B7 + §B8)
// No UI, no engine, no platform dependencies beyond the C++20 STL.

#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#include <unordered_map>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  CueNumber — decimal-nested like MagicQ: 1.0, 1.5, 2.0, 2.1, 3.0
//  Stored as integer major + integer minor (minor 0..99 → display 0.00..0.99)
// ─────────────────────────────────────────────────────────────────────────────
struct CueNumber {
    int major = 1;
    int minor = 0;   // 0..99
};

inline bool operator<(CueNumber a, CueNumber b) noexcept {
    if (a.major != b.major) return a.major < b.major;
    return a.minor < b.minor;
}

inline bool operator==(CueNumber a, CueNumber b) noexcept {
    return a.major == b.major && a.minor == b.minor;
}

inline bool operator!=(CueNumber a, CueNumber b) noexcept {
    return !(a == b);
}

inline bool operator<=(CueNumber a, CueNumber b) noexcept {
    return !(b < a);
}

inline bool operator>(CueNumber a, CueNumber b) noexcept {
    return b < a;
}

inline bool operator>=(CueNumber a, CueNumber b) noexcept {
    return !(a < b);
}

// Returns "1.0", "2.5", "10.15" etc.
inline std::string to_string(CueNumber n) {
    return std::to_string(n.major) + "." + std::to_string(n.minor);
}

// ─────────────────────────────────────────────────────────────────────────────
//  PathInterp — fade curve shape applied to fade_in / fade_out segments
// ─────────────────────────────────────────────────────────────────────────────
enum class PathInterp {
    Linear,
    SCurve,
    EaseIn,
    EaseOut,
    EaseInOut,
    SnapLate,
    SnapEarly,
    Expression
};

// ─────────────────────────────────────────────────────────────────────────────
//  FanMode — how split times spread across fixtures in a rig
// ─────────────────────────────────────────────────────────────────────────────
enum class FanMode {
    None,
    FrontBack,
    Even,
    CenterOut,
    EndsOut,
    Random
};

// ─────────────────────────────────────────────────────────────────────────────
//  SplitTimes — per-parameter-class independent timing overrides
//  -1.f means "use the cue's default fade time for that segment"
// ─────────────────────────────────────────────────────────────────────────────
struct SplitTimes {
    float intensity = -1.f;
    float color     = -1.f;
    float position  = -1.f;
    float fx        = -1.f;
    float beam      = -1.f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CueTimingBlock — full MagicQ-style timing for one cue
// ─────────────────────────────────────────────────────────────────────────────
struct CueTimingBlock {
    float      fade_in    = 0.f;   // seconds; 0 = snap
    float      fade_out   = 0.f;   // seconds; 0 = snap
    float      delay_in   = 0.f;   // seconds before fade_in begins
    float      delay_out  = 0.f;   // seconds before fade_out begins
    float      hold       = 0.f;   // seconds at full intensity; 0 = no hold
    float      wait       = 0.f;   // seconds before auto-advance; 0 = HALT (manual)
    PathInterp path       = PathInterp::Linear;
    FanMode    fan        = FanMode::None;
    float      fan_seed   = 0.f;
    SplitTimes split;
    std::string path_expr; // expression string, used when path == Expression
};

// ─────────────────────────────────────────────────────────────────────────────
//  TriggerType — what triggers advancement out of this cue
// ─────────────────────────────────────────────────────────────────────────────
enum class TriggerType {
    Halt,      // manual GO required
    Follow,    // auto-advance after time_s from cue activation
    Wait,      // auto-advance after time_s from cue completion (end of hold)
    Timecode,  // LTC / MTC / ArtNetTC frame number
    MIDI,
    OSC,
    DMX,
    Audio
};

// ─────────────────────────────────────────────────────────────────────────────
//  CueTrigger — all trigger parameters for one cue
// ─────────────────────────────────────────────────────────────────────────────
struct CueTrigger {
    TriggerType type    = TriggerType::Halt;

    // Follow / Wait
    float  time_s       = 0.f;

    // Timecode
    int    tc_frame     = 0;

    // MIDI
    int    midi_ch      = 1;
    int    midi_note    = -1;   // -1 = unused
    int    midi_cc      = -1;
    int    midi_pc      = -1;

    // OSC
    std::string osc_address;

    // DMX
    int      dmx_universe  = 0;
    int      dmx_channel   = 1;
    uint8_t  dmx_threshold = 128;

    // Audio
    std::string audio_trigger;   // "beat", "onset", "sub_threshold",
                                 // "mid_threshold", "hi_threshold"
    float audio_threshold = 0.5f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  TrackingMode — whether this cue participates in LTP tracking
// ─────────────────────────────────────────────────────────────────────────────
enum class TrackingMode {
    Tracking,  // LTP: only explicit overrides are outputted; rest tracked
    CueOnly    // outputs only the parameters stored in this cue entry
};

// ─────────────────────────────────────────────────────────────────────────────
//  LinkMode — what the cue list does after this cue's trigger fires
// ─────────────────────────────────────────────────────────────────────────────
enum class LinkMode {
    Stop,   // halt playback
    Loop,   // loop back to entry 0
    Next,   // advance to the next sequential entry
    Jump    // jump to the cue number given by jump_major / jump_minor
};

// ─────────────────────────────────────────────────────────────────────────────
//  FxStackDiff — sparse diff of FX parameters vs tracked state (legacy, kept
//  for backward compatibility with old project files).
// ─────────────────────────────────────────────────────────────────────────────
struct FxStackDiff {
    std::string json_blob;
};

// ─────────────────────────────────────────────────────────────────────────────
//  GlobalFxType — waveform shapes for the Global Layer FX stack
// ─────────────────────────────────────────────────────────────────────────────
enum class GlobalFxType {
    Sine,       // smooth sine wave
    Square,     // square wave (strobe)
    Saw,        // sawtooth
    Triangle,   // triangle wave
    Flicker,    // random intensity variation
    RampUp,     // linear ramp 0->1 over period
    RampDown,   // linear ramp 1->0
    Bump,       // single gaussian bump
    Chase,      // fast square chase
    Strobe,     // dedicated strobe (square wave, duty cycle shown always)
};

// ─────────────────────────────────────────────────────────────────────────────
//  FxBlendMode — how a global FX entry combines with the running intensity
// ─────────────────────────────────────────────────────────────────────────────
enum class FxBlendMode {
    Add,        // base_intensity + effect (clamped 0..1)
    Subtract,   // base_intensity - effect (clamped 0..1)
    Absolute,   // replace with effect value
    Multiply,   // base * effect
};

// ─────────────────────────────────────────────────────────────────────────────
//  FxDirection — how phase offset is distributed across objects for chasing.
//  "Objects" = laser objects in the frame (lines, dots, etc.).
//  Forward/Backward create a wave that travels across the frame.
//  CentreOut/CentreIn radiate from or collapse to the spatial centre.
//  OddEven alternates phase 0/0.5 for a snap-back look.
//  Random gives each object a stable but unique phase offset.
// ─────────────────────────────────────────────────────────────────────────────
enum class FxDirection {
    Sync,       // all objects identical phase (no chase — classic behaviour)
    Forward,    // phase 0→1 across objects (wave travels through them in order)
    Backward,   // phase 1→0 (wave reverses)
    CentreOut,  // centre objects lead, edge objects trail (expands outward)
    CentreIn,   // edge objects lead, centre trails (collapses inward)
    OddEven,    // alternating 0 / 0.5 phase — snappy checkerboard look
    Random,     // random stable phase per object (same seed each frame)
};

// ─────────────────────────────────────────────────────────────────────────────
//  GlobalFxEntry — one entry in the Global Layer FX stack
// ─────────────────────────────────────────────────────────────────────────────
struct GlobalFxEntry {
    GlobalFxType type      = GlobalFxType::Sine;
    FxBlendMode  blend     = FxBlendMode::Absolute;
    float        rate      = 1.f;   // Hz
    float        depth     = 1.f;   // amplitude 0..1
    float        duty      = 0.5f;  // duty cycle (for Square/Chase)
    float        offset    = 0.f;   // phase offset 0..1
    FxDirection  direction = FxDirection::Sync;
    float        dir_width = 1.f;   // Spread: phase spread (0=sync, 1=full wave)
    int          parts     = 1;     // wave cycles across group (1=one wave, 2=two, ...)
    int          segs      = 1;     // objects per segment (N adjacent share the same phase)
    float        width     = 1.f;   // Width gate: fraction of objects ON at any moment
    bool         enabled   = true;
    float        crossfade = 0.f;   // 0=snap  1=full smooth (low-pass)
};

// ─────────────────────────────────────────────────────────────────────────────
//  FrameFxType — geometric/color FX applied per-frame to the point buffer
// ─────────────────────────────────────────────────────────────────────────────
enum class FrameFxType {
    PanX,        // sine pan left/right
    PanY,        // sine pan up/down
    Rotate,      // sine rotation
    Scale,       // sine scale up/down
    BounceX,     // |sin| bounce (always positive X)
    BounceY,     // |sin| bounce vertical
    ShakeX,      // random X jitter
    ShakeY,      // random Y jitter
    ColorCycle,  // hue rotation over time
    ColorPulse,  // brightness pulse
    RainbowTrail,// rainbow color along path
    Spiral,      // spiral movement (pan X + pan Y 90 apart)
    Col2,        // 2-color: square-wave between object color and hue-shifted version (depth=shift 0..1)
    Col3,        // 3-color: snap cycle through 3 hues 120° apart at current saturation/value
    ColFlick,    // color flicker: random per-channel intensity variation (fire/glitch)
    Strobe,      // rhythmic blanking: points blanked when phase > depth (depth = duty 0..1)
    Col4,           // 4-color cycle
    Col5,           // 5-color cycle
    RotateContinuous, // continuous unidirectional rotation
};

// ─────────────────────────────────────────────────────────────────────────────
//  FrameFxEntry — one entry in the FX Layer stack
// ─────────────────────────────────────────────────────────────────────────────
struct FrameFxEntry {
    FrameFxType type      = FrameFxType::PanX;
    float       rate      = 1.f;   // Hz
    float       depth     = 0.1f;  // Size: amplitude (normalized coords for geometry)
    float       offset    = 0.f;   // phase 0..1
    float       size      = 1.f;   // geometry scale before FX (0..4, 1=normal)
    float       spread    = 1.f;   // X-axis stretch before FX (0..4, 1=normal)
    FxDirection direction = FxDirection::Sync;
    float       dir_width = 1.f;   // Spread: phase spread (0=sync, 1=full wave)
    int         parts     = 1;     // wave cycles across group (1=one wave, 2=two, ...)
    int         segs      = 1;     // objects per segment (N adjacent share the same phase)
    float       width     = 1.f;   // Width gate: fraction of objects ON at any moment
    bool        enabled   = true;
    float       crossfade = 0.f;   // 0=snap  1=full smooth
    // Custom colors for color FX (Col2, Col3, ColFlick, ColorCycle, ColorPulse, RainbowTrail)
    float col_a_r = 1.f, col_a_g = 0.f, col_a_b = 0.f;  // Color A (default red)
    float col_b_r = 0.f, col_b_g = 0.f, col_b_b = 1.f;  // Color B (default blue)
    float col_c_r = 0.f, col_c_g = 1.f, col_c_b = 0.f;  // Color C (default green)
    float col_d_r = 1.f, col_d_g = 1.f, col_d_b = 0.f;  // Color D (default yellow)
    float col_e_r = 1.f, col_e_g = 0.f, col_e_b = 1.f;  // Color E (default magenta)
    bool  use_custom_colors = false;
};

// ─────────────────────────────────────────────────────────────────────────────
//  FxLayer — the Frame FX stack for one cue (replaces FxStackDiff)
// ─────────────────────────────────────────────────────────────────────────────
struct FxLayer {
    std::vector<FrameFxEntry> fx;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ChaserStep — one step in a chaser cue's step list
//  cue_ref_idx: 0-based index into the cue array (same as cmd::ActivateCue slot)
// ─────────────────────────────────────────────────────────────────────────────
struct ChaserStep {
    int   cue_ref_idx = -1;   // which cue to show during this step
    float hold_s      = -1.f; // hold time override; -1 = use chaser global_hold
    float xfade_s     = -1.f; // crossfade time override; -1 = use chaser global_xfade
    bool  beat_sync   = false; // advance on beat instead of time
    int   beat_div    = 4;    // 1=bar(4beats), 2=half, 4=quarter, 8=8th, 16=16th
};

// ─────────────────────────────────────────────────────────────────────────────
//  GlobalLayer — master dim + FX stack for one cue
// ─────────────────────────────────────────────────────────────────────────────
struct GlobalLayer {
    float global_dim   = 1.f;   // master dim (0..1) applied BEFORE fx stack
    // Legacy fields (kept for backward compat with old project files):
    float strobe_rate  = 0.f;   // Hz; 0 = no strobe
    float strobe_duty  = 0.5f;  // 0..1
    // Geometry modifiers applied to output point coordinates:
    float size         = 1.f;   // uniform geometry scale (0..4, 1=normal)
    float spread       = 1.f;   // X-axis stretch (0..4, 1=normal)
    // FX stack:
    std::vector<GlobalFxEntry> fx;
};

// ─────────────────────────────────────────────────────────────────────────────
//  LaserObjectType — types of objects in the Keyframe layer
// ─────────────────────────────────────────────────────────────────────────────
enum class LaserObjectType {
    Line,     // polyline (2+ control points)
    Dot,      // single point (rendered as filled circle)
    Bezier,   // cubic bezier (4 control points per segment)
    Arc,      // arc (centre, radius, start_angle, end_angle)
    Circle,   // circle (centre + radius control point)
    Text      // text label at a position
};

// ─────────────────────────────────────────────────────────────────────────────
//  LaserObjectPoint — one control point with position
// ─────────────────────────────────────────────────────────────────────────────
struct LaserObjectPoint {
    float x = 0.f;
    float y = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  LaserObject — a single drawable object in the Keyframe layer
//  Objects have stable IDs used to morph between cues.
// ─────────────────────────────────────────────────────────────────────────────
struct LaserObject {
    uint64_t         id         = 0;   // stable ID for morphing across cues
    LaserObjectType  type       = LaserObjectType::Line;
    std::vector<LaserObjectPoint> pts; // control points (normalised -1..1)
    float            r          = 0.f; // color red   0..1
    float            g          = 0.898f;
    float            b          = 1.f;
    float            thickness  = 1.f; // line thickness (px units)
    float            size       = 0.05f; // dot radius / arc radius / circle radius
    std::string      text;              // text content (for Text type)
    bool             selected   = false; // UI-only, not serialised
};

// ─────────────────────────────────────────────────────────────────────────────
//  KeyframeLayer — list of LaserObjects at a specific cue
// ─────────────────────────────────────────────────────────────────────────────
struct KeyframeLayer {
    std::vector<LaserObject> objects;
    // Transition settings FROM this cue TO the next cue
    float     morph_time     = 0.5f;   // seconds to morph matching objects
    float     fade_in_time   = 0.3f;   // seconds for new objects to appear
    float     fade_out_time  = 0.3f;   // seconds for removed objects to disappear
    // PathInterp already defined in this file
    PathInterp morph_curve   = PathInterp::SCurve;
    // Symmetry — applied at PointBuffer level in render_keyframe_layer
    // 0=None, 1=MirrorX, 2=MirrorY, 3=MirrorXY, 4=Radial2, 5=Radial3, 6=Radial4,
    // 7=Radial6, 8=Radial8, 9=Radial12
    int       symmetry_mode  = 0;
    float     sym_cx         = 0.f;
    float     sym_cy         = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  FullCueEntry — all per-cue data stored in a CueList (spec §B7.3)
// ─────────────────────────────────────────────────────────────────────────────
struct FullCueEntry {
    CueNumber       number;
    std::string     cue_id;       // references Project::cues[x].id
    std::string     name;
    std::string     comment;
    uint32_t        color_tag   = 0xFF4488FFu;  // RGBA UI swatch

    CueTimingBlock  timing;
    CueTrigger      trigger;
    TrackingMode    tracking    = TrackingMode::Tracking;
    bool            is_block    = false;   // BLOCK cue — terminates backward tracking
    bool            assert_flag = false;   // force re-output even when tracked value unchanged

    FxLayer         fx_layer;

    // Per-stream FX overrides: stream_id -> FxLayer override.
    // If a stream_id is present here, its FxLayer overrides fx_layer for that stream.
    // Streams absent from this map fall back to fx_layer (the global default).
    std::unordered_map<int, FxLayer> per_stream_fx;

    // Per-stream keyframe layers: stream_id -> KeyframeLayer override.
    // If a stream_id is present here, its KeyframeLayer overrides keyframe_layer for that stream.
    // Streams absent from this map fall back to keyframe_layer (the global default).
    std::unordered_map<int, KeyframeLayer> per_stream_kf;

    // Sparse map: param_name -> override value for this cue.
    // Empty means pure tracking cue (no parameter changes).
    std::vector<std::pair<std::string, float>> param_overrides;

    LinkMode        link        = LinkMode::Next;
    int             jump_major  = 0;   // target when link == Jump
    int             jump_minor  = 0;

    // ── Chaser / step-sequence mode ──────────────────────────────────────────
    bool                    is_chaser          = false;
    float                   chaser_global_hold = 0.5f;  // seconds per step
    float                   chaser_global_xfade= 0.0f;  // crossfade between steps
    bool                    chaser_beat_sync   = false; // sync to engine BPM
    int                     chaser_beat_div    = 4;     // 1=bar,2=half,4=qtr,8=8th,16=16th
    std::vector<ChaserStep> chaser_steps;

    // ── 3-layer system ───────────────────────────────────────────────────────
    GlobalLayer    global_layer;
    KeyframeLayer  keyframe_layer;

    // Stream IDs that were active when this cue was recorded.
    // Used by build_frame() to route per-stream content when the playback-level
    // output_stream_ids is empty (the common case for default playbacks).
    std::vector<int> output_stream_ids;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackConfig — per-playback DMX trigger and end-of-cuelist behaviour
// ─────────────────────────────────────────────────────────────────────────────
struct PlaybackConfig {
    enum class DmxMode { Off, OneChannel, TwoChannel };
    DmxMode dmx_mode      = DmxMode::Off;
    int     dmx_universe  = 0;        // 0-based
    int     dmx_channel   = 0;        // 0-based (0-511)
    uint8_t dmx_threshold = 10;       // value above = active

    enum class EndBehavior { Stop, Loop };
    EndBehavior end_behavior = EndBehavior::Loop;

    bool go_at_bpm = false;  // advance cue on each BPM beat
    bool fx_at_bpm = false;  // all FX rates run at BPM tempo (rate=1 = 1 cycle/beat)

    int  keyboard_go_key = 0; // ImGuiKey value for keyboard GO trigger; 0 = none (UI-only, stored for persistence)

    bool  fade_on_first_trigger  = false; // when OFF (default), first cue snaps in immediately on GO
    float first_trigger_fade_s   = 0.f;  // >0: override fade-in seconds for the very first GO trigger
    bool  remember_cuelist_position = false;  // if false, position resets to 0 on stop

    // Output stream routing: which stream IDs this playback renders to.
    // Empty = all streams (default, backward-compatible behaviour).
    std::vector<int> output_stream_ids;
};

// ─────────────────────────────────────────────────────────────────────────────
//  PlaybackDef — one MagicQ-style playback (executor/fader stack)
//  Each playback has an independent cuestack and intensity fader.
// ─────────────────────────────────────────────────────────────────────────────
struct PlaybackDef {
    int                      id       = 0;
    std::string              name;
    std::vector<FullCueEntry> cuelist; // the ordered sequence of cues for this playback
    float                    intensity = 1.f;   // fader level 0..1
    bool                     active   = false;  // is this playback currently GO'd
    bool                     blind    = false;  // blind mode (edit without affecting output)
    // DMX trigger (universe 0-based, channel 1-based, 0=disabled)
    int                      dmx_universe = -1;
    int                      dmx_channel  = -1;
    uint8_t                  dmx_threshold = 64;
    // Extended per-playback configuration (DMX trigger mode + end behavior)
    PlaybackConfig           config;
};

} // namespace idhmfis
