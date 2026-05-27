#pragma once
// Project document — the single source of truth for a show.
// All persistent state lives here. The engine reads snapshots; the UI writes
// through Command objects so every change is undoable.
//
// This header includes nlohmann/json.hpp (header-only, no link cost) because
// the to_json() / from_json() member signatures require the complete type.
// For ADL free-function overloads of all contained types, include
// serialization.h instead.

#include <string>
#include <vector>
#include <stdexcept>

#include "../core/types.h"
#include "cue.h"
#include "../cuelist/cuelist_types.h"
#include "palette.h"
#include "../timeline/timeline_types.h"

// nlohmann::json is a type alias for a complex template.  Rather than
// forward-declaring it (which would require repeating all template params),
// we simply include the header here.  It is header-only so there is no
// link-time overhead; compilation-time cost is paid once per TU anyway.
#include <nlohmann/json.hpp>

namespace idhmfis {

// Current project file schema version.  Update this single constant whenever
// the schema changes; all comparison and migration code references it.
static constexpr const char* kCurrentSchemaVersion = "2.9.0";

// ─────────────────────────────────────────────────────────────────────────────
//  DMX fixture profile — describes how a range of DMX channels maps to
//  GeneratorParams fields.
// ─────────────────────────────────────────────────────────────────────────────
struct DmxFixtureProfile {
    std::string name;

    struct ChannelDef {
        int         offset;       // 1-based within this profile's address block
        std::string param_name;   // maps to a GeneratorParams field (or special)
        float       scale_min = 0.f;
        float       scale_max = 1.f;
    };

    std::vector<ChannelDef> channels;

    // Build the canonical 40-channel fixture profile described in the spec.
    static DmxFixtureProfile make_default_40ch();
};

// ─────────────────────────────────────────────────────────────────────────────
//  DMX patch entry — binds a fixture profile to a universe+address
// ─────────────────────────────────────────────────────────────────────────────
struct DmxPatch {
    int               universe   = 0;
    int               start_addr = 1;   // 1-512
    DmxFixtureProfile profile;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Cue list entry — references a cue and adds timeline/playback metadata
// ─────────────────────────────────────────────────────────────────────────────
struct CueListEntry {
    std::string cue_id;
    double      in_time           = 0.0;
    double      out_time          = 0.0;   // 0 = use cue's own duration
    float       fade_in           = 0.5f;  // seconds
    float       fade_out          = 0.5f;  // seconds
    bool        auto_next         = false;
    double      auto_next_delay   = 0.0;   // seconds after out_time
};

// ─────────────────────────────────────────────────────────────────────────────
//  Project — the top-level document
// ─────────────────────────────────────────────────────────────────────────────
struct Project {
    // --- Document identity ---
    std::string schema_version = "2.9.0";
    std::string name;
    std::string author;
    std::string created_at;    // ISO 8601 UTC
    std::string modified_at;   // ISO 8601 UTC

    // --- Show content ---
    std::vector<Cue>          cues;
    std::vector<CueListEntry> cue_list;
    std::vector<DmxPatch>     dmx_patches;

    // Extended cue list — populated by the engine when using the MagicQ-class CueList.
    // Supersedes cue_list when non-empty. Stored separately from the legacy field.
    std::vector<FullCueEntry> full_cue_list;

    // ── MagicQ-style multiple playbacks ──────────────────────────────────────
    std::vector<PlaybackDef> playbacks;

    // ── Timeline system ───────────────────────────────────────────────────────
    std::vector<TimelineDef>    timelines;
    ProjectTimecodeConfig       tc_config;

    // --- Global render settings ---
    int   point_rate     = kDefaultPointRate;
    int   ndi_width      = kDefaultNDIWidth;
    int   ndi_height     = kDefaultNDIHeight;
    int   ndi_fps        = kDefaultNDIFPS;   // kept for backward-compat serialization
    int   ndi_fps_N      = kDefaultNDIFPS;   // fps numerator  (e.g. 60000 for 59.94)
    int   ndi_fps_D      = 1;                // fps denominator (e.g. 1001 for 59.94)
    bool  ndi_clock_video = true;            // NDI-clocked timing vs. app-clocked
    int   artnet_universe = kDefaultUniverse;

    // --- UI key bindings (ImGuiKey values; 0 = none) ---
    int  bpm_tap_key            = 0;
    int  emergency_shutoff_key  = 0;  // Emergency Shutoff bindable key

    // --- Color palettes ---
    ColorPalette    color_palette;
    PositionPalette position_palette;

    // --- Color swatches (32 user-definable slots) ---
    static constexpr int kNumSwatches = 32;
    struct ColorSwatch {
        float r = 0.f, g = 0.f, b = 0.f;
        bool  used = false;
    };
    ColorSwatch color_swatches[kNumSwatches]{};

