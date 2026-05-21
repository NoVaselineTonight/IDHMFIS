#include "block_stagger.h"
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace idhmfis {

BlockStagger::BlockStagger() {
    p_group_size_ = { "group_size", 4.f,  0.f,  1.f,  16.f, 1.f,  "" };
    p_phase_      = { "phase",      0.f,  0.f,  0.f,   1.f, 0.01f, "" };
    p_speed_      = { "speed",      2.f,  0.f,  0.f,  20.f, 0.1f, "Hz" };
    p_mode_       = { "mode",       0.f,  0.f,  0.f,   2.f, 1.f,  "" };
}

std::vector<FxParam*> BlockStagger::params() {
    return { &p_group_size_, &p_phase_, &p_speed_, &p_mode_ };
}

const std::vector<FxParam*> BlockStagger::params() const {
    return { const_cast<FxParam*>(&p_group_size_),
             const_cast<FxParam*>(&p_phase_),
             const_cast<FxParam*>(&p_speed_),
             const_cast<FxParam*>(&p_mode_) };
}

// lcg-based per-group random (stable each frame given same seed)
static uint32_t lcg(uint32_t x) {
    return x * 1664525u + 1013904223u;
}

void BlockStagger::process(PointBuffer& buf, float dt, const ExprContext& /*ctx*/) {
    anim_t_ += dt;

    const float speed      = p_speed_.effective();
    const float phase_off  = p_phase_.effective();
    const int   grp_sz     = std::clamp(static_cast<int>(std::round(p_group_size_.effective())), 1, 16);
    const int   mode       = static_cast<int>(std::round(p_mode_.effective()));
    const int   n          = static_cast<int>(buf.size());
    if (n == 0) return;

    const int num_groups = (n + grp_sz - 1) / grp_sz;

    for (int i = 0; i < n; ++i) {
        int  grp_idx = i / grp_sz;
        bool lit     = false;

        float group_phase = static_cast<float>(grp_idx) / static_cast<float>(std::max(num_groups, 1));
        (void)group_phase; // available for future chase-phase modes

        switch (mode) {
        case 0: // alternate: even groups lit when anim_t_*speed%1 < 0.5, odd groups opposite
        {
            float t   = std::fmod(anim_t_ * speed + phase_off, 1.f);
            bool  odd = (grp_idx & 1) != 0;
            lit = odd ? (t >= 0.5f) : (t < 0.5f);
            break;
        }
        case 1: // chase: one group lit at a time, rotating
        {
            float t         = std::fmod(anim_t_ * speed + phase_off, 1.f);
            int   lit_group = static_cast<int>(t * static_cast<float>(num_groups)) % num_groups;
            lit = (grp_idx == lit_group);
            break;
        }
        case 2: // random: each group independently flashes at speed Hz with random phase
        {
            uint32_t rnd = lcg(static_cast<uint32_t>(grp_idx));
            float rand_phase = static_cast<float>(rnd & 0xFFFFu) / 65535.f;
            float t = std::fmod(anim_t_ * speed + phase_off + rand_phase, 1.f);
            lit = (t < 0.5f);
            break;
        }
        default:
            lit = true;
            break;
        }

        if (!lit) {
            buf[static_cast<size_t>(i)].blanked = true;
        }
    }
}

} // namespace idhmfis
