#include "block_scatter.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

BlockScatter::BlockScatter() {
    p_amount_        = { "amount",        0.2f, 0.f,  0.f,  1.f,  0.01f, "" };
    p_speed_         = { "speed",         3.f,  0.f,  0.f, 10.f,  0.1f,  "Hz" };
    p_seed_          = { "seed",          0.f,  0.f,  0.f, 100.f, 1.f,   "" };
    p_restore_speed_ = { "restore_speed", 0.f,  0.f,  0.f,  2.f,  0.01f, "" };
}

std::vector<FxParam*> BlockScatter::params() {
    return { &p_amount_, &p_speed_, &p_seed_, &p_restore_speed_ };
}

const std::vector<FxParam*> BlockScatter::params() const {
    return { const_cast<FxParam*>(&p_amount_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_seed_),
             const_cast<FxParam*>(&p_restore_speed_) };
}

// Hash-based noise: returns value in [-1, 1]
float BlockScatter::noise(uint32_t idx, uint32_t seed) {
    uint32_t h = idx * 2654435761u ^ seed * 2246822519u;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return (static_cast<float>(h & 0xFFFFu) / 32767.5f) - 1.f;
}

void BlockScatter::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    anim_t_ += dt;
    // Wrap at 65536 to prevent float precision loss while keeping sin() phase continuous
    // (65536 is a power-of-2 so fmod is exact and sin phase is preserved for any speed)
    if (anim_t_ > 65536.f) anim_t_ -= 65536.f;
    const float amount  = p_amount_.effective();
    const float speed   = p_speed_.effective();
    const uint32_t seed = static_cast<uint32_t>(std::round(p_seed_.effective()));
    const float kScale  = 32767.f;
    const float kInv    = 1.f / kScale;
    const float twopi   = static_cast<float>(2.0 * M_PI);

    const int n = static_cast<int>(buf.size());
    for (int i = 0; i < n; ++i) {
        LaserPoint& pt = buf[static_cast<size_t>(i)];
        if (pt.blanked) continue;

        // Per-point stable random angle and phase
        float rand_x  = noise(static_cast<uint32_t>(i),     seed);
        float rand_y  = noise(static_cast<uint32_t>(i) + 1u, seed);
        float rand_ph = noise(static_cast<uint32_t>(i) + 2u, seed) * static_cast<float>(M_PI);

        float dx = rand_x * amount * std::sin(twopi * speed * anim_t_ + rand_ph);
        float dy = rand_y * amount * std::sin(twopi * speed * anim_t_ + rand_ph + 1.f);

        float nx = static_cast<float>(pt.x) * kInv + dx;
        float ny = static_cast<float>(pt.y) * kInv + dy;
        pt.x = static_cast<int16_t>(std::clamp(nx, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(ny, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
