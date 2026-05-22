#pragma once
// Show Engine — the central 1000 Hz tick loop.
// Reads input queues, evaluates cue stack, calls generators, pushes to RenderBus.
// Single-threaded; all writes to engine state go through CommandQueue.

#include "types.h"
#include "render_bus.h"
#include "spsc_queue.h"
#include "timer.h"
#include "../cuelist/cuelist.h"
#include "../fx/fx_engine.h"
#include "../fx/expr_context.h"
#include "../render/point_optimizer.h"
#include "../render/ndi_sender.h"
#include "../render/laser_rasterizer.h"
#include "../input/timecode.h"
#include "../input/midi_learn.h"
#include "../timeline/timeline_types.h"
#include "../timeline/timeline_engine.h"
#include "../audio/timeline_audio_player.h"
#include "../input/artnet_sender.h"
#include "../dac/dac_registry.h"
#include "../zones/zone_manager.h"
#include "../safety/safety_manager.h"
#include "../project/palette.h"
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <thread>
#include <variant>

namespace idhmfis {

// Forward declarations
class IGenerator;
class AudioAnalyzer;
struct Project;

// ─────────────────────────────────────────────────────────────────────────────
//  OutputStreamDef — typed output descriptor for multi-head output buses.
//  Each patched output (LaserStream, NDI, HDMI) gets one entry.
//  Carried by cmd::SetOutputPatch and stored as stream_defs_ in ShowEngine.
// ─────────────────────────────────────────────────────────────────────────────
struct OutputStreamDef {
    int              id          = -1;
    std::string      name;
    OutputStreamType type        = OutputStreamType::Laser;
    bool             enabled     = true;

    // DAC config (for Laser type)
    std::string      dac_type    = "helios";  // "helios", "etherdream", "laserdock", "emulated"
    std::string      dac_address;
    int              point_rate  = 30000;

    // NDI config (for Ndi type)
    std::string      ndi_source_name;

    // HDMI config (for Hdmi type)
    std::string      hdmi_title;

