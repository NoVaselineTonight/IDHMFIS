#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Grid Scan overlay: injects raster-style horizontal, vertical, or both-axis
// scan lines into the output buffer.  Line positions are animated by sweeping
// them across the field, creating the classic raster-scan EDM laser effect.
//
// Unlike ScannerLine (which gates/highlights existing points), GridScan
// APPENDS new lit line segments so the effect is visible over any generator.
//
// direction : 0=horizontal, 1=vertical, 2=both
// line_count: 2..32 simultaneous scan lines per axis
// stagger   : 0=all lines same phase, 1=evenly staggered phase per line
// speed     : sweep speed (Hz, each full crossing of the field per second)
class BlockGridScan : public IFxBlock {
public:
    BlockGridScan();

    const char* name()        const override { return "GridScan"; }
    const char* category()    const override { return "Geometry"; }
    const char* description() const override {
        return "Injects animated grid scan lines (H, V, or both). "
               "Appends scan lines on top of any generator output.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_direction_;    // 0=H, 1=V, 2=both
    FxParam p_line_count_;   // 2..32
    FxParam p_stagger_;      // 0..1
    FxParam p_line_pts_;     // points per line segment (2..32)
    FxParam p_color_r_;      // 0..1
    FxParam p_color_g_;      // 0..1
    FxParam p_color_b_;      // 0..1
    FxParam p_beat_sync_;    // 0=off, 1=on (speed follows beat phase)

    float anim_ = 0.f;       // cumulative animation time (seconds)
};

} // namespace idhmfis
