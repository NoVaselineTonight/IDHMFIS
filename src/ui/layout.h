#pragma once
// IDHMFIS — Panel layout declarations
// layout_draw() renders the full dockspace + all 5 panels for one frame.

#include "ui_state.h"
#include "command_palette.h"   // also pulls in imgui.h -> defines ImVec2, ImVec4
#include "keyboard_shortcuts.h"
#include "imgui.h"
#include "../zones/zone_types.h"
#include "image_import.h"
#include "../timeline/timeline_types.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declare ImTextureID so layout.h doesn't pull in backend headers
using ImTextureID_t = void*;

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  §B5 Expression editor state (per automation parameter)
// ─────────────────────────────────────────────────────────────────────────────
struct ExprEditorState {
    char text[512] = "sin(t * 2 * pi)";
    std::string compile_error;
    bool has_error = false;
    // preview plot data (256 samples)
    static constexpr int kPlotSamples = 256;
    float plot_y[kPlotSamples]{};
    bool plot_dirty = true;
};

// ─────────────────────────────────────────────────────────────────────────────
//  §B3 Frame editor state
// ─────────────────────────────────────────────────────────────────────────────
struct FrameEditorState {
    enum class Tool {
        Select, NodeEdit, Move, Rotate, Scale, Pivot, GroupScale,
        PremadeShapes,
        FilledShape,
        Pen, Line, Circle, Rect, Arc, Polygon, Star, Dot, Text, Lasso,
        Polyline, Bezier,
        ImportImage
    };
    Tool active_tool = Tool::Line;

    // Current in-progress primitive
    std::vector<ImVec2> wip_points;
    bool drawing = false;

    // Placed primitives (for display/export)
    struct PlacedPrim {
        std::string type;  // "polyline", "bezier", "arc", "circle", "text"
        std::vector<ImVec2> pts;
        ImVec4 color = {0.f, 0.898f, 1.f, 1.f};
        std::string text;
    };
    std::vector<PlacedPrim> placed;
    int selected_idx = -1;

    // Canvas transform
    ImVec2 canvas_origin = {0.f, 0.f};
    float canvas_zoom = 1.f;

    // Onion-skin
    bool show_onion = false;
    float onion_alpha = 0.3f;

    // Color picker state
    ImVec4 draw_color = {0.f, 0.898f, 1.f, 1.f};

    // Text input popup state
    bool text_popup_open = false;
    ImVec2 text_place_pos = {0.f, 0.f};
    char text_input_buf[256] = {};

    // ACT symmetry: when true, edits to any object propagate live to all symmetry copies
    bool symmetry_act_mode = false;

    // ── Symmetry mode ────────────────────────────────────────────────────────
    enum class SymmetryMode {
        None,
        MirrorX,    // bilateral mirror across vertical axis
        MirrorY,    // bilateral mirror across horizontal axis
        MirrorXY,   // both axes (4-fold)
        Radial2,    // 2-fold rotational
        Radial3,    // 3-fold rotational
        Radial4,    // 4-fold rotational
        Radial6,    // 6-fold rotational
        Radial8,    // 8-fold rotational
        Radial12,   // 12-fold rotational
    };
    SymmetryMode symmetry          = SymmetryMode::None;
    float        symmetry_center_x = 0.f;
    float        symmetry_center_y = 0.f;

    // ── Keyframe animation ───────────────────────────────────────────────────
    struct FrameKeyframe {
        float time_s = 0.f;
        std::vector<PlacedPrim> prims;  // full snapshot at this time
    };
    std::vector<FrameKeyframe> keyframes;
    float kf_current_time    = 0.f;  // current preview time (0..kf_duration)
    float kf_duration        = 4.f;  // total animation duration
    bool  kf_playing         = false;
    int   kf_selected        = -1;
    bool  show_keyframe_editor = false;

    // ── New object-based editing (replaces PlacedPrim for primary workflow) ──
    std::vector<LaserObject> objects;      // current frame objects
    uint64_t next_object_id = 1;           // monotonic ID counter