    // Per-output processing
    OutputTransform      transform;
    OutputSafetyConfig   safety;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Commands sent from UI thread to engine thread
// ─────────────────────────────────────────────────────────────────────────────
namespace cmd {

struct Play       {};
struct Stop       {};
struct Pause      {};

struct SetMasterIntensity { float value; };
struct SetPointRate       { int   pps;   };
struct SetBPM             { float bpm;   };

struct ActivateCue {
    int   slot;        // 0-based cue slot
    float fade_time;   // seconds
};

struct DeactivateCue {
    int   slot;
    float fade_time;
};

struct SetGeneratorParam {
    int          cue_slot;
    std::string  param_name;
    float        value;
};

struct LoadProject {
    std::shared_ptr<Project> project;
};

struct SetBeamThickness { float px; };
struct SetBloomRadius   { float px; };
struct SetHazeDensity   { float d;  };
struct SetExposure      { float e;  };

struct Seek         { double time_s; };  // jump playhead to time
struct GoNext       {};                   // advance to next cue list entry
struct GoPrev       {};                   // go back to previous entry
struct SetLoopEnd   { double time_s; };  // set loop region end

// Cue list playback controls (MagicQ-class CueList state machine)
struct CueListGo   {};
struct CueListBack {};
struct CueListJump { int major = 0; int minor = 0; };

// FX engine per-cue controls
struct SetFxParam    { int cue_idx; int fx_slot; std::string param; float value; };
struct SetFxEnabled  { int cue_idx; int fx_slot; bool enabled; };
struct SetFxBypassed { int cue_idx; int fx_slot; bool bypassed; };
struct SetFxWet      { int cue_idx; int fx_slot; float wet; };

// §Macro: trigger a macro slot (stub — storage only for now)
struct TriggerMacro  { int slot; };

// Record mode
struct SetRecordMode { bool active; };
struct RecordCue     { std::string name; float fade_in; float fade_out; };
struct UpdateCue     { int idx; std::string param; float value; };
struct DeleteCue     { int idx; };
struct InsertCue     { int after_idx; FullCueEntry entry; };
struct RenameCue     { int idx; std::string name; };
struct SetCueTiming  { int idx; float fade_in; float fade_out; float delay_in; float hold; };
struct MoveCue       { int from_idx; int to_idx; };

// Zone routing commands
struct SetZone    { Zone zone; };
struct RemoveZone { int id; };

// BAM / Safety commands
struct BamPaint      { int row; int col; int brush_size; uint8_t val; };
struct BamClear      {};
struct BamSetEnabled { bool enabled; };
struct ResetScanFail {};         // operator acknowledges scan-fail and clears the latch
struct SetScanFailEnabled { bool enabled; }; // enable/disable scan-fail protection

// Quick Show commands
struct QuickShowTrigger { int page; int row; int col; };
struct QuickShowAssign  { int page; int row; int col; int cue_idx; };
struct QuickShowClear   { int page; int row; int col; };

// LivePRO commands
struct LiveProXY    { float x; float y; };
struct LiveProScale { float scale; };
struct LiveProSpeed { float speed; };
struct LiveProBeat  { int slot; };

// MIDI learn commands
struct SetMidiBinding    { MidiBinding binding; };
struct RemoveMidiBinding { std::string target; };
struct ClearMidiBindings {};

// Palette selection commands
struct SetActiveColor    { int slot; };   // selects a palette color for next drawn shape
struct SetActivePosition { int slot; };  // positions current cue at palette XY

// Manual DMX output patching (from Inspector faders)
struct PatchDmxChannel { int universe; int channel; uint8_t value; };

// ArtNet DMX output configuration
struct SetArtNetOutput {
    std::string ip;
    int         universe_offset = 0;
    bool        enabled         = false;
};

// Chaser / step-sequence commands
struct UpdateCueListEntry {
    int          idx;
    FullCueEntry entry;
};

struct NewChaserCue {
    std::string name = "New Chaser";
};

// Playback management commands
struct NewPlayback        { std::string name; };
struct DeletePlayback     { int id; };
struct RenamePlayback     { int id; std::string name; };   // rename an existing playback
struct SetPlaybackGo      { int id; };                 // GO / activate
struct SetPlaybackStop    { int id; };                 // STOP / deactivate
struct SetPlaybackClear   { int id; };                 // CLEAR — wipe cuestack, return to cue 1
struct SetPlaybackBlind   { int id; bool blind; };     // toggle blind mode
struct SetPlaybackIntensity { int id; float intensity; };
struct SetPlaybackCueGo   { int id; };                 // advance to next cue in this playback
struct SetPlaybackCueBack { int id; };
struct SetPlaybackJump    { int id; int cue_major; int cue_minor; };
struct RecordToPlayback   { int playback_id; FullCueEntry entry; };
struct UpdatePlaybackCue  { int playback_id; int cue_idx; FullCueEntry entry; };
struct DeletePlaybackCue  { int playback_id; int cue_idx; };
struct SetPlaybackDmxTrigger { int id; int universe; int channel; uint8_t threshold; };
struct SetPlaybackConfig     { int playback_id; PlaybackConfig config; };

// Programmer frame — current frame-editor objects at highest priority
struct SetProgrammerFrame {
    KeyframeLayer objects;
    GlobalLayer   global_layer;
    FxLayer       fx_layer;
    bool          blind  = false;
    bool          active = false;
};

// NDI output configuration
struct SetNdiConfig {
    std::string name;
    int         width        = 1920;
    int         height       = 1080;
    int         fps_N        = 60;    // fps numerator  (e.g. 60000 for 59.94)
    int         fps_D        = 1;     // fps denominator (e.g. 1001 for 59.94)
    bool        enabled      = true;
    bool        clock_video  = true;  // NDI-clocked timing vs. app-clocked
};

// Beam/glow raster params (also carries Otaniemi projector-mode overrides)
struct SetRasterConfig {
    float beam_radius              = 2.5f;
    float glow_radius              = 8.f;
    float glow_alpha               = 0.25f;
    // Otaniemi fields — only applied when otaniemi_enabled is true
    bool  otaniemi_enabled         = false;
    float otaniemi_line_thickness  = 4.0f;
    float otaniemi_brightness_boost= 1.3f;
    float otaniemi_glow_radius     = 8.0f;
    bool  otaniemi_auto_fill       = true;
};

// Output enable / kill switch — when disabled, build_frame() submits a blank frame
struct SetOutputEnable { bool enabled; };

// Master enable per stream kind — acts as a top-level gate on top of per-output patch.
// When disabled for a kind, NO frames of that kind are sent regardless of per-stream flags.
struct SetStreamTypeEnabled {
    enum class Kind : int { DAC = 0, NDI = 1, ArtNet = 2, IDN = 3 };
    Kind kind;
    bool enabled;
};

// Emergency Shutoff — force-blanks all output immediately; active state persists
// until explicitly cleared (user presses Ctrl+Alt+Enter or sends active=false).
struct SetEmergencyShutoff { bool active; };

// Safety Blackout Zones — HARD enforcement applied after all other processing
struct SetSafetyBlackout {
    bool enabled;
    struct BorderCrop { float left, right, top, bottom, tilt_deg; } borders;
    struct BlockZone  { float cx, cy, hw, hh, angle_deg; bool enabled; };
    std::vector<BlockZone> zones;
};

// Output patch — define N output streams; engine fans out the final frame to each.
// streams_def: new typed descriptor list (preferred).
// streams: legacy OutputStreamConfig list (kept for backward compatibility).
// When streams_def is non-empty it takes precedence; otherwise streams is used.
struct SetOutputPatch {
    std::vector<OutputStreamConfig> streams;
    std::vector<OutputStreamDef>    streams_def;
};

// Select which output stream IDs receive programmer (frame-editor) content.
// Streams not in this set still run their normal cue playback output.
struct SetActiveStreams {
    std::vector<int> stream_ids;  // empty = all streams
};

struct SetMirroredStreams {
    std::vector<int> stream_ids;  // IDs to X-flip; empty = no mirroring
};

// ── Timeline commands ─────────────────────────────────────────────────────────
struct CreateTimeline       { TimelineDef def; };
struct DeleteTimeline       { std::string id; };
struct RenameTimeline       { std::string id; std::string name; };
struct SetTimelineArmed         { std::string id; bool armed; };
struct SetTimelineRecordArmed   { std::string id; bool armed; };
struct TimelinePlay         { std::string id; };
struct TimelinePause        { std::string id; };
struct TimelineStop         { std::string id; };
struct TimelineRewind       { std::string id; };
struct TimelineSeek         { std::string id; int64_t frame; };
struct SetTimelineSource    { std::string id; std::string slot; };
struct SetTimelineLink      { std::string id; bool link_mode; };
struct SetTimelineOffset    { std::string id; int64_t offset_frames; };
struct AddTimelineTrack     { std::string timeline_id; std::string name; };
struct RemoveTimelineTrack  { std::string timeline_id; int track_id; };
struct UpdateTimelineTrack  {
    std::string timeline_id; int track_id;
    std::string name; bool muted; bool locked; bool collapsed;
};
struct AddTimelineEvent     { std::string timeline_id; int track_id; TimelineEvent event; };
struct RemoveTimelineEvent  { std::string timeline_id; int track_id; int64_t event_id; };
struct UpdateTimelineEvent  { std::string timeline_id; int track_id; TimelineEvent event; };
struct SetTimecodeSettings  { ProjectTimecodeConfig config; };
struct TimelineWaitForGoAdvance { std::string id; };

// ── Timeline audio commands ───────────────────────────────────────────────────
struct SetTimelineAudio   { std::string id; AudioTrackDef track; std::vector<float> peaks; }; // peaks precomputed by UI thread
struct ClearTimelineAudio { std::string id; };

// Explicit programmer clear: wipes programmer_active_, all content, AND all per-stream latches.
struct ClearProgrammer {};

// Latch programmer content to specific streams permanently.
// Called when the operator deselects a stream while programmer has content.
// The engine stores this content per-stream; it stays until CLR (ClearProgrammer).
struct LatchProgrammer {
    std::vector<int> stream_ids;
    KeyframeLayer    objects;
    GlobalLayer      global_layer;
    FxLayer          fx_layer;
};

} // namespace cmd

using EngineCommand = std::variant<
    cmd::Play, cmd::Stop, cmd::Pause,
    cmd::SetMasterIntensity, cmd::SetPointRate, cmd::SetBPM,
    cmd::ActivateCue, cmd::DeactivateCue, cmd::SetGeneratorParam,
    cmd::LoadProject,
    cmd::SetBeamThickness, cmd::SetBloomRadius,
    cmd::SetHazeDensity, cmd::SetExposure,
    cmd::Seek, cmd::GoNext, cmd::GoPrev, cmd::SetLoopEnd,
    cmd::CueListGo, cmd::CueListBack, cmd::CueListJump,
    cmd::SetFxParam, cmd::SetFxEnabled, cmd::SetFxBypassed, cmd::SetFxWet,
    cmd::TriggerMacro,
    cmd::SetRecordMode, cmd::RecordCue, cmd::UpdateCue,
    cmd::DeleteCue, cmd::InsertCue, cmd::RenameCue, cmd::SetCueTiming, cmd::MoveCue,
    cmd::SetZone, cmd::RemoveZone,
    cmd::BamPaint, cmd::BamClear, cmd::BamSetEnabled, cmd::ResetScanFail,
    cmd::SetScanFailEnabled,
    cmd::QuickShowTrigger, cmd::QuickShowAssign, cmd::QuickShowClear,
    cmd::LiveProXY, cmd::LiveProScale, cmd::LiveProSpeed, cmd::LiveProBeat,
    cmd::SetMidiBinding, cmd::RemoveMidiBinding, cmd::ClearMidiBindings,
    cmd::SetActiveColor, cmd::SetActivePosition,
    cmd::PatchDmxChannel,
    cmd::SetArtNetOutput,
    cmd::UpdateCueListEntry, cmd::NewChaserCue,
    cmd::NewPlayback, cmd::DeletePlayback, cmd::RenamePlayback,
    cmd::SetPlaybackGo, cmd::SetPlaybackStop, cmd::SetPlaybackClear,
    cmd::SetPlaybackBlind, cmd::SetPlaybackIntensity,
    cmd::SetPlaybackCueGo, cmd::SetPlaybackCueBack, cmd::SetPlaybackJump,
    cmd::RecordToPlayback, cmd::UpdatePlaybackCue, cmd::DeletePlaybackCue,
    cmd::SetPlaybackDmxTrigger,
    cmd::SetNdiConfig, cmd::SetRasterConfig,
    cmd::SetPlaybackConfig,
    cmd::SetProgrammerFrame,
    cmd::SetOutputEnable,
    cmd::SetStreamTypeEnabled,
    cmd::SetSafetyBlackout,
    cmd::SetEmergencyShutoff,
    cmd::SetOutputPatch,
    cmd::SetActiveStreams,
    cmd::SetMirroredStreams,
    // Timeline system
    cmd::CreateTimeline, cmd::DeleteTimeline, cmd::RenameTimeline,
    cmd::SetTimelineArmed, cmd::SetTimelineRecordArmed,
    cmd::TimelinePlay, cmd::TimelinePause, cmd::TimelineStop,
    cmd::TimelineRewind, cmd::TimelineSeek,
    cmd::SetTimelineSource, cmd::SetTimelineLink, cmd::SetTimelineOffset,
    cmd::AddTimelineTrack, cmd::RemoveTimelineTrack, cmd::UpdateTimelineTrack,
    cmd::AddTimelineEvent, cmd::RemoveTimelineEvent, cmd::UpdateTimelineEvent,
    cmd::SetTimecodeSettings, cmd::TimelineWaitForGoAdvance,
    cmd::SetTimelineAudio, cmd::ClearTimelineAudio,
    cmd::ClearProgrammer,
    cmd::LatchProgrammer
>;

// ─────────────────────────────────────────────────────────────────────────────
//  Engine state snapshot (double-buffered; UI reads this)
// ─────────────────────────────────────────────────────────────────────────────
struct EngineSnapshot {
    double       time            = 0.0;
    double       loop_end        = 32.0;
    bool         playing         = false;
    float        master_intensity= 1.f;
    float        bpm             = 120.f;
    int          point_rate      = kDefaultPointRate;
    int          active_cue      = -1;
    int          cue_list_idx    = -1;
    float        engine_fps      = 0.f;
    uint64_t     frame_count     = 0;
    AudioSnapshot audio;
    PointBuffer  preview_points; // low-res copy for UI (max 256 pts)
    double       artnet_latency_ms = 0.0;
    // DMX mirror (first 2 universes) for UI display
    DmxUniverse  dmx[2];

