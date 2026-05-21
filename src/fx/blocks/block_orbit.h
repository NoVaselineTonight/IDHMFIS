#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Orbits the entire pattern around a centre point in a circle or ellipse.
class BlockOrbit : public IFxBlock {
public:
    BlockOrbit();

    const char* name()        const override { return "Orbit"; }
    const char* category()    const override { return "Movement"; }
    const char* description() const override {
        return "Translates the whole pattern in a circular/elliptical orbit over time.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_radius_;        // 0..1
    FxParam p_speed_;         // Hz, -10..10
    FxParam p_phase_;         // 0..360 deg
    FxParam p_ellipse_ratio_; // 0..1 (1=circle, 0=flat)

    float anim_phase_ = 0.f;  // accumulated phase in radians
};

} // namespace idhmfis
