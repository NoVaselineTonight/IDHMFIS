#include "block_color_ramp.h"
#include "../../core/types.h"
#include <algorithm>

namespace idhmfis {

BlockColorRamp::BlockColorRamp() {
    p_hue_start_  = { "hue_start",  0.f,  0.f, 0.f, 1.f, 0.01f, "" };
    p_hue_end_    = { "hue_end",    0.67f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_saturation_ = { "saturation", 1.f,  0.f, 0.f, 1.f, 0.01f, "" };
    p_brightness_ = { "brightness", 1.f,  0.f, 0.f, 1.f, 0.01f, "" };
}

std::vector<FxParam*> BlockColorRamp::params() {
    return { &p_hue_start_, &p_hue_end_, &p_saturation_, &p_brightness_ };
}

const std::vector<FxParam*> BlockColorRamp::params() const {
    return { const_cast<FxParam*>(&p_hue_start_),
             const_cast<FxParam*>(&p_hue_end_),
             const_cast<FxParam*>(&p_saturation_),
             const_cast<FxParam*>(&p_brightness_) };
}

void BlockColorRamp::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    (void)dt;
    const float hs  = p_hue_start_.effective();
    const float he  = p_hue_end_.effective();
    const float sat = p_saturation_.effective();
    const float bri = p_brightness_.effective();

    for (LaserPoint& pt : buf) {
        // Compute luminance of current colour (BT.601 coefficients)
        float r = static_cast<float>(pt.r) / 255.f;
        float g = static_cast<float>(pt.g) / 255.f;
        float b = static_cast<float>(pt.b) / 255.f;
        float lum = std::clamp(0.299f * r + 0.587f * g + 0.114f * b, 0.f, 1.f);

        // Map luminance -> hue in [0, 360)
        float hue = (hs + (he - hs) * lum) * 360.f;

        Color4 c = Color4::from_hsv(hue, sat, bri);
        pt.r = c.r8();
        pt.g = c.g8();
        pt.b = c.b8();
    }
}

} // namespace idhmfis
