#pragma once
// UIState — plain data snapshot owned exclusively by the UI thread.
// The engine writes into a back-buffer; the UI thread swaps it each frame.
// No locks are needed inside the UI; synchronisation happens at the swap point.

#include "../core/types.h"
#include "../cuelist/cuelist_types.h"
#include "../dac/dac_registry.h"
#include "../zones/zone_types.h"
#include "../input/midi_learn.h"
#include "../project/palette.h"
#include "../render/cue_thumbnailer.h"
#include "../timeline/timeline_types.h"
#include <array>
#include <optional>
#include <vector>
#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Lightweight cue descriptor — enough for the cue library grid.
// ─────────────────────────────────────────────────────────────────────────────
struct CueInfo {
    int         index        = 0;
    std::string name         = "Untitled";
    std::string generator    = "unknown";
    Color4      card_color   = { 0.f, 0.898f, 1.f };   // default: laser cyan
    float       duration_s   = 4.f;                     // 0 = loop forever
    bool        selected     = false;
    // Bug2 fix: trigger type from FullCueEntry  (0=Halt, 1=Follow, 2=Wait, 3=TC …)
    int         trigger_type = 0;
    // Bug3 fix: actual fade times from CueListEntry
    float       fade_in      = 0.f;
    float       fade_out     = 0.f;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Single keyframe on an automation track.
// ─────────────────────────────────────────────────────────────────────────────
struct Keyframe {
    float time_s  = 0.f;
    float value   = 0.f;
    int   curve   = 0;    // 0=linear, 1=ease-in, 2=ease-out, 3=step
};

// ─────────────────────────────────────────────────────────────────────────────
//  Automation track — one parameter over time.
// ─────────────────────────────────────────────────────────────────────────────
struct AutomationTrack {
    std::string          param_name;
    std::vector<Keyframe> keyframes;
    bool                 visible = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Cue block on the timeline (positioned instance of a CueInfo).
// ─────────────────────────────────────────────────────────────────────────────
struct TimelineCue {
    int   cue_index  = 0;
    float start_s    = 0.f;
    float duration_s = 4.f;
    int   track      = 0;   // vertical lane (0-based)
};

// ─────────────────────────────────────────────────────────────────────────────
//  Complete UI frame snapshot.
// ─────────────────────────────────────────────────────────────────────────────
struct UIState {
    // --- Laser preview ---------------------------------------------------------
    PointBuffer  preview_points;        // Current frame points for rasteriser
    float        preview_zoom    = 0.f; // 0 = fit to panel, 1 = 100%, other = scale

    // --- Transport / timing ----------------------------------------------------
    float        engine_fps       = 0.f;
    float        bpm              = 120.f;
    bool         playing          = false;
    bool         paused           = false;
    double       playhead_s       = 0.0;    // current play position in seconds
    double       loop_end_s       = 32.0;   // loop region end

    // --- Output status ---------------------------------------------------------
    float        artnet_latency_ms = 0.f;
    bool         ndi_streaming     = false;
    bool         dac_connected     = false;
    int          dac_pps           = 0;     // current output point rate

    // --- Cue state -------------------------------------------------------------
    int          active_cue_idx   = -1;
    float        master_intensity = 1.f;
    std::vector<CueInfo>      cues;
    std::vector<TimelineCue>  timeline_cues;

    // --- Generator params for the active cue -----------------------------------
    GeneratorParams active_params;

    // --- DMX -------------------------------------------------------------------
    DmxUniverse  dmx_uni_0;   // universe 0 snapshot
    DmxUniverse  dmx_uni_1;   // universe 1 snapshot

    // --- Audio -----------------------------------------------------------------
    AudioSnapshot audio;

    // --- Automation tracks (for active cue) ------------------------------------
    std::vector<AutomationTrack> automation_tracks;

    // --- Project info ----------------------------------------------------------
    std::string  project_name   = "Untitled Show";
    std::string  project_path;          // empty if unsaved
    bool         project_dirty  = false;