    // ── Timecode (LTC/MTC/ArtNetTC/Internal) ─────────────────────────────
    TimecodeState timecode;

    // ── Cue list state ────────────────────────────────────────────────────
    int          cuelist_entry_count  = 0;
    int          cuelist_current_idx  = -1;
    double       cuelist_fade_level   = 0.0;
    std::string  next_cue_name;      // name of the cue GO will advance to next
    bool         record_mode          = false;

    // ── MIDI learn state ──────────────────────────────────────────────────
    bool        midi_learn_armed    = false;
    std::string midi_learn_target;
    int         active_color_slot   = -1;
    int         active_position_slot= -1;

    // ── DAC hot-plug state ────────────────────────────────────────────────
    std::vector<DacDescriptor> available_dacs;
    bool                       dac_hot_plugged = false; // set true on any change

    // ── Zone routing state ────────────────────────────────────────────────
    std::vector<Zone> zones;

    // ── Safety system state ─────────────────────
    bool        safety_ok     = true;
    std::string safety_status = "OK";
    bool        bam_enabled   = true;
    uint8_t     bam_cells[64 * 64]{}; // flattened 64x64 grid for UI display

    // ── Quick Show mirror (page×row×col cue_idx, -1=empty) ───────────────
    // Flattened: [page*60 + row*10 + col]
    static constexpr int kQSTotalSlots = 32 * 6 * 10;
    int quickshow_slots[kQSTotalSlots]{};  // cue_idx values, -1 = empty