    // Interpolation type for keyframe transitions
    enum class InterpType { Linear, EaseInOut, Bezier, Hold };
    InterpType kf_interp = InterpType::EaseInOut;

    // Multi-frame / keyframe animation (object-based)
    struct AnimFrame {
        float                    time_s = 0.f;
        std::vector<LaserObject> objects; // snapshot of all objects at this time
        InterpType               interp = InterpType::EaseInOut;
    };
    std::vector<AnimFrame> anim_frames;
    float anim_current_time  = 0.f;
    float anim_duration      = 4.f;
    bool  anim_playing       = false;
    int   anim_selected_frame = -1;   // -1 = none

    // Selection state
    std::vector<uint64_t> selected_ids;   // IDs of selected objects
    bool                  box_selecting  = false;
    float                 box_x0 = 0.f, box_y0 = 0.f; // drag-select box start (canvas space)
    float                 box_x1 = 0.f, box_y1 = 0.f;

    // Per-object editing (shown in properties panel when object selected)
    float  edit_thickness  = 1.f;
    float  edit_size       = 0.05f;
    float  edit_r          = 0.f;
    float  edit_g          = 0.898f;
    float  edit_b          = 1.f;

    // Layer visibility toggles (for the 3-layer display)
    bool   show_global_layer   = true;
    bool   show_fx_layer       = true;
    bool   show_keyframe_layer = true;

    // Active layer being edited
    enum class ActiveLayer { Global, FX, Keyframe };
    ActiveLayer active_layer = ActiveLayer::Keyframe;

    // Object drag state
    bool              obj_dragging     = false;
    float             obj_drag_start_mx = 0.f; // mouse pos when drag started (canvas space)
    float             obj_drag_start_my = 0.f;
    std::vector<std::vector<LaserObjectPoint>> obj_drag_orig_pts; // original pts per selected obj

    // Control point drag state
    int   cp_drag_obj_idx = -1;  // index into objects[] being dragged
    int   cp_drag_pt_idx  = -1;  // which control point
    float cp_drag_orig_x  = 0.f;
    float cp_drag_orig_y  = 0.f;

    // Object name counter (for auto-naming)
    int   obj_name_counter = 1;

    // Object visibility (stored by ID)
    std::vector<uint64_t> hidden_ids;

    // Record-keyframe mode
    bool record_kf_mode = false;

    // ── Rotate tool state ────────────────────────────────────────────────────
    bool  rot_dragging    = false;
    float rot_start_angle = 0.f;   // mouse angle when drag began
    float rot_orig_angle  = 0.f;   // object angle when drag began
    float pivot_x         = 0.f;   // current transform pivot (canvas space ±1)
    float pivot_y         = 0.f;
    bool  pivot_custom    = false;  // true if user moved pivot manually

    // ── Scale tool state ─────────────────────────────────────────────────────
    bool  scale_dragging  = false;
    float scale_start_dist= 1.f;   // distance from pivot when drag began
    std::vector<std::vector<LaserObjectPoint>> scale_orig_pts; // per-selected obj

    // ── GroupScale tool state ─────────────────────────────────────────────────
    bool  gs_dragging         = false;
    int   gs_handle           = -1;     // 0-3: corners TL,TR,BR,BL; 4-7: edges T,R,B,L; -1=body
    float gs_box_x0           = 0.f;    // bounding box at drag start
    float gs_box_y0           = 0.f;
    float gs_box_x1           = 0.f;
    float gs_box_y1           = 0.f;
    float gs_drag_start_mx    = 0.f;    // mouse pos when drag started (normalized canvas -1..1)
    float gs_drag_start_my    = 0.f;
    std::vector<std::vector<LaserObjectPoint>> gs_orig_pts; // per-selected obj original points

    // ── Lasso select state ───────────────────────────────────────────────────
    bool                    lasso_active = false;
    std::vector<ImVec2>     lasso_pts;   // canvas-space polygon