    // --- FX stack snapshot (for active cue) -----------------------------------
    struct FxBlockInfo {
        std::string name;
        std::string category;
        bool enabled   = true;
        bool bypassed  = false;
        float wet      = 1.f;
        struct Param {
            std::string name;
            float val      = 0.f;
            float base_val = 0.f;
        };
        std::vector<Param> params;
    };
    std::vector<FxBlockInfo> active_fx_stack;

    // --- Cue list playback state -----------------------------------------------
    int    cuelist_entry_count  = 0;
    int    cuelist_current_idx  = -1;
    double cuelist_fade_level   = 0.0;
    std::string next_cue_name;    // name of the cue that GO will fire next
    bool   record_mode          = false;
    int    cuelist_selected_idx = -1;  // UI-selected cue (independent of active)

    // --- Macro/Executor slots (32 slots) --------------------------------------
    std::array<std::string, 32> macro_names;  // empty string = display as "M{n}"

    // --- Rolling ArtNet latency history (for histogram) -----------------------
    static constexpr int kLatencyHistoryLen = 64;
    float latency_history[kLatencyHistoryLen]{};
    int   latency_history_idx = 0;

    // --- Recent files ----------------------------------------------------------
    std::vector<std::string> recent_files;

    // --- Output kill switch ---------------------------------------------------
    bool output_enabled = true;   // true = laser output ON by default

    // --- Emergency Shutoff ---------------------------------------------------
    bool emergency_shutoff_active  = false; // true = all output force-blanked
    int  emergency_shutoff_key     = 0;     // ImGuiKey value, 0 = unbound
    bool emergency_key_capturing   = false; // true = waiting for key press to bind

    // --- DAC list (populated from engine snapshot) ----------------------------
    std::vector<DacDescriptor> available_dacs;

    // --- Zone routing state ---------------------------------------------------
    std::vector<Zone> zones;

    // --- BAM / Safety state -------------------------------------------------------
    struct BamSnapshot {
        bool enabled = false;
        bool safety_ok = true;
        std::string safety_status = "OK";
        // 64x64 grid for display (flattened, row-major)
        uint8_t cells[64 * 64]{};
    };
    BamSnapshot bam;

    // --- Quick Show grid ----------------------------------------------------------
    struct QuickShowSlot {
        std::string cue_name;
        int         cue_idx = -1;   // -1 = empty
        bool        active  = false;
        bool        is_next = false;
    };
    static constexpr int kQSPages = 32;
    static constexpr int kQSRows  = 6;
    static constexpr int kQSCols  = 10;
    QuickShowSlot quickshow[kQSPages][kQSRows][kQSCols]{};
    int           quickshow_page  = 0;

    // --- LivePRO performance mode ------------------------------------------------
    float livepro_x = 0.f, livepro_y = 0.f;
    float livepro_scale = 1.f, livepro_speed = 1.f;
    float beat_flash_timers[16]{};
    std::string beat_labels[16];
    bool  livepro_flash_hold = false;

    // --- MIDI learn state --------------------------------------------------------
    bool        midi_learn_armed    = false;
    std::string midi_learn_target;
    std::vector<MidiBinding> midi_bindings; // mirror of MidiLearnMap

    // --- Palette state -----------------------------------------------------------
    ColorPalette    color_palette;
    PositionPalette position_palette;
    int             active_color_slot    = -1;
    int             active_position_slot = -1;

    // --- Cue thumbnails (one per cue, 128x128 RGBA from background renderer) ----
    // Mirrors CueThumbnail but owned by UI thread — copied from the thumbnailer.
    struct CueThumbnailRef {
        bool ready = false;
        uint64_t version = 0;
        std::array<uint8_t, kThumbPixels * 4> pixels{};
    };
    std::vector<CueThumbnailRef> cue_thumbnails; // one per cue

    // --- Chaser / full cue list (synced from engine when cuelist_version changes) --
    std::vector<FullCueEntry> full_cue_list;
    uint32_t cuelist_version = UINT32_MAX; // UINT32_MAX forces first sync

    // --- Per-playback cuelist (for the cuestack editor, synced on demand) ---------
    std::vector<FullCueEntry> pb_cuelist;
    int                       pb_cuelist_pb_id = -1;