    // ── ArtNet output state ───────────────────────────────────────────────
    bool artnet_out_enabled = false;

    // ── NDI output state ──────────────────────────────────────────────────
    bool ndi_active      = false;
    int  ndi_connections = 0;

    // ── Multi-output stream states ─────────────────────────────────────────
    struct OutputStreamSnap {
        int              id         = 0;
        std::string      name;
        OutputStreamType type       = OutputStreamType::Laser;
        bool             enabled    = false;
        bool             ndi_active = false;
        int              ndi_conns  = 0;
        std::string      dac_type;
        std::string      dac_address;
    };
    std::vector<OutputStreamSnap> output_streams;
    std::vector<int>              active_stream_ids;

    // ── Current output patch state (stream_defs_ mirror for UI) ───────────────
    std::vector<OutputStreamDef>  stream_defs;

    // ── Per-output content frame (one per active output stream) ───────────────
    // Populated by build_frame() each tick. UI can read these for per-output
    // previews without re-running the engine.
    struct OutputFrame {
        int         stream_id = 0;
        PointBuffer points;
    };
    std::vector<OutputFrame> output_frames;

    // ── Chaser state ─────────────────────────────────────────────────────────
    bool chaser_active     = false;
    int  chaser_step       = 0;
    int  chaser_step_count = 0;