    // ── Polygon/Star shape params ────────────────────────────────────────────
    int   shape_sides    = 6;   // polygon sides
    float shape_inner    = 0.4f; // star inner radius ratio
    bool  shape_is_star  = false;

    // ── Keyboard dwell/density per selected object ───────────────────────────
    int   edit_dwell     = 2;   // extra dwell points at vertices
    float edit_density   = 0.5f; // point density along segments

    // ── Arc tool WIP state ───────────────────────────────────────────────────
    int   arc_click_count = 0;   // 0=waiting, 1=center set, 2=start set

    // ── Move tool axis constraint ────────────────────────────────────────────
    bool  move_constrain_x = false;  // constrain to X axis (Y key)
    bool  move_constrain_y = false;  // constrain to Y axis (X key)

    // ── Filled Shapes tool state ─────────────────────────────────────────────
    struct FilledShapeState {
        enum class Shape {
            FilledRect, FilledCircle, FilledTriangle,
            FilledStar, FilledPolygon, FilledText
        };
        Shape  shape           = Shape::FilledRect;
        int    polygon_sides   = 6;      // for FilledPolygon (3..16)
        int    star_points     = 5;      // for FilledStar (3..12)
        float  fill_density    = 1.f;    // point density (0.1..2.0, unused for now)
        float  fill_line_gap   = 0.04f;  // gap between scanlines (0.005..0.1)
        float  col_r           = 1.f;
        float  col_g           = 1.f;
        float  col_b           = 1.f;
        float  size            = 0.3f;   // normalized half-size
        float  rot             = 0.f;    // rotation degrees
        bool   outline_only    = false;  // if true just draw outline
        // Text-specific
        std::string text_content  = "LASER";
        float       text_size     = 0.2f;
        float       letter_gap    = 0.05f;
        // Pending-place state
        bool   pending_place   = false;
        // Window open state (false = user closed panel -> switch tool back to Select)
        bool   open            = true;
    };
    FilledShapeState filled_shape;

    // ── Premade Shapes tool state ────────────────────────────────────────────
    struct PremadeShapesState {
        enum class FuncType {
            Linear, Quadratic, Cubic, Sine, Cosine,
            Circle, Spiral, Lissajous
        };
        FuncType func = FuncType::Sine;

        float param_a = 1.f;
        float param_b = 1.f;
        float param_c = 0.f;
        float param_d = 0.f;
        float lissajous_a     = 3.f;
        float lissajous_b     = 2.f;
        float lissajous_delta = 0.f;

        float x_min       = -1.f;
        float x_max       =  1.f;
        int   sample_count =  16;

        enum class ObjType { Dot, Line, Circle, Square, Triangle };
        ObjType obj_type = ObjType::Dot;
        float   obj_size = 0.05f;
        float   obj_rot  = 0.f;
        bool    obj_rot_tangent = false;

        float scale_x  = 0.8f;
        float scale_y  = 0.8f;
        float offset_x = 0.f;
        float offset_y = 0.f;

        float col_r = 1.f, col_g = 0.f, col_b = 0.f;
        bool  use_gradient = false;
        float col2_r = 0.f, col2_g = 0.f, col2_b = 1.f;

        bool show_curve_preview = true;

        // Window open state (false = user closed the panel -> switch tool back to Select)
        bool open = true;

        // Live canvas preview points — rebuilt every frame when tool is active
        PointBuffer preview_pts;
    };
    PremadeShapesState premade_shapes;

    // ── Image Import tool state ──────────────────────────────────────────────
    ImageImportState image_import;

    // ── Undo / redo stacks ───────────────────────────────────────────────────
    std::vector<std::vector<LaserObject>> fe_undo_stack;
    std::vector<std::vector<LaserObject>> fe_redo_stack;
    static constexpr int kMaxUndoDepth = 50;
};

