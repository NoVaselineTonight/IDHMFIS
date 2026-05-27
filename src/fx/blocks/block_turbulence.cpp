#include "block_turbulence.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

BlockTurbulence::BlockTurbulence() {
    p_amount_  = { "amount",  0.1f, 0.f, 0.f, 0.5f, 0.01f, "" };
    p_scale_   = { "scale",   1.f,  0.f, 0.5f, 4.f, 0.1f,  "" };
    p_speed_   = { "speed",   0.5f, 0.f, 0.f,  4.f, 0.1f,  "Hz" };
    p_octaves_ = { "octaves", 2.f,  0.f, 1.f,  4.f, 1.f,   "" };
}

std::vector<FxParam*> BlockTurbulence::params() {
    return { &p_amount_, &p_scale_, &p_speed_, &p_octaves_ };
}

const std::vector<FxParam*> BlockTurbulence::params() const {
    return { const_cast<FxParam*>(&p_amount_),
             const_cast<FxParam*>(&p_scale_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_octaves_) };
}

// Hash-based smooth value noise
float BlockTurbulence::hash2(float x, float y) {
    // integer lattice hash
    int ix = static_cast<int>(std::floor(x));
    int iy = static_cast<int>(std::floor(y));
    uint32_t h = static_cast<uint32_t>(ix) * 1619u ^ static_cast<uint32_t>(iy) * 31337u;
    h ^= h >> 16;
    h *= 0x45d9f3bu;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFu) / 65535.f;
}

float BlockTurbulence::smooth_noise(float x, float y) {
    int   ix = static_cast<int>(std::floor(x));
    int   iy = static_cast<int>(std::floor(y));
    float fx = x - std::floor(x);
    float fy = y - std::floor(y);

    // Hermite interpolation
    float ux = fx * fx * (3.f - 2.f * fx);
    float uy = fy * fy * (3.f - 2.f * fy);

    float a = hash2(static_cast<float>(ix),     static_cast<float>(iy));
    float b = hash2(static_cast<float>(ix + 1), static_cast<float>(iy));
    float c = hash2(static_cast<float>(ix),     static_cast<float>(iy + 1));
    float d = hash2(static_cast<float>(ix + 1), static_cast<float>(iy + 1));

    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

float BlockTurbulence::fbm(float x, float y, int octaves) const {
    float v     = 0.f;
    float amp   = 0.5f;
    float freq  = 1.f;
    float total = 0.f;
    for (int o = 0; o < octaves; ++o) {
        v     += smooth_noise(x * freq, y * freq) * amp;
        total += amp;
        amp   *= 0.5f;
        freq  *= 2.f;
    }
    return (total > 0.f) ? v / total : 0.f;
}

void BlockTurbulence::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    anim_t_ += dt;
    if (anim_t_ > 65536.f) anim_t_ -= 65536.f;
    const float amount  = p_amount_.effective();
    const float scale   = p_scale_.effective();
    const float speed   = p_speed_.effective();
    const int   octaves = std::clamp(static_cast<int>(std::round(p_octaves_.effective())), 1, 4);
    const float kScale  = 32767.f;
    const float kInv    = 1.f / kScale;

    float tz = anim_t_ * speed;

    for (LaserPoint& pt : buf) {
        if (pt.blanked) continue;

        float nx = static_cast<float>(pt.x) * kInv;
        float ny = static_cast<float>(pt.y) * kInv;

        // Sample noise at two offset positions for dx and dy
        float noise_x = fbm(nx * scale + tz,        ny * scale,        octaves);
        float noise_y = fbm(nx * scale,              ny * scale + tz + 3.7f, octaves);

        // Map noise 0..1 to -1..1
        float dx = (noise_x * 2.f - 1.f) * amount;
        float dy = (noise_y * 2.f - 1.f) * amount;

        float ox = nx + dx;
        float oy = ny + dy;
        pt.x = static_cast<int16_t>(std::clamp(ox, -1.f, 1.f) * kScale);
        pt.y = static_cast<int16_t>(std::clamp(oy, -1.f, 1.f) * kScale);
    }
}

} // namespace idhmfis