    // ── CueList version counter (increments on any cue list change) ───────────
    uint32_t cuelist_version = 0;

    // ── Multi-playback state ──────────────────────────────────────────────────
    static constexpr int kMaxPlaybacks = 40;
    struct PlaybackSnap {
        int         id           = -1;
        bool        active       = false;
        bool        blind        = false;
        float       intensity    = 1.f;
        int         current_cue  = -1;
        int         cue_count    = 0;
        std::string name;
        std::string current_cue_name;
        float       fade_level   = 0.f;
        float       fade_in_dur  = 0.f;   // total fade-in duration in seconds (0 = snap)
        float       fade_elapsed = 0.f;   // seconds elapsed in current fade
    };
    PlaybackSnap playbacks[kMaxPlaybacks]{};
    int          playback_count = 0;

    // ── FX stack for the currently active cue (max 32 blocks) ────────────
    struct FxBlockSnapshot {
        char name[64];
        char category[32];
        bool  enabled;
        bool  bypassed;
        float wet;
        struct Param {
            char  name[32];
            float effective_val;
            float base_val;
        } params[16];
        int param_count;
    };
    static constexpr int kMaxFxBlocks = 32;
    FxBlockSnapshot fx_blocks[kMaxFxBlocks]{};
    int             fx_block_count = 0;

    // ── Programmer state (for UI indicator) ──────────────────────────────────
    bool programmer_active = false;
    bool programmer_blind  = false;

    // ── Timeline system snapshot ───────────────────────────────────────────────
    struct TimelineSnap {
        std::string          id;
        std::string          name;
        TimelineState        state            = TimelineState::Idle;
        int64_t              position_frames  = 0;
        SmpteRate            fps              = SmpteRate::Fps25;
        int64_t              length_frames    = 0;
        std::string          tc_slot;
        bool                 link_mode        = true;
        bool                 record_armed     = false;
        TimecodeSourceStatus       source_status = TimecodeSourceStatus::Disabled;
        int                        track_count   = 0;
        std::vector<TimelineTrack> tracks;        // full track+event data for editor
        std::optional<AudioTrackDef> audio_track; // nullopt = no audio loaded
        std::vector<float>           audio_peaks; // pre-computed peak envelope
    };
    std::vector<TimelineSnap>   timelines;
    ProjectTimecodeConfig       tc_config;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ChaserRunState — runtime state for an active chaser cue
// ─────────────────────────────────────────────────────────────────────────────
struct ChaserRunState {
    int    step             = 0;
    double step_enter_s     = 0.0;
    int    effective_cue_idx= -1;  // resolved cue slot index
};

// ─────────────────────────────────────────────────────────────────────────────
//  Show Engine
// ─────────────────────────────────────────────────────────────────────────────
class ShowEngine {
public:
    ShowEngine(RenderBus& bus, AudioAnalyzer& audio);
    ~ShowEngine();

