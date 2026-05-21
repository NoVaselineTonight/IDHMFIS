#include "block_envelope_follower.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockEnvelopeFollower::BlockEnvelopeFollower() {
    p_band_    = { "band",    0.f,  0.f, 0.f, 3.f,  1.f,   "" };
    p_attack_  = { "attack",  0.01f, 0.f, 0.001f, 1.0f,  0.001f, "s" };
    p_release_ = { "release", 0.1f, 0.f, 0.001f, 2.0f,  0.001f, "s" };
    p_gain_    = { "gain",    1.f,  0.f, 0.f,  4.f,  0.01f, "" };
    p_offset_  = { "offset",  0.f,  0.f, 0.f,  1.f,  0.01f, "" };
    p_output_  = { "output",  0.f,  0.f, 0.f,  1.f,  0.001f, "" };
}

std::vector<FxParam*> BlockEnvelopeFollower::params() {
    return { &p_band_, &p_attack_, &p_release_, &p_gain_, &p_offset_, &p_output_ };
}

const std::vector<FxParam*> BlockEnvelopeFollower::params() const {
    return { const_cast<FxParam*>(&p_band_),
             const_cast<FxParam*>(&p_attack_),
             const_cast<FxParam*>(&p_release_),
             const_cast<FxParam*>(&p_gain_),
             const_cast<FxParam*>(&p_offset_),
             const_cast<FxParam*>(&p_output_) };
}

void BlockEnvelopeFollower::process(PointBuffer& buf, float dt, const ExprContext& ctx) {
    (void)buf;  // does not modify the buffer

    // Select input band
    const int band = static_cast<int>(std::round(p_band_.effective()));
    float input = 0.f;
    switch (band) {
    case 0: input = ctx.sub;  break;
    case 1: input = ctx.mid;  break;
    case 2: input = ctx.hi;   break;
    case 3: input = ctx.rms;  break;
    default: input = ctx.rms; break;
    }
    input = std::clamp(input, 0.f, 1.f);

    // 1-pole IIR envelope follower
    // Coefficient derived from time constant: coeff = exp(-dt / tau)
    float coeff = 0.f;
    if (input > envelope_) {
        float tau = std::max(p_attack_.effective(), 1e-4f);
        coeff = (dt > 0.f) ? std::exp(-dt / tau) : 0.f;
    } else {
        float tau = std::max(p_release_.effective(), 1e-4f);
        coeff = (dt > 0.f) ? std::exp(-dt / tau) : 0.f;
    }
    envelope_ = coeff * envelope_ + (1.f - coeff) * input;

    // Apply gain and offset
    float out_val = std::clamp(p_offset_.effective() + p_gain_.effective() * envelope_,
                               p_output_.min_val, p_output_.max_val);
    p_output_.base_value = out_val;
}

} // namespace idhmfis