// ─────────────────────────────────────────────────────────────────────────────
//  §Feeds  Per-output programmer feed state (MagicQ-style)
//  Keyed by sorted comma-joined active stream ID string, e.g. "1,2,3"
// ─────────────────────────────────────────────────────────────────────────────
struct ProgrammerFeedState {
    std::vector<LaserObject> objects;
    GlobalLayer              global;
    FxLayer                  fx;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Callbacks the layout layer fires back up to the application
// ─────────────────────────────────────────────────────────────────────────────
struct LayoutCallbacks {
    std::function<void()>               on_new_project;
    std::function<void()>               on_open_project;
    std::function<void(const std::string& path)> on_open_recent; // open a specific path from recent list
    std::function<void()>               on_save_project;
    std::function<void()>               on_save_as;
    std::function<void()>               on_undo;
    std::function<void()>               on_redo;
    std::function<void()>               on_play;
    std::function<void()>               on_pause;
    std::function<void()>               on_stop;
    std::function<void()>               on_fullscreen;
    std::function<void(int cue_idx)>                  on_cue_selected;
    std::function<void(int cue_idx)>                  on_cue_duplicate;
    std::function<void(int cue_idx)>                  on_cue_delete;
    std::function<void(double t)>                     on_seek;
    std::function<void()>                             on_help;
    // Called by inspector when user edits a generator param slider
    std::function<void(const char* name, float val)>  on_param_changed;
    // Called when master intensity changes
    std::function<void(float)>                        on_master_intensity;
    // Cue list transport
    std::function<void()>                             on_go;
    std::function<void()>                             on_back;
    std::function<void(int, int)>                     on_cuelist_jump;
    // FX block controls: (cue_idx, block_idx, ...)
    std::function<void(int, int, const char*, float)> on_fx_param_changed;
    std::function<void(int, int, bool)>               on_fx_enabled;
    std::function<void(int, int, bool)>               on_fx_bypassed;
    std::function<void(int, int, float)>              on_fx_wet;
    // §B5: Called when an expression compiles successfully
    std::function<void(const std::string& param_name, const std::string& expr_text)> on_expr_compiled;
    // §B3: Called when user exports a frame to a cue
    std::function<void(const std::vector<FrameEditorState::PlacedPrim>&)> on_frame_export;
    // §B3: Called when user exports the keyframe animation
    std::function<void(const std::vector<FrameEditorState::FrameKeyframe>&)> on_frame_animation_export;

    // Automation keyframe callbacks
    std::function<void(int track_idx, float time_s, float value, int curve)> on_automation_add_kf;
    std::function<void(int track_idx, int kf_idx)>                           on_automation_del_kf;
    std::function<void(int track_idx, int kf_idx, float time_s, float value)> on_automation_move_kf;
    // §B6: Called when user edits BPM via DragFloat in Transport panel
    std::function<void(float)> on_bpm_changed;
    // §Macro: Called when user clicks a macro button (slot 0-31)
    std::function<void(int)> on_macro_trigger;

    // Record mode controls
    std::function<void(bool)>                              on_set_record_mode;
    std::function<void(const std::string&, float, float)>  on_record_cue;    // name, fade_in, fade_out
    std::function<void(int)>                               on_delete_cue;
    std::function<void(int, float, float, float, float)>   on_set_cue_timing; // idx, fi, fo, di, hold
    std::function<void(int, const std::string&)>           on_rename_cue;

    // Output kill switch — fires when user toggles the OUTPUT ENABLE button
    std::function<void(bool)>                              on_output_enable;
    // Emergency shutoff — fires when PANIC button is clicked (true=activate, false=release)
    std::function<void(bool)>                              on_emergency_shutoff;
    // HDMI borderless window toggle
    std::function<void(bool)>                              on_hdmi_window_toggle;
    // Enabled output streams — master enable per stream kind (0=DAC,1=NDI,2=ArtNet,3=IDN)
    std::function<void(int kind, bool enabled)>            on_stream_type_enabled;

