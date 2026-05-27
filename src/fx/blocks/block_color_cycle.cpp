#include "block_color_cycle.h"
#include "../../core/types.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockColorCycle::BlockColorCycle() {
    p_hue_offset_  = { "hue_offset",  0.f,  0.f, 0.f, 1.f, 0.01f, "" };
    p_saturation_  = { "saturation",  1.f,  0.f, 0.f, 1.f, 0.01f, "" };
    p_brightness_  = { "brightness",  1.f,  0.f, 0.f, 1.f, 0.01f, "" };
    p_cycle_speed_ = { "cycle_speed", 0.5f, 0.f, 0.f, 4.f, 0.01f, "cyc/s" };
    p_mode_        = { "mode",        0.f,  0.f, 0.f, 1.f, 1.f,   "" };
}

std::vector<FxParam*> BlockColorCycle::params() {
    return { &p_hue_offset_, &p_saturation_, &p_brightness_, &p_cycle_speed_, &p_mode_ };
}

const std::vector<FxParam*> BlockColorCycle::params() const {
    return { const_cast<FxParam*>(&p_hue_offset_),
             const_cast<FxParam*>(&p_saturation_),
             const_cast<FxParam*>(&p_brightness_),
             const_cast<FxParam*>(&p_cycle_speed_),
             const_cast<FxParam*>(&p_mode_) };
}

// Compute hue from an existing RGB colour
static void rgb_to_hsv(float r, float g, float b, float& h, float& s, float& v) {
    float cmax = std::fmax(r, std::fmax(g, b));
    float cmin = std::fmin(r, std::fmin(g, b));
    float delta = cmax - cmin;
    v = cmax;
    s = (cmax > 1e-6f) ? delta / cmax : 0.f;

    if (delta < 1e-6f) {
        h = 0.f;
        return;
    }
    if (cmax == r)      h = 60.f * std::fmod((g - b) / delta, 6.f);
    else if (cmax == g) h = 60.f * ((b - r) / delta + 2.f);
    else                h = 60.f * ((r - g) / delta + 4.f);
    if (h < 0.f) h += 360.f;
}

void BlockColorCycle::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float base_hue = p_hue_offset_.effective();
    const float sat      = p_saturation_.effective();
    const float bri      = p_brightness_.effective();
    const float speed    = p_cycle_speed_.effective();
    const bool  by_time  = (p_mode_.effective() >= 0.5f);

    // Advance animated hue
    anim_hue_ = std::fmod(anim_hue_ + dt * speed, 1.f);

    const int n = static_cast<int>(buf.size());
    for (int idx = 0; idx < n; ++idx) {
        LaserPoint& pt = buf[static_cast<size_t>(idx)];

        // Derive current hue from existing colour
        float r = static_cast<float>(pt.r) / 255.f;
        float g = static_cast<float>(pt.g) / 255.f;
        float b = static_cast<float>(pt.b) / 255.f;
        float orig_h = 0.f, orig_s = 0.f, orig_v = 0.f;
        rgb_to_hsv(r, g, b, orig_h, orig_s, orig_v);

        float extra = 0.f;
        if (by_time) {
            extra = anim_hue_;
        } else {
            // by path: distribute hue shift across path position
            float t = (n > 1) ? static_cast<float>(idx) / static_cast<float>(n - 1) : 0.f;
            extra = anim_hue_ + t;
        }

        float hue_norm = std::fmod(orig_h / 360.f + base_hue + extra, 1.f);
        if (hue_norm < 0.f) hue_norm += 1.f;
        float new_h = hue_norm * 360.f;
        float new_s = sat;
        float new_v = bri * (orig_v > 0.f ? orig_v : 1.f);

        Color4 c = Color4::from_hsv(new_h, new_s, new_v);
        pt.r = c.r8();
        pt.g = c.g8();
        pt.b = c.b8();
    }
}

} // namespace idhmfis
