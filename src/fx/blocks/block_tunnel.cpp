#include "block_tunnel.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockTunnel::BlockTunnel() {
    p_speed_       = { "speed",       0.5f, 0.f, 0.1f, 4.f,  0.01f, "Hz" };
    p_rings_       = { "rings",       4.f,  0.f, 2.f,  16.f, 1.f,   "" };
    p_perspective_ = { "perspective", 0.5f, 0.f, 0.f,  1.f,  0.01f, "" };
    p_color_shift_ = { "color_shift", 0.f,  0.f, 0.f,  1.f,  1.f,   "" };
}

std::vector<FxParam*> BlockTunnel::params() {
    return { &p_speed_, &p_rings_, &p_perspective_, &p_color_shift_ };
}

const std::vector<FxParam*> BlockTunnel::params() const {
    return { const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_rings_),
             const_cast<FxParam*>(&p_perspective_),
             const_cast<FxParam*>(&p_color_shift_) };
}

void BlockTunnel::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed      = p_speed_.effective();
    const int   rings      = std::clamp(static_cast<int>(std::round(p_rings_.effective())), 2, 16);
    const float persp      = p_perspective_.effective();
    const bool  colorshift = (p_color_shift_.effective() >= 0.5f);
    const float kScale     = 32767.f;
    const float twopi      = static_cast<float>(2.0 * M_PI);

    anim_phase_ = std::fmod(anim_phase_ + speed * dt, 1.f);

    // Build concentric ring overlays into scratch
    scratch_ = buf;

    // Generate ring points (octagon approximation, 32 pts per ring)
    constexpr int kPtsPerRing = 32;
    scratch_.reserve(buf.size() + static_cast<size_t>(rings * kPtsPerRing));

    for (int r = 0; r < rings; ++r) {
        // ring phase: 0..1 animated so rings move inward
        float ring_t = std::fmod(static_cast<float>(r) / static_cast<float>(rings) + anim_phase_, 1.f);

        // perspective: smaller rings near 0 (far), bigger near 1 (close)
        float base_scale = ring_t; // 0=far, 1=close
        float scale = base_scale + persp * (1.f - base_scale) * base_scale;
        scale = std::clamp(scale, 0.01f, 1.f);

        // Hue shift based on ring index if enabled
        float hue = colorshift ? (static_cast<float>(r) / static_cast<float>(rings)) : 0.f;
        uint8_t rr, gg, bb;
        if (colorshift) {
            // Simple HSV: rotate through spectrum
            Color4 c = Color4::from_hsv(hue * 360.f, 1.f, 1.f);
            rr = c.r8(); gg = c.g8(); bb = c.b8();
        } else {
            rr = 255; gg = 255; bb = 255;
        }

        for (int i = 0; i < kPtsPerRing; ++i) {
            float a = twopi * static_cast<float>(i) / static_cast<float>(kPtsPerRing);
            LaserPoint p;
            p.x = static_cast<int16_t>(std::clamp(std::cos(a) * scale, -1.f, 1.f) * kScale);
            p.y = static_cast<int16_t>(std::clamp(std::sin(a) * scale, -1.f, 1.f) * kScale);
            p.r = rr; p.g = gg; p.b = bb;
            p.blanked = false;
            scratch_.push_back(p);
        }
    }

    buf.swap(scratch_);
}

} // namespace idhmfis