    // Start the 1000 Hz engine thread
    void start();
    void stop();

    // Thread-safe command interface (called from UI/network threads)
    void send(EngineCommand cmd);

    // Input feeds (called from listener threads via lock-free queues)
    void push_dmx(int universe, const DmxUniverse& data);

    // Snapshot for UI consumption (double-buffered, non-blocking)
    EngineSnapshot snapshot() const;

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Called from UI thread to get a snapshot of the full cue list (for chaser editing UI)
    std::vector<FullCueEntry> read_full_cue_list() const;

    // Called from UI thread to get the cuelist for one specific playback
    std::vector<FullCueEntry> read_playback_cuelist(int pb_id) const;

    // Called from UI thread to get a snapshot of all timeline defs (tracks + events)
    std::vector<TimelineDef> read_timelines() const;

private:
    void engine_loop();
    void tick(double dt);
    void process_commands();
    void process_dmx_input();
    void evaluate_cues(double t, double dt);
    void build_frame();
    void update_snapshot(double fps);

    // Sync the live CueList entries back to project_->full_cue_list.
    // Called after any cue list mutation to keep the project document current.
    void sync_cuelist_to_project();

    // Record an event to every playing + record_armed timeline
    void record_event_to_armed_timelines(TimelineEventType type,
                                         const std::string& target_id,
                                         std::map<std::string, float> params = {},
                                         std::string label = {});

    RenderBus&              bus_;
    AudioAnalyzer&          audio_;

    MpscQueue<EngineCommand>               cmd_queue_;
    MpscQueue<std::pair<int,DmxUniverse>>  dmx_queue_;

    std::shared_ptr<Project>               project_;

    // Engine state (engine-thread owned)
    bool         playing_        = false;
    double       time_           = 0.0;
    double       fx_time_        = 0.0;  // always-advancing clock for FX/Global (never resets)
    float        master_         = 1.f;
    float        bpm_            = 120.f;
    int          point_rate_     = kDefaultPointRate;
    int          active_cue_     = -1;
    int          cue_list_idx_   = -1;   // current position in project cue_list
    float        fade_level_     = 1.f;
    double       loop_end_       = 32.0;
    bool         record_mode_    = false;
    bool         output_enabled_          = true;  // kill switch: false = blank every frame
    bool         emergency_shutoff_active_ = false; // emergency: force-blank all output

    // Render params (read by NDI rasterizer thread too via atomics)
    std::atomic<float>  beam_thickness_{2.0f};
    std::atomic<float>  bloom_radius_  {8.0f};
    std::atomic<float>  haze_density_  {0.4f};
    std::atomic<float>  exposure_      {1.0f};

    DmxUniverse  dmx_[kMaxUniverses];

    // Protects extra_laser_buses_ against concurrent access from UI thread
    // (extra_laser_bus() call) and engine thread (SetOutputPatch handler).
    mutable std::mutex           extra_buses_mtx_;

    // Double-buffered snapshot
    mutable std::mutex           snap_mtx_;
    EngineSnapshot               snap_[2];
    std::atomic<int>             snap_write_idx_{0};

    std::atomic<bool>            running_{false};
    std::thread                  thread_;

    uint64_t     frame_count_   = 0;
    HRTimer      frame_timer_;

    // Watchdog: updated every tick; watchdog thread checks it
    std::atomic<uint64_t> watchdog_heartbeat_{0};

    // ── CueList + FX integration (§B7/B8, §B4) ───────────────────────────
    CueList               cue_list_;               // MagicQ-class playback state machine
    std::vector<FxEngine> cue_fx_engines_;          // one FxEngine per cue slot
    ExprContext           expr_ctx_;               // updated each tick
    float                 last_dt_ = 0.f;          // last tick dt, used by build_frame()

    PointOptimizer        point_optimizer_;        // §B1 — 7-step output optimizer

    // ── DAC hot-plug registry ─────────────────────────────────────────────
    DacRegistry           dac_registry_;
    std::atomic<bool>     dac_changed_{ false };  // set by on_changed callback

    // ── Zone routing ──────────────────────────────────────────────────────
    ZoneManager           zone_manager_;

    // ── Safety (BAM + scan-fail + interlock) ─────────────────────────────
    SafetyManager         safety_;