    // BAM editor callbacks
    std::function<void(int row, int col, int brush, uint8_t val)> on_bam_paint;
    std::function<void()>                                          on_bam_clear;
    std::function<void(bool)>                                      on_bam_enabled;
    // Safety — operator resets the scan-fail latch after resolving the cause
    std::function<void()>                                          on_reset_scan_fail;
    // Safety — enable or disable scan-fail protection (disable during NDI-only / no-laser use)
    std::function<void(bool)>                                      on_scan_fail_enabled;

    // ILDA import / export
    std::function<void()>                                  on_import_ilda;
    std::function<void()>                                  on_export_ilda;

    // Zone routing controls
    std::function<void(const Zone&)>  on_zone_set;    // create or update a zone
    std::function<void(int)>          on_zone_remove; // remove zone by id

    // Quick Show callbacks
    std::function<void(int page, int row, int col)>              on_quickshow_trigger;
    std::function<void(int page, int row, int col, int cue_idx)> on_quickshow_assign;
    std::function<void(int page, int row, int col)>              on_quickshow_clear;

    // LivePRO callbacks
    std::function<void(float x, float y)> on_livepro_xy;
    std::function<void(float scale)>      on_livepro_scale;
    std::function<void(float speed)>      on_livepro_speed;
    std::function<void(int slot)>         on_livepro_beat_trigger;

    // MIDI learn callbacks
    std::function<void(MidiBinding)>        on_midi_bind;
    std::function<void(const std::string&)> on_midi_unbind;
    std::function<void()>                   on_midi_clear;

    // Palette selection callbacks
    std::function<void(int)>                on_color_select;
    std::function<void(int)>                on_position_select;

    // Manual DMX output patching (Inspector faders)
    std::function<void(int universe, int channel, uint8_t value)> on_dmx_patch;

    // ArtNet output configuration (Settings panel)
    std::function<void(const std::string& ip, int univ_offset, bool enabled)> on_artnet_out_config;

    // Chaser cue editing
    std::function<void(int idx, FullCueEntry entry)> on_cuelist_update_entry;
    std::function<void()>                             on_new_chaser_cue;

    // Setup window callbacks
    struct NdiConfig {
        std::string source_name  = "IDHMFIS";
        int         width        = 1920;
        int         height       = 1080;
        int         fps_N        = 60;    // fps numerator  (e.g. 60000 for 59.94)
        int         fps_D        = 1;     // fps denominator (e.g. 1001 for 59.94)
        bool        enabled      = true;
        bool        clock_video  = true;  // NDI-clocked timing (stable, +1 frame latency)
    };
    std::function<void(const NdiConfig&)>            on_setup_ndi_config;
    std::function<void(int osc_port)>                on_setup_osc_port;
    std::function<void(float, float, float)>         on_setup_beam_params; // thickness, glow_r, glow_alpha

    // Playback fader callbacks (added at end to avoid breaking other agents' edits)
    std::function<void(int pb_id)>                         on_playback_go;
    std::function<void(int pb_id)>                         on_playback_stop;
    std::function<void(int pb_id)>                         on_playback_clear;
    std::function<void(int pb_id, bool)>                   on_playback_blind;
    std::function<void(int pb_id, float)>                  on_playback_intensity;
    std::function<void(int pb_id, const std::string&)>     on_playback_rename;
    std::function<void()>                                  on_playback_new;

    // Fix4b: Record frame into a playback slot
    std::function<void(int pb_id, FullCueEntry)> on_record_frame_to_playback;

    // INCLUDE / UPDATE — load a cue into the programmer, then update it back
    std::function<void(int cue_idx)>                    on_include_cue;
    std::function<void(int pb_id, int cue_idx)>         on_include_playback_cue;
    std::function<void(int cue_idx, FullCueEntry)>      on_update_included_cue;

