#pragma once
// Zone types — routing configuration for multi-projector output.
// Each Zone represents one physical projector and its geometric/color mapping.

#include <string>
#include <cstdint>

namespace idhmfis {

// Per-zone geometric transform applied before DAC output.
struct ZoneTransform {
    float offset_x   = 0.f;    // -1..1
    float offset_y   = 0.f;    // -1..1
    float scale_x    = 1.f;    // 0.1..4
    float scale_y    = 1.f;
    float rotation   = 0.f;    // degrees
    float shear_x    = 0.f;
    float shear_y    = 0.f;
    // Keystone (4-corner warp): normalised offsets per corner
    float kstone_tl[2] = {};   // top-left x,y offset
    float kstone_tr[2] = {};
    float kstone_bl[2] = {};
    float kstone_br[2] = {};
    bool  use_keystone = false;
};

// A single output zone — one physical projector/DAC channel.
struct Zone {
    int         id          = 0;
    std::string name        = "Zone 1";
    bool        enabled     = true;
    bool        solo        = false;      // only this zone outputs
    bool        blind       = false;      // zone is active but muted from output
    std::string dac_id;                   // assigned DAC from DacRegistry
    int         dac_port    = 0;          // port on multi-output DACs
    int         target_pps  = 30000;      // point rate for this zone
    ZoneTransform transform;
    // Color correction per zone
    float       color_r     = 1.f;
    float       color_g     = 1.f;
    float       color_b     = 1.f;
    float       intensity   = 1.f;       // 0..1 master for this zone
    // Test pattern
    bool        show_test   = false;
    int         test_pattern = 0;        // 0=circle, 1=crosshair, 2=box, 3=star, 4=scanlines
};

} // namespace idhmfis