    // ── Quick Show grid storage ───────────────────────────────────────────
    struct QSEntry { int cue_idx = -1; };
    QSEntry quickshow_[32][6][10]{};

    // ── MIDI learn ────────────────────────────────────────────────────────
    MidiLearnMap     midi_map_;
    MidiLearnSession midi_learn_;

    // ── Color + position palettes ─────────────────────────────────────────
    ColorPalette     color_palette_;
    PositionPalette  position_palette_;
    int              active_color_slot_    = -1;
    int              active_position_slot_ = -1;

    // ── Stream-type master enables (top-level gates set by SetStreamTypeEnabled) ─
    bool         stream_type_dac_enabled_    = true;
    bool         stream_type_ndi_enabled_    = true;
    bool         stream_type_artnet_enabled_ = true;

    // ── ArtNet DMX output ─────────────────────────────────────────────────
    ArtNetSender artnet_out_;
    bool         artnet_out_enabled_         = false;
    int          artnet_out_universe_offset_ = 0;

    // ── NDI software rasterization output ────────────────────────────────
    NdiSender                ndi_sender_;
    RasterConfig             raster_cfg_;
    std::atomic<bool>        ndi_enabled_{false};
    std::vector<uint8_t>     ndi_pixel_buf_;     // BGRA rasterize target
    int                      ndi_width_  = 1920;
    int                      ndi_height_ = 1080;
    int                      ndi_fps_N_  = 60;   // fps numerator  (e.g. 60000 for 59.94)
    int                      ndi_fps_D_  = 1;    // fps denominator (e.g. 1001 for 59.94)

    // ── Chaser runtime state ──────────────────────────────────────────────────
    std::optional<ChaserRunState>  chaser_run_;
    std::atomic<uint32_t>               cuelist_version_{0};
    mutable std::mutex                  cuelist_read_mtx_;
    std::vector<FullCueEntry>           cuelist_read_cache_;
    std::map<int, std::vector<FullCueEntry>> pb_cuelist_cache_;

    mutable std::mutex                  timeline_read_mtx_;
    std::vector<TimelineDef>            timeline_read_cache_;

    // ── Per-playback runtime state ────────────────────────────────────────────
    struct PlaybackRunState {
        int     id           = -1;
        int     current_idx  = -1;
        int     prev_idx     = -1;    // cue we're fading FROM (-1 = none)
        float   fade_level   = 0.f;
        double  fade_start   = 0.0;  // kept for reference but not used for alpha computation
        float   fade_elapsed = 0.f;  // seconds elapsed since fade started (monotonic, not tied to time_)
        float   fade_in_dur  = 0.f;  // duration of current fade-in (seconds)
        float   fade_alpha   = 1.f;  // current fade-in alpha (0=start, 1=done)
        float   prev_alpha   = 0.f;  // fade-out alpha for previous cue (1=full, 0=gone)
        float   intensity    = 1.f;
        bool    active       = false;
        bool    blind        = false;
        uint8_t dmx_ch2_prev        = 0;     // previous ch2 value for TwoChannel rising-edge detection
        double  hold_start          = -1.0;  // time when current cue's hold phase started (-1 = not started)
        float   beat_phase          = 0.f;   // accumulator for go_at_bpm (0..1, wraps to advance cue)
        bool    first_trigger_done  = false; // true after the first GO; resets on STOP/CLEAR
    };
    std::vector<PlaybackRunState>  playback_states_;
    int                            next_playback_id_{1};

    // ── Programmer (frame-editor) state ──────────────────────────────────────
    KeyframeLayer  programmer_objects_;
    GlobalLayer    programmer_global_layer_;
    FxLayer        programmer_fx_layer_;
    bool           programmer_blind_  = false;
    bool           programmer_active_ = false;

    // Per-stream latched programmer state.
    // Populated by LatchProgrammer when a stream is deselected with content in the programmer.
    // Cleared globally by ClearProgrammer (CLR button).
    struct StreamProgrammerState {
        KeyframeLayer objects;
        GlobalLayer   global_layer;
        FxLayer       fx_layer;
        bool          has_content = false;
    };
    std::unordered_map<int, StreamProgrammerState> stream_prog_;

    // ── Safety Blackout Zones ─────────────────────────────────────────────────
    bool                              safety_blackout_enabled_ = true;
    cmd::SetSafetyBlackout::BorderCrop safety_blackout_borders_{};
    std::vector<cmd::SetSafetyBlackout::BlockZone> safety_blackout_zones_;
    void apply_safety_blackout(PointBuffer& pts);