    // Cuestack editor callbacks
    std::function<void(int pb_id, int cue_idx, FullCueEntry)> on_playback_cue_update;
    std::function<void(int pb_id, int cue_idx)>               on_playback_cue_delete;
    std::function<void(int pb_id, int cue_idx)>               on_playback_cue_jump;
    std::function<void(int pb_id)>                            on_playback_cue_back;

    // PBCONF: notify engine of updated per-playback configuration
    std::function<void(int pb_id, const UIState::PlaybackConf&)> on_playback_config;

    // Safety Blackout Zones — called whenever the blackout config changes in the UI
    std::function<void(const UIState::SafetyBlackoutConfig&)> on_safety_blackout_changed;

    // Autosave settings changed (enabled, interval_seconds)
    std::function<void(bool, int)> on_autosave_changed;

    // Developer logging toggle — true = enable, false = disable
    std::function<void(bool)> on_dev_logging_changed;

    // Network configuration applied — full config snapshot
    std::function<void(const UIState::NetworkConfig&)> on_network_config_apply;

    // Output patch changed — full list of current patch configs
    std::function<void(const std::vector<OutputStreamConfig>&)> on_output_patch_changed;
    // Active stream selection changed (which streams programmer targets)
    // First arg: old IDs (before change), second arg: new IDs (after change)
    std::function<void(const std::vector<int>&, const std::vector<int>&)> on_active_streams_changed;
    std::function<void(const std::vector<int>&)> on_mirrored_streams_changed;

    // ── Timeline callbacks ────────────────────────────────────────────────────
    std::function<void(TimelineDef)>                              on_timeline_create;
    std::function<void(const std::string&)>                       on_timeline_delete;
    std::function<void(const std::string&, const std::string&)>   on_timeline_rename;
    std::function<void(const std::string&, bool)>                 on_timeline_set_armed;
    std::function<void(const std::string&, bool)>                 on_timeline_set_record_armed;
    std::function<void(const std::string&)>                       on_timeline_play;
    std::function<void(const std::string&)>                       on_timeline_pause;
    std::function<void(const std::string&)>                       on_timeline_stop;
    std::function<void(const std::string&)>                       on_timeline_rewind;
    std::function<void(const std::string&, int64_t)>              on_timeline_seek;
    std::function<void(const std::string&, const std::string&)>   on_timeline_set_source;
    std::function<void(const std::string&, bool)>                 on_timeline_set_link;
    std::function<void(const std::string&, int64_t)>              on_timeline_set_offset;
    std::function<void(const std::string&, const std::string&)>   on_timeline_add_track;
    std::function<void(const std::string&, int)>                  on_timeline_remove_track;
    std::function<void(const std::string&, int, std::string, bool, bool, bool)> on_timeline_update_track;
    std::function<void(const std::string&, int, TimelineEvent)>   on_timeline_add_event;
    std::function<void(const std::string&, int, int64_t)>         on_timeline_remove_event;
    std::function<void(const std::string&, int, TimelineEvent)>   on_timeline_update_event;
    std::function<void(const std::string&)>                       on_timeline_wait_for_go;
    std::function<void(ProjectTimecodeConfig)>                    on_tc_config_changed;
    std::function<void(const std::string& id, AudioTrackDef)>     on_timeline_set_audio;
    std::function<void(const std::string& id)>                    on_timeline_clear_audio;

    // Explicit programmer clear — ChamSys rule: only this callback (wired to
    // cmd::ClearProgrammer) may wipe per-stream programmer content.
    std::function<void()> on_clear_programmer;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Programmer undo entry — snapshot taken before CLR wipes the programmer
// ─────────────────────────────────────────────────────────────────────────────
struct ProgrammerUndoEntry {
    FrameEditorState                                     frame_editor;
    GlobalLayer                                          programmer_global;
    FxLayer                                              programmer_fx_layer;
    std::unordered_map<std::string, ProgrammerFeedState> programmer_feeds;
    std::vector<int>                                     active_stream_ids;
    int                                                  active_group_id = -1;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Layout context — persists between frames
// ─────────────────────────────────────────────────────────────────────────────
struct LayoutContext {
    // Dockspace setup flag (one-time initialisation)
    bool dockspace_initialised = false;