    // --- Multi-playback snapshot (mirrored from engine snapshot each frame) ------
    static constexpr int kMaxPlaybacks = 40;
    struct PlaybackSnap {
        int         id               = -1;
        bool        active           = false;
        bool        blind            = false;
        float       intensity        = 1.f;
        int         current_cue      = -1;
        int         cue_count        = 0;
        std::string name;
        std::string current_cue_name;
        float       fade_level       = 0.f;
        float       fade_in_dur      = 0.f;   // total fade duration (0 = snap)
        float       fade_elapsed     = 0.f;   // seconds elapsed in current fade
    };
    struct PlaybackSnapArray {
        PlaybackSnap playbacks[kMaxPlaybacks]{};
        int          playback_count = 0;
    } snap;

    // ── Per-playback configuration (PBCONF popup) ─────────────────────────────
    struct PlaybackConf {
        std::string name;             // display name (editable in PBCONF popup)
        enum class DmxMode { Off, OneChannel, TwoChannel } dmx_mode = DmxMode::Off;
        int  dmx_universe  = 0;
        int  dmx_channel   = 1;
        int  dmx_threshold = 10;
        enum class EndBehavior { Stop, Loop } end_behavior = EndBehavior::Stop;
        int  keyboard_key  = 0;   // ImGuiKey value for GO trigger, 0 = none
        bool key_capturing = false;
        bool go_at_bpm              = false;  // advance cue on each BPM beat
        bool fx_at_bpm              = false;  // FX rates locked to BPM tempo
        bool  fade_on_first_trigger  = false;  // when ON, first cue uses its configured fade-in
        float first_trigger_fade_s   = 0.f;   // >0: override fade-in (seconds) for the first GO
        bool  remember_cuelist_position = false;
        // Which output stream IDs this playback renders to (empty = all streams)
        std::vector<int> output_stream_ids;
    };
    PlaybackConf pb_conf[kMaxPlaybacks]{};

    // Global BPM tap key binding
    int  bpm_tap_key       = 0;   // ImGuiKey value, 0 = none
    bool bpm_key_capturing = false;

    // Timeline transport key bindings (ImGuiKey values, 0 = unbound)
    // Defaults are set in layout.cpp on first use via the keybind configurator.
    int  tl_key_play    = 0;   // Space (32)
    int  tl_key_stop    = 0;   // S
    int  tl_key_rewind  = 0;   // R
    int  tl_key_go      = 0;   // Enter
    int  tl_key_capturing        = 0;
    bool tl_key_capture_active   = false;

    // ── Color input mode ───────────────────────────────────────────────────────
    enum class ColorInputMode { RGBPercent, RGBAbs, CMY, HSI };
    ColorInputMode color_input_mode = ColorInputMode::RGBPercent;
    bool color_settings_open = false;   // floating color settings window

    // ── Timing display format (fade in, fade out, hold columns) ──────────────
    // Seconds  — "1.25 s"  (default)
    // BPM      — "2.50 bt" beat count relative to current BPM
    // MMSSMS   — "00:01.250" (MM:SS.ms)
    enum class TimingDisplayMode { Seconds, BPM, MMSSMS };
    TimingDisplayMode timing_display_mode = TimingDisplayMode::Seconds;

    // ── Color swatches: 8 default + 24 custom recordable slots ────────────────
    static constexpr int kNumSwatches = 32;
    struct ColorSwatch {
        float r = 0.f, g = 0.f, b = 0.f;
        bool  used = false;
    };
    ColorSwatch color_swatches[kNumSwatches]{};

    // ── REM mode — next click on a deletable item removes it ──────────────────
    bool rem_mode = false;

    // ── Keybind map: action_name → ImGuiKey value (0 = unbound) ──────────────
    struct KeybindEntry {
        std::string action;   // human-readable action name
        std::string id;       // internal ID
        int         key = 0;  // ImGuiKey value; 0 = unbound
    };
    std::vector<KeybindEntry> keybinds;

