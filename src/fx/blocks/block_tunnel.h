#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Tunnel effect: adds concentric rings that animate toward centre.
class BlockTunnel : public IFxBlock {
public:
    BlockTunnel();

    const char* name()        const override { return "Tunnel"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Adds concentric rings scaling toward centre over time, simulating a tunnel.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_speed_;       // Hz 0.1..4
    FxParam p_rings_;       // 2..16
    FxParam p_perspective_; // 0..1
    FxParam p_color_shift_; // 0=off, 1=on

    float anim_phase_ = 0.f;

    PointBuffer scratch_;
};

} // namespace idhmfis