    // Laser preview texture (set by app when NDI frame arrives)
    ImTextureID_t preview_texture = nullptr;
    int           preview_tex_w   = 0;
    int           preview_tex_h   = 0;

    // Inspector state
    int inspector_tab = 0;  // 0=CueParams, 1=DMX, 2=Automation

    // Cue library
    std::string cue_search;
    int         cue_context_idx = -1;

    // Timeline
    float  timeline_zoom    = 80.f;   // pixels per second
    double timeline_offset  = 0.0;    // horizontal scroll in seconds
    bool   timeline_drag    = false;

    // Panel focus order for Tab cycling
    int  panel_focus = 0;

    // Beam thickness parameter (persisted per-session)
    float beam_thickness = 1.5f;

    // FX stack panel state
    int fx_selected_block = -1;   // which block is currently expanded

    // §B5: Expression editor states (keyed by param name)
    std::unordered_map<std::string, ExprEditorState> expr_editors;

    // §B3: Frame editor state
    FrameEditorState frame_editor;

    // Cue Sheet state
    int  cuelist_selected_idx    = -1;
    bool cuelist_show_properties = false;

    // Zones panel state
    int zones_selected = -1;   // currently expanded zone id (-1 = none)

    // Setup window state
    bool      setup_window_open = false;  // toggled by File>Setup or gear button
    LayoutCallbacks::NdiConfig ndi_config;

    // Fix4a: REC arm state
    bool rec_armed = false;

    // Programmer blind mode (objects suppressed from output, still visible in editor)
    bool frame_editor_blind = false;

    // INCLUDE mode state
    bool incl_armed          = false;  // INCL button armed — next cue click loads into programmer
    int  included_cue_idx    = -1;     // index of main cuelist cue included (-1 = none)
    int  included_pb_id      = -1;     // playback ID for included PB cue (-1 = none)
    int  included_pb_cue_idx = -1;     // cue index within included playback (-1 = none)

    // Global layer and FX layer for programmer output
    GlobalLayer programmer_global;
    FxLayer     programmer_fx_layer;

    // MagicQ-style programmer feeds: saved state per output selection
    std::unordered_map<std::string, ProgrammerFeedState> programmer_feeds;

    // Undo stack for CLR (max 32 entries)
    std::vector<ProgrammerUndoEntry> programmer_undo_stack;

    // Panel open/close flags (used by ImGui::Begin with bool* p_open)
    bool panel_cue_lib_open    = true;
    bool panel_inspector_open  = true;
    bool panel_preview_open    = true;   // Laser Preview window
    bool panel_timeline_open   = true;
    bool panel_3d_open         = true;   // 3D Preview floating window
    bool frame_editor_open     = true;
    bool bpm_open              = true;
    bool livepro_open          = false;
    bool quickshow_open        = false;

    // Fix4d: which playback to show in the cuestack view (-1 = none)
    int cuestack_selected_pb = -1;
    bool cuestack_open       = false;

    // 5-mode view system
    enum class ViewMode { Programmer, Show, Setup, Patch, Safety };
    ViewMode active_view = ViewMode::Programmer;

    // Set to false whenever we enter Show view to force one-time layout reset
    bool show_layout_initialised = false;

    // PBCONF popup — which playback id has its config popup open (-1 = none)
    int pbconf_open_id = -1;

    // True when frame editor window has input focus (updated each frame by panel_frame_editor)
    bool fe_window_focused = false;

    // Safety blackout zone overlay toggle (preview panel toolbar)
    bool show_safety_overlay = true;

    // Autosave settings (mirrored from AutoSave object)
    bool autosave_enabled    = true;
    int  autosave_interval_s = 30;

    // Developer logging — persisted to settings.json next to the executable
    bool developer_logging = false;