    // ── Network configuration (all protocol inputs / sidecars) ────────────────
    struct NetworkConfig {
        // ── Global interface selection ──────────────────────────────────────────
        bool        iface_auto          = true;   // true = first non-loopback IPv4
        int         iface_index         = 0;      // selected adapter index (when auto=false)
        std::string iface_ip_display;             // current IP string shown in the UI (read-only display)

        // ── ArtNet input ────────────────────────────────────────────────────────
        // Default to enabled: the listener starts unconditionally at launch so
        // Chamsys / grandMA on the same LAN are visible from the first frame.
        bool        artnet_enabled      = true;
        bool        artnet_auto         = true;
        int         artnet_universe     = 0;
        int         artnet_net          = 0;
        int         artnet_subnet       = 0;
        std::string artnet_listen_ip    = "0.0.0.0";
        int         artnet_port         = 6454;
        bool        artnet_merge_htp    = true;
        int         artnet_merge_mode   = 0;      // 0=LTP, 1=HTP
        int         artnet_priority     = 100;

        // ── sACN (E1.31) input ──────────────────────────────────────────────────
        bool        sacn_enabled        = false;
        bool        sacn_auto           = true;
        int         sacn_universe       = 1;
        int         sacn_priority       = 100;
        std::string sacn_multicast_ip   = "239.255.0.1";
        int         sacn_port           = 5568;
        bool        sacn_per_universe   = true;

        // ── OSC ─────────────────────────────────────────────────────────────────
        bool        osc_in_enabled      = true;
        bool        osc_in_auto         = true;
        int         osc_in_port         = 7700;
        std::string osc_in_ip           = "0.0.0.0";
        bool        osc_out_enabled     = false;
        bool        osc_out_auto        = true;
        std::string osc_out_ip          = "127.0.0.1";
        int         osc_out_port        = 7701;
        std::string osc_prefix          = "/idhmfis/";

        // ── CITP/CAEX ───────────────────────────────────────────────────────────
        bool        citp_enabled        = false;
        bool        citp_auto           = true;
        int         citp_tcp_port       = 6430;
        std::string citp_multicast_group= "239.224.0.180";
        int         citp_multicast_port = 4809;
        std::string citp_source_name    = "IDHMFIS";
        bool        citp_respond_capture= true;

        // ── IDN-Stream ──────────────────────────────────────────────────────────
        bool        idn_enabled         = false;
        bool        idn_auto            = true;
        std::string idn_broadcast_addr  = "255.255.255.255";
        int         idn_port            = 7255;
        int         idn_channel         = 0;

        // ── CLS (CommonLaserStream) ─────────────────────────────────────────────
        bool        cls_enabled         = false;
        bool        cls_auto            = true;
        int         cls_tcp_port        = 7256;
        int         cls_udp_port        = 7256;
        bool        cls_udp_broadcast   = true;

        // ── EtherDream ──────────────────────────────────────────────────────────
        bool        etherdream_enabled  = false;
        bool        etherdream_auto     = true;
        float       etherdream_timeout  = 2.0f;
        std::string etherdream_preferred_ip;

        // ── NDI output ──────────────────────────────────────────────────────────
        bool        ndi_net_auto        = true;
        std::string ndi_net_source_name = "IDHMFIS Laser Preview";
        int         ndi_net_bandwidth   = 1;   // 0=Low, 1=High Quality
        int         ndi_net_fps         = 30;

        // ── DMX universe base offset ─────────────────────────────────────────────
        // 0 = no adjustment (default).  Shifts the universe used in playback trigger
        // lookups.  Use -1 if your project stores universe 1 but Art-Net arrives on 0.
        int         dmx_universe_offset  = 0;
    };
    NetworkConfig net_config;

    // ── ArtNet input live status (NOT persisted — updated every UI frame) ────────
    // Written from the UI-thread InputRouter owner; read by panel_setup to render
    // the status row without any locking (all atomic reads underneath).
    struct ArtNetInputStatus {
        bool     running         = false;
        uint64_t packets         = 0;
        double   last_age_ms     = -1.0;  // ms since last ArtDMX packet; -1 = never
        int      active_universes= 0;
    };
    ArtNetInputStatus artnet_input_status;