    // ── Multi-output patch ────────────────────────────────────────────────────
    // Configured via cmd::SetOutputPatch.  When non-empty the engine fans out the
    // final composited frame (after global safety blackout) to every enabled stream.
    // If empty the legacy single-bus path is used (backwards-compatible).
    struct OutputStreamRuntime {
        OutputStreamConfig config;
        // NDI is hosted here; laser uses the separate bus/DacManager array.
        std::unique_ptr<NdiSender>     ndi_sender;
        std::vector<uint8_t>           ndi_pixel_buf;
        // Bus for the laser path — pointer owned by engine (extra buses) or by
        // ui_app (slot 0 only).  Null if not yet wired.
        RenderBus*                     laser_bus = nullptr;
    };
    std::vector<OutputStreamRuntime>   output_streams_;
    std::vector<OutputStreamDef>       stream_defs_;       // current patch (OutputStreamDef descriptors)
    std::vector<int>                   active_stream_ids_;
    std::vector<int>                   mirrored_stream_ids_;
    // Extra laser buses owned by the engine for streams beyond the first.
    std::vector<std::unique_ptr<RenderBus>> extra_laser_buses_;

    // Apply per-stream safety blackout on a copy of the points
    void apply_per_stream_safety(PointBuffer& pts, const OutputSafetyConfig& cfg) const;
    // Apply per-stream output transform (offset/scale/rotation/flip)
    void apply_per_stream_transform(PointBuffer& pts, const OutputTransform& xf) const;

    // ── Timecode router (LTC/MTC/ArtNetTC selector) ──────────────────────────
    TimecodeRouter timecode_router_;

    // ── Timeline system ───────────────────────────────────────────────────────
    TimelineEngine        timeline_engine_;
    TimelineAudioPlayer   timeline_audio_player_;
    ProjectTimecodeConfig tc_config_;

    // Dispatch a timeline-fired event to the appropriate engine command
    void dispatch_timeline_event(const TimelineEngine::FiredEvent& fe);

    // ── Preview frames (downsampled, for UI) ─────────────────────────────────
    PointBuffer                          preview_pts_;
    std::unordered_map<int, PointBuffer> per_stream_preview_pts_; // keyed by stream id

    // ── Evaluated params cache ─────────────────────────────────────────────────
    // evaluate_cues() writes the fully-computed GeneratorParams here (after
    // applying DMX patches, automation, fade level, and master intensity).
    // build_frame() reads from here so it picks up all modulations.
    GeneratorParams evaluated_params_{};
    bool            evaluated_params_valid_ = false; // true when a cue is active

    float gfx_smooth_[32]{};   // per-slot smoothed GlobalFX value
    float ffx_smooth_[32]{};   // per-slot smoothed FrameFX position value

    // Playback helper accessors (engine-thread only)
    PlaybackDef*      find_playback_def(int id);
    const PlaybackDef* find_playback_def(int id) const;
    PlaybackRunState* find_playback_state(int id);

    // Static helper to render a KeyframeLayer to a PointBuffer
    static PointBuffer render_keyframe_layer(const KeyframeLayer& kf, int target_pts);

public:
    uint64_t watchdog_heartbeat() const {
        return watchdog_heartbeat_.load(std::memory_order_relaxed);
    }

    const MidiLearnMap& midi_learn_map() const { return midi_map_; }

    // Returns a snapshot of current output stream configs (for UI display).
    // Thread-safe: reads under snap_mtx_.
    std::vector<OutputStreamConfig> output_stream_configs() const;

    // Wire an external laser bus into stream slot i so the engine can submit frames.
    // Called from ui_app when creating a DacManager for stream i.
    // i == 0 is always the legacy bus_ (already wired at construction).
    // i >= 1 is an extra engine-owned bus.
    RenderBus* extra_laser_bus(int stream_index);
};

// ─────────────────────────────────────────────────────────────────────────────
//  Watchdog — monitors engine thread for stalls
// ─────────────────────────────────────────────────────────────────────────────
class EngineWatchdog {
public:
    explicit EngineWatchdog(ShowEngine& engine);
    ~EngineWatchdog();
    void start();
    void stop();

private:
    void watch_loop();
    ShowEngine&         engine_;
    std::atomic<bool>   running_{false};
    std::thread         thread_;
    static constexpr int kStallThresholdMs = 500;
};

} // namespace idhmfis
