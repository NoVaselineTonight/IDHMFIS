#pragma once
#include "../fx_block.h"

namespace idhmfis {

// 2D rotation of all points around a configurable centre, with optional auto-spin.
class BlockRotate : public IFxBlock {
public:
    BlockRotate();

    const char* name()        const override { return "Rotate"; }
    const char* category()    const override { return "Geometry"; }
    const char* description() const override {
        return "Rotates all points around a centre point. Speed auto-animates the angle.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_angle_;     // -pi..pi (static offset)
    FxParam p_center_x_;  // -1..1
    FxParam p_center_y_;  // -1..1
    FxParam p_speed_;     // rad/s  (-20..20)

    float anim_angle_ = 0.f;  // accumulated spin
};

} // namespace idhmfis