    // ── NDI / Output configuration ─────────────────────────────────────────────
    struct OutputConfig {
        bool        ndi_enabled   = true;
        std::string ndi_name      = "IDHMFIS";
        int         ndi_width     = 1920;
        int         ndi_height    = 1080;
        int         ndi_fps       = 60;
        int         ndi_fps_N     = 60;    // fps numerator  (e.g. 60000 for 59.94)
        int         ndi_fps_D     = 1;     // fps denominator (e.g. 1001 for 59.94)
        bool        ndi_clock_video = true; // NDI-clocked (true) vs. app-clocked (false)
        float       beam_radius   = 2.5f;
        float       glow_radius   = 8.f;
        float       glow_alpha    = 0.25f;
        // ArtNet output settings (mirrors project file fields)
        std::string artnet_out_ip              = "2.255.255.255";
        int         artnet_out_universe_offset = 0;
        bool        artnet_out_enabled         = false;
        // Additional output destinations
        bool        hdmi_window_enabled        = false;  // borderless SDL window on chosen display
        bool        dac_enabled                = true;   // DAC/ILDA laser hardware output
        bool        idn_stream_enabled         = false;  // IDN broadcast sidecar (UDP 7255)
        bool        virtual_camera_enabled     = false;  // Virtual DirectShow camera output

        // ── Enabled output streams — master enables per stream kind ──────────────
        // Each is a top-level gate; unchecking mutes the entire stream type
        // regardless of per-output patch settings.  Default true (on).
        bool        stream_type_dac_enabled    = true;
        bool        stream_type_ndi_enabled    = true;
        bool        stream_type_artnet_enabled = true;
        bool        stream_type_idn_enabled    = true;

        // ── Laser Output Quality (sent to engine as SetOptimizerConfig) ──────────
        // Controls the 7-step PointOptimizer pipeline that shapes the galvo scan.
        // Increasing dwell counts smooths scan artifacts; reducing them raises speed.
        int   opt_target_pps             = 30000;  // point rate sent to DAC
        float opt_blank_dwell            = 8.f;    // settle points at blank→lit transitions
        float opt_corner_dwell           = 3.f;    // settle points at sharp corners
        float opt_corner_angle_threshold = 0.3f;   // radians; corners sharper than this get dwell
        bool  opt_enable_reorder         = true;   // nearest-neighbour path reorder
        bool  opt_enable_overscan_clip   = true;   // hard-clip at ±(1 + overscan_margin)
        float opt_overscan_margin        = 0.05f;  // extra margin for overscan clip
    };
    OutputConfig output_config;

    // ── Safety Blackout Zones ─────────────────────────────────────────────────
    struct SafetyBlackoutConfig {
        bool enabled = true;  // master enable for all blackout zones

        // Border crop sliders — each edge can be cropped
        // Values are in normalized output space 0..1 (how far to crop from that edge)
        struct BorderCrop {
            float left   = 0.f;  // 0=no crop on left
            float right  = 0.f;  // 0=no crop on right (from right edge)
            float top    = 0.f;  // 0=no crop at top
            float bottom = 0.f;  // 0=no crop at bottom
            float tilt   = 0.f;  // rotation of the crop boundary, degrees -45..45
        } borders;

        // Rectangular block zones
        struct BlockZone {
            bool        enabled   = true;
            std::string name      = "Zone";
            float       cx        = 0.f, cy = 0.f;    // center in normalized output -1..1
            float       hw        = 0.1f, hh = 0.1f;  // half-width and half-height
            float       angle_deg = 0.f;               // rotation
            bool        dragging  = false;             // UI drag state
            bool        resizing  = false;
            int         resize_handle = -1;
        };
        std::vector<BlockZone> zones;
    };
    SafetyBlackoutConfig safety_blackout;

