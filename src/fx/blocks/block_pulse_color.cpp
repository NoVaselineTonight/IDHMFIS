#include "block_pulse_color.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockPulseColor::BlockPulseColor() {
    p_color_a_r_ = { "color_a_r", 1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_a_g_ = { "color_a_g", 0.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_a_b_ = { "color_a_b", 0.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_b_r_ = { "color_b_r", 0.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_b_g_ = { "color_b_g", 0.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_color_b_b_ = { "color_b_b", 1.f, 0.f, 0.f, 1.f, 0.01f, "" };
    p_speed_     = { "speed",     2.f, 0.f, 0.1f, 20.f, 0.1f, "Hz" };
    p_waveform_  = { "waveform",  0.f, 0.f, 0.f,  2.f,  1.f, "" };
    p_sync_beat_ = { "sync_beat", 0.f, 0.f, 0.f,  1.f,  1.f, "" };
}

std::vector<FxParam*> BlockPulseColor::params() {
    return { &p_color_a_r_, &p_color_a_g_, &p_color_a_b_,
             &p_color_b_r_, &p_color_b_g_, &p_color_b_b_,
             &p_speed_, &p_waveform_, &p_sync_beat_ };
}

const std::vector<FxParam*> BlockPulseColor::params() const {
    return { const_cast<FxParam*>(&p_color_a_r_),
             const_cast<FxParam*>(&p_color_a_g_),
             const_cast<FxParam*>(&p_color_a_b_),
             const_cast<FxParam*>(&p_color_b_r_),
             const_cast<FxParam*>(&p_color_b_g_),
             const_cast<FxParam*>(&p_color_b_b_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_waveform_),
             const_cast<FxParam*>(&p_sync_beat_) };
}

void BlockPulseColor::process(PointBuffer& buf, float dt, const ExprContext& ctx) {
    const float speed     = p_speed_.effective();
    const bool  sync_beat = (p_sync_beat_.effective() >= 0.5f);
    const int   waveform  = static_cast<int>(std::round(p_waveform_.effective()));

    float phase;
    if (sync_beat) {
        phase = static_cast<float>(ctx.beat);
    } else {
        anim_t_ += dt * speed;
        phase = std::fmod(anim_t_, 1.f);
    }

    float t;
    switch (waveform) {
    case 1: // square
        t = (phase < 0.5f) ? 0.f : 1.f;
        break;
    case 2: // saw
        t = phase;
        break;
    default: // sine
        t = 0.5f + 0.5f * std::sin(phase * static_cast<float>(2.0 * M_PI));
        break;
    }

    const float ar = p_color_a_r_.effective();
    const float ag = p_color_a_g_.effective();
    const float ab = p_color_a_b_.effective();
    const float br = p_color_b_r_.effective();
    const float bg = p_color_b_g_.effective();
    const float bb = p_color_b_b_.effective();

    const uint8_t outr = static_cast<uint8_t>(std::clamp(ar + (br - ar) * t, 0.f, 1.f) * 255.f);
    const uint8_t outg = static_cast<uint8_t>(std::clamp(ag + (bg - ag) * t, 0.f, 1.f) * 255.f);
    const uint8_t outb = static_cast<uint8_t>(std::clamp(ab + (bb - ab) * t, 0.f, 1.f) * 255.f);

    for (LaserPoint& pt : buf) {
        if (pt.blanked) continue;
        pt.r = outr;
        pt.g = outg;
        pt.b = outb;
    }
}

} // namespace idhmfis