    // ── Patch view state ──────────────────────────────────────────────────────
    int  patch_selected_id  = -1;   // which output is selected in the left table
    bool patch_add_modal    = false; // add-output modal open
    int  patch_add_type     = 0;    // 0=Laser, 1=NDI, 2=HDMI
    int  patch_add_count    = 1;    // how many to add
    int  patch_next_id      = 2;    // monotonic ID counter; 0=primary laser, 1=NDI are seeded at startup

    // ── STREAMS selection window ──────────────────────────────────────────────
    // Shown between programmer panel and preview.  Tracks which outputs are
    // selected for programmer output.  Empty = all outputs receive programmer.
    bool streams_window_open = true;

    // MOVE mode for streams grid — drag-and-drop reorder of output cells
    bool stream_move_mode = false;

    // MOVE mode for patch panel — drag-and-drop reorder of patched outputs
    bool patch_move_mode = false;

    // ── New floating panels ───────────────────────────────────────────────────
    bool clock_open       = false;
    bool snake_open       = false;
    bool conn_status_open = false;

    // ── Quit flag — set by File > Quit to trigger the existing quit flow ─────
    bool quit_requested = false;

    // ── Timeline view state ───────────────────────────────────────────────────
    std::string timeline_view_id;             // which timeline is open in the editor ("" = none)
    bool        timeline_view_open = true;    // false when user closes the editor window
    float       timeline_view_zoom    = 20.f; // pixels per frame
    double      timeline_view_scroll  = 0.0;  // horizontal scroll in frames
    bool        timeline_view_table   = false; // true = table/list view
    int         timeline_view_track_h = 32;   // track lane height in pixels
    int64_t     timeline_cursor_frame = 0;    // cursor/insert position
    int         timeline_selected_track_id = -1; // track id selected in editor (-1 = none)
    bool        timeline_marquee_active = false;
    float       timeline_marquee_x0   = 0.f;
    float       timeline_marquee_y0   = 0.f;
    float       timeline_marquee_x1   = 0.f;
    float       timeline_marquee_y1   = 0.f;
    std::vector<int64_t> timeline_selected_event_ids;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Main entry point
// ─────────────────────────────────────────────────────────────────────────────
void layout_draw(UIState&          state,
                 LayoutContext&    ctx,
                 LayoutCallbacks&  cbs,
                 CommandPalette&   palette,
                 ShortcutRegistry& shortcuts);

// ─────────────────────────────────────────────────────────────────────────────
//  Panel functions (also callable individually for testing)
// ─────────────────────────────────────────────────────────────────────────────
void panel_transport          (UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
void panel_operator_sidebar   (UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
void panel_cue_library(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
void panel_preview    (UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
void panel_inspector  (UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
void panel_timeline   (UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// §B3: Frame editor panel
void panel_frame_editor(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// §Macro: Macro/Executor grid panel
void panel_macros(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Zone routing panel
void panel_zones(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Safety/BAM editor panel
void panel_bam(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Quick Show cue grid panel
void panel_quickshow(UIState& state, LayoutCallbacks& cbs, bool* p_open = nullptr);
// LivePRO performance mode panel
void panel_livepro(UIState& state, LayoutCallbacks& cbs, bool* p_open = nullptr);
// MIDI Learn panel
void panel_midi_learn(UIState& state, LayoutCallbacks& cbs);
// Color and Position palette panel
void panel_palette(UIState& state, LayoutCallbacks& cbs);
// 10-slot playback fader bar (fixed strip at bottom of screen)
void panel_playback_bar(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Output patch view
void panel_patch(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// STREAMS selection window (shown in Programmer view between programmer and preview)
void panel_streams(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Timeline list panel (Show view left column)
void panel_timelines(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Timeline graphical editor panel (Show view main area)
void panel_timeline_view(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Floating clock / timecode panel
void panel_clock(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Floating snake game panel
void panel_snake(UIState& state, LayoutContext& ctx);
// Floating connection status panel
void panel_connection_status(UIState& state, LayoutContext& ctx);

} // namespace idhmfis