    // ── Output patch state (populated from engine snapshot) ───────────────────
    struct PatchedOutput {
        int                 id           = 0;
        std::string         name;
        OutputStreamType    type         = OutputStreamType::Laser;
        bool                enabled      = true;
        bool                ndi_active   = false;
        int                 ndi_conns    = 0;
        std::string         dac_type     = "auto";
        std::string         dac_address;
        // CITP/CAEX sidecar identity — populated by ui_app from the DacManager.
        // Shown in the Patch panel so operators can verify Capture sees the source.
        std::string         citp_stream_name; // e.g. "IDHMFIS" or "IDHMFIS-2"
        // Whether this output is currently targeted by the programmer.
        // Mirrors state.selected_output_ids membership; kept in sync by panel_patch.
        bool                selected_for_programming = false;
        // Full config for the properties panel
        OutputStreamConfig  config;
    };
    std::vector<PatchedOutput> patched_outputs;
    std::vector<int>           active_stream_ids;          // which streams programmer edits go to
    std::vector<int>           active_mirrored_stream_ids; // subset receiving X-flipped programmer output

    // Output groups — recorded via REC + click empty cell in the STREAMS grid
    struct OutputGroup {
        int              id             = 0;
        std::string      name;
        std::vector<int> member_ids;
        std::vector<int> mirrored_ids;   // subset of member_ids that receive X-flipped output
    };
    std::vector<OutputGroup> output_groups;
    int                      output_group_next_id = 1;
    int                      active_group_id      = -1;  // -1 = no group selected

    // ── Output targeting (lighting console model) ─────────────────────────────
    // When broadcast_to_all is true the programmer writes to all outputs.
    // When false, only the outputs in selected_output_ids receive programmer content.
    std::vector<int> selected_output_ids;
    bool             broadcast_to_all = true;

    // ── Otaniemi Mode (video-projector-as-laser-simulator operator setup) ─────
    struct OtaniemiConfig {
        bool  enabled          = false;
        float line_thickness   = 4.0f;   // pixels, 1.0–20.0
        bool  auto_fill_shapes = true;   // fill closed shapes instead of scanning outlines
        float brightness_boost = 1.3f;  // 1.0–3.0
        float glow_radius      = 8.0f;  // glow/bloom radius in pixels, 0–30
    };
    OtaniemiConfig otaniemi;

    // ── 3D Preview render settings ─────────────────────────────────────────────
    struct Preview3dSettings {
        bool  scan_mode        = false;   // false = IRL projector (default)
        float scan_speed       = 0.8f;    // scans/sec (scan mode only)
        int   trail_pct        = 20;      // trail length as % of point count (scan mode)
        float beam_brightness  = 1.0f;    // multiplier on all laser alpha
        float haze_alpha       = 0.35f;   // projector-to-wall haze beam alpha (0..1)
        float wall_glow_px     = 5.f;     // outer glow radius on wall hit (px)
        float beam_width_px    = 1.5f;    // solid wall segment width (px)
        // Room dimensions (metres)
        float room_half_width  = 6.f;    // half-width of room (wall is ±room_half_width)
        float room_height      = 5.f;    // floor-to-ceiling height
        float room_depth       = 20.f;   // depth of room (front to back wall)
        // Projection — half-size (metres) of the laser output on the back wall.
        // The laser scan field is square: output spans ±proj_scale in both X and Y,
        // centered at mid-wall height.  Set equal to maintain 1:1 aspect ratio.
        float proj_scale       = 3.0f;
    };
    Preview3dSettings preview_3d;

    // Per-laser position/orientation for the 3D preview.
    // Indexed by patched_outputs position -- matched by id.
    struct LaserPlacement3D {
        int   stream_id = -1;
        float pos_x = 0.f, pos_y = 3.f, pos_z = 0.5f;   // metres in room
        float yaw = 0.f;   // rotation around Y (left-right) in radians
        float pitch = 0.f; // rotation around X (up-down) in radians
    };
    std::vector<LaserPlacement3D> laser_placements;

    // Per-stream point buffers for 3D preview (synced from snapshot)
    struct StreamPreview {
        int         stream_id = -1;
        PointBuffer points;
    };
    std::vector<StreamPreview> stream_previews;

    // ── Timeline system ───────────────────────────────────────────────────────
    struct TimelineInfo {
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
    std::vector<TimelineInfo>  timelines;
    ProjectTimecodeConfig      tc_config;
    int                        selected_timeline_idx = -1;
};

} // namespace idhmfis
