#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Adds a bright scanning line that sweeps back and forth.
// blend mode 0=add (overlay), 1=gate (only passes points inside the line).
class BlockScannerLine : public IFxBlock {
public:
    BlockScannerLine();

    const char* name()        const override { return "ScannerLine"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Sweeping bright scan line. axis=0 horizontal, 1 vertical. blend=0 add, 1 gate.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_axis_;   // 0=H, 1=V
    FxParam p_speed_;  // Hz
    FxParam p_width_;  // 0..0.2
    FxParam p_color_r_; // 0..1
    FxParam p_color_g_; // 0..1
    FxParam p_color_b_; // 0..1
    FxParam p_blend_;  // 0=add, 1=gate

    float anim_pos_ = 0.f;   // -1..1 current line position
    float anim_dir_ = 1.f;   // +1 or -1
};

} // namespace idhmfis
