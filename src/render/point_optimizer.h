#pragma once
// §B1 — 7-Step Point Optimizer
// Processes a PointBuffer through a deterministic pipeline to produce
// output suitable for galvo DAC playback.

#include "../core/types.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────────────────────────────────────
struct OptimizerConfig {
    int   target_pps             = 30000;  // target output points per second
    float blank_dwell            = 8.f;    // extra dwell points at blank-to-lit transitions
    float corner_dwell           = 3.f;    // extra dwell points at sharp corners
    float corner_angle_threshold = 0.3f;   // radians — angle above which a corner dwell is inserted
    bool  enable_reorder         = true;   // nearest-neighbour segment reordering
    bool  enable_overscan_clip   = true;
    float overscan_margin        = 0.05f;  // clip outside [-1-m .. 1+m]
};

// ─────────────────────────────────────────────────────────────────────────────
//  PointOptimizer — apply 7-step pipeline
// ─────────────────────────────────────────────────────────────────────────────
class PointOptimizer {
public:
    explicit PointOptimizer(const OptimizerConfig& cfg = {});

    void        set_config(const OptimizerConfig& cfg);
    PointBuffer optimize(const PointBuffer& input) const;

private:
    // Pipeline steps (applied in order)
    PointBuffer step_flatten        (const PointBuffer& in) const;
    PointBuffer step_anchor         (const PointBuffer& in) const;
    PointBuffer step_color_edge_interp(const PointBuffer& in) const;
    PointBuffer step_blanking       (const PointBuffer& in) const;
    PointBuffer step_path_ordering  (const PointBuffer& in) const;
    PointBuffer step_density_norm   (const PointBuffer& in) const;
    PointBuffer step_overscan_clip  (const PointBuffer& in) const;

    OptimizerConfig cfg_;
};

} // namespace idhmfis