    // --- Safety blackout configuration ---
    struct SafetyBlackoutConfig {
        bool enabled = true;

        struct BorderCrop {
            float left   = 0.f;
            float right  = 0.f;
            float top    = 0.f;
            float bottom = 0.f;
            float tilt   = 0.f;
        } borders;

        struct BlockZone {
            bool        enabled   = true;
            std::string name      = "Zone";
            float       cx        = 0.f, cy = 0.f;
            float       hw        = 0.1f, hh = 0.1f;
            float       angle_deg = 0.f;
        };
        std::vector<BlockZone> zones;
    };
    SafetyBlackoutConfig safety_blackout;

    // --- Output patch (all configured output streams, saved/restored with show) ---
    std::vector<OutputStreamConfig> output_patch;
    int                             output_patch_next_id = 0;  // next unique stream ID

    // --- Output groups (logical groupings of streams for combined addressing) ---
    struct OutputGroup {
        int              id           = 0;
        std::string      name;
        std::vector<int> member_ids;
        std::vector<int> mirrored_ids;
    };
    std::vector<OutputGroup> output_groups;
    int                      output_group_next_id = 1;

    // --- Network configuration (all protocol inputs / sidecars) ---
    struct NetworkConfig {
        bool        iface_auto          = true;
        int         iface_index         = 0;
        bool        artnet_enabled      = false;
        bool        artnet_auto         = true;
        int         artnet_universe     = 0;
        int         artnet_net          = 0;
        int         artnet_subnet       = 0;
        std::string artnet_listen_ip    = "0.0.0.0";
        int         artnet_port         = 6454;
        bool        artnet_merge_htp    = true;
        int         artnet_merge_mode   = 0;
        int         artnet_priority     = 100;
        bool        sacn_enabled        = false;
        bool        sacn_auto           = true;
        int         sacn_universe       = 1;
        int         sacn_priority       = 100;
        std::string sacn_multicast_ip   = "239.255.0.1";
        int         sacn_port           = 5568;
        bool        sacn_per_universe   = true;
        bool        osc_in_enabled      = true;
        bool        osc_in_auto         = true;
        int         osc_in_port         = 7700;
        std::string osc_in_ip           = "0.0.0.0";
        bool        osc_out_enabled     = false;
        bool        osc_out_auto        = true;
        std::string osc_out_ip          = "127.0.0.1";
        int         osc_out_port        = 7701;
        std::string osc_prefix          = "/idhmfis/";
        bool        citp_enabled        = false;
        bool        citp_auto           = true;
        int         citp_tcp_port       = 6430;
        std::string citp_multicast_group= "239.224.0.180";
        int         citp_multicast_port = 4809;
        std::string citp_source_name    = "IDHMFIS";
        bool        citp_respond_capture= true;
        bool        idn_enabled         = false;
        bool        idn_auto            = true;
        std::string idn_broadcast_addr  = "255.255.255.255";
        int         idn_port            = 7255;
        int         idn_channel         = 0;
        bool        cls_enabled         = false;
        bool        cls_auto            = true;
        int         cls_tcp_port        = 7256;
        int         cls_udp_port        = 7256;
        bool        cls_udp_broadcast   = true;
        bool        etherdream_enabled  = false;
        bool        etherdream_auto     = true;
        float       etherdream_timeout  = 2.0f;
        std::string etherdream_preferred_ip;
        bool        ndi_net_auto        = true;
        std::string ndi_net_source_name = "IDHMFIS Laser Preview";
        int         ndi_net_bandwidth   = 1;
        int         ndi_net_fps         = 30;
        int         dmx_universe_offset  = 0;  // shifts the universe number in all DMX trigger lookups
    };
    NetworkConfig net_config;

    // --- Output configuration (NDI/HDMI/DAC/VCam render settings) ---
    struct OutputConfig {
        bool        ndi_enabled   = true;
        std::string ndi_name      = "IDHMFIS";
        int         ndi_width     = 1920;
        int         ndi_height    = 1080;
        int         ndi_fps       = 60;
        int         ndi_fps_N     = 60;
        int         ndi_fps_D     = 1;
        bool        ndi_clock_video = true;
        float       beam_radius   = 2.5f;
        float       glow_radius   = 8.f;
        float       glow_alpha    = 0.25f;
        bool        hdmi_window_enabled    = false;
        bool        dac_enabled            = true;
        bool        idn_stream_enabled     = false;
        bool        virtual_camera_enabled = false;
        // Enabled output streams — master enables per stream kind (default true)
        bool        stream_type_dac_enabled    = true;
        bool        stream_type_ndi_enabled    = true;
        bool        stream_type_artnet_enabled = true;
        bool        stream_type_idn_enabled    = true;
    };
    OutputConfig output_config;

