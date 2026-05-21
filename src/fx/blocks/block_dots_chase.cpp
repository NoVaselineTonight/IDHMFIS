#include "block_dots_chase.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace idhmfis {

BlockDotsChase::BlockDotsChase() {
    p_dot_count_ = { "dot_count", 4.f,  0.f,  1.f, 32.f, 1.f,   "" };
    p_spacing_   = { "spacing",   0.f,  0.f,  0.f,  1.f, 0.01f, "" };
    p_speed_     = { "speed",     1.f,  0.f, -4.f,  4.f, 0.1f,  "Hz" };
    p_dot_size_  = { "dot_size",  2.f,  0.f,  1.f, 10.f, 1.f,   "" };
    p_fade_tail_ = { "fade_tail", 0.f,  0.f,  0.f,  1.f, 1.f,   "" };
}

std::vector<FxParam*> BlockDotsChase::params() {
    return { &p_dot_count_, &p_spacing_, &p_speed_, &p_dot_size_, &p_fade_tail_ };
}

const std::vector<FxParam*> BlockDotsChase::params() const {
    return { const_cast<FxParam*>(&p_dot_count_),
             const_cast<FxParam*>(&p_spacing_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_dot_size_),
             const_cast<FxParam*>(&p_fade_tail_) };
}

void BlockDotsChase::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    const float speed    = p_speed_.effective();
    anim_offset_ = std::fmod(anim_offset_ + speed * dt, 1.f);
    if (anim_offset_ < 0.f) anim_offset_ += 1.f;

    const int   dots     = std::clamp(static_cast<int>(std::round(p_dot_count_.effective())), 1, 32);
    const int   dot_size = std::clamp(static_cast<int>(std::round(p_dot_size_.effective())), 1, 10);
    const bool  fade     = (p_fade_tail_.effective() >= 0.5f);
    const int   n        = static_cast<int>(buf.size());
    if (n == 0) return;

    // Mark which indices should be lit
    // For each dot d, its centre index = (anim_offset_ + d/dots) * n
    for (int i = 0; i < n; ++i) {
        buf[static_cast<size_t>(i)].blanked = true;
    }

    for (int d = 0; d < dots; ++d) {
        float frac    = std::fmod(anim_offset_ + static_cast<float>(d) / static_cast<float>(dots), 1.f);
        int   centre  = static_cast<int>(frac * static_cast<float>(n)) % n;

        for (int s = -dot_size + 1; s < dot_size; ++s) {
            int idx = (centre + s + n) % n;
            buf[static_cast<size_t>(idx)].blanked = false;

            if (fade && s != 0) {
                float t    = 1.f - std::fabs(static_cast<float>(s)) / static_cast<float>(dot_size);
                auto& pt   = buf[static_cast<size_t>(idx)];
                pt.r = static_cast<uint8_t>(static_cast<float>(pt.r) * t);
                pt.g = static_cast<uint8_t>(static_cast<float>(pt.g) * t);
                pt.b = static_cast<uint8_t>(static_cast<float>(pt.b) * t);
            }
        }
    }
}

} // namespace idhmfis