    // --- 3D Preview settings ---
    struct Preview3dSettings {
        bool  scan_mode       = false;
        float scan_speed      = 0.8f;
        int   trail_pct       = 20;
        float beam_brightness = 1.0f;
        float haze_alpha      = 0.35f;
        float wall_glow_px    = 5.f;
        float beam_width_px   = 1.5f;
        float room_half_width = 6.f;   // half-width of room in metres
        float room_height     = 5.f;   // floor-to-ceiling height
        float room_depth      = 20.f;  // depth (front to back wall)
        float proj_scale      = 3.0f;  // laser projection half-size on back wall
    };
    Preview3dSettings preview_3d;

    // --- Laser placements (3D preview per-laser positions) ---
    struct LaserPlacement3D {
        int   stream_id = -1;
        float pos_x = 0.f, pos_y = 3.f, pos_z = 0.5f;
        float yaw = 0.f;
        float pitch = 0.f;
    };
    std::vector<LaserPlacement3D> laser_placements;

    // --- Color input mode (serialized as int) ---
    int color_input_mode = 0;  // 0=RGBPercent, 1=RGBAbs, 2=CMY, 3=HSI

    // --- Otaniemi Mode settings ---
    struct OtaniemiConfig {
        bool  enabled          = false;
        float line_thickness   = 4.0f;
        bool  auto_fill_shapes = true;
        float brightness_boost = 1.3f;
        float glow_radius      = 8.0f;
    };
    OtaniemiConfig otaniemi;

    // --- Active stream IDs (programmer targets) ---
    std::vector<int> active_stream_ids;

    // --- Programmer broadcast mode ---
    // true = send to all enabled outputs (ignores active_stream_ids).
    // Default true for backward compat with files that don't have this field.
    bool broadcast_to_all = true;

    // --- UI layout state (persisted to show file) ---
    struct LayoutSave {
        int  active_view              = 0;    // 0=Programmer, 1=Show, 2=Setup, 3=Patch, 4=Safety
        bool show_layout_initialised  = false;
        bool dockspace_initialised    = false;
    };
    LayoutSave ui_layout;

    // --- ArtNet output settings (persisted to project file) ---
    std::string artnet_out_ip              = "2.255.255.255";
    int         artnet_out_universe_offset = 0;
    bool        artnet_out_enabled         = false;

    // --- Post-processing / appearance ---
    float beam_thickness = 2.0f;
    float bloom_radius   = 8.0f;
    float haze_density   = 0.4f;
    float exposure       = 1.0f;
    bool  film_grain     = false;

    // ------------------------------------------------------------------
    // Utility
    // ------------------------------------------------------------------

    // Returns the index of the cue with the given id, or -1.
    int find_cue(const std::string& id) const {
        for (int i = 0; i < static_cast<int>(cues.size()); ++i)
            if (cues[i].id == id) return i;
        return -1;
    }

    // Generate a new UUID v4 string (RFC 4122, variant 1, version 4).
    static std::string new_id();

    // ------------------------------------------------------------------
    // Serialisation — full implementations in serialization.cpp.
    // Call sites must #include "serialization.h" (not just project.h)
    // so that the nlohmann::json definition is visible before use.
    // ------------------------------------------------------------------
    nlohmann::json  to_json() const;
    static Project  from_json(const nlohmann::json& j);

    // Save to path (atomic: write temp file then rename).
    // Returns true on success; throws std::runtime_error on failure.
    bool save(const std::string& path) const;

    // Load from path. Throws std::runtime_error on failure.
    static Project load(const std::string& path);

    // ------------------------------------------------------------------
    // Schema migration helpers (used by from_json)
    // ------------------------------------------------------------------
    // Returns true if the given version string is older than "1.0.0".
    static bool needs_migration(const std::string& schema_ver);
};

// ─────────────────────────────────────────────────────────────────────────────
//  ProjectLoadResult — returned by project_load_full() for graceful degradation
// ─────────────────────────────────────────────────────────────────────────────
struct ProjectLoadResult {
    Project                  project;
    bool                     ok                = false;
    bool                     integrity_ok      = true;
    bool                     integrity_present = false;
    std::vector<std::string> warnings;
    std::string              fatal_error;
};

// Load with integrity check and per-section graceful degradation.
// Never throws — errors are reported in the returned result.
ProjectLoadResult project_load_full(const std::string& path);

// ─────────────────────────────────────────────────────────────────────────────
//  ProjectLoadError — structured error type for load failures
// ─────────────────────────────────────────────────────────────────────────────
struct ProjectLoadError : std::runtime_error {
    enum class Reason {
        FileNotFound,
        ParseError,
        SchemaMismatch,
        MissingField,
        InvalidData,
    };
    Reason reason;
    explicit ProjectLoadError(const std::string& msg, Reason r)
        : std::runtime_error(msg), reason(r) {}
};

} // namespace idhmfis
