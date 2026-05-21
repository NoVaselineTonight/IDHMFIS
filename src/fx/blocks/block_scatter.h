#pragma once
#include "../fx_block.h"
#include <cstdint>

namespace idhmfis {

// Scatter: displaces each point by an oscillating random offset.
class BlockScatter : public IFxBlock {
public:
    BlockScatter();

    const char* name()        const override { return "Scatter"; }
    const char* category()    const override { return "Dots"; }
    const char* description() const override {
        return "Displaces each point by a random oscillating offset. Creates shimmering.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_amount_;        // 0..1
    FxParam p_speed_;         // Hz 0..10
    FxParam p_seed_;          // 0..100
    FxParam p_restore_speed_; // 0..2

    float anim_t_ = 0.f;

    // Simple pseudo-random: per-point stable offsets
    static float noise(uint32_t idx, uint32_t seed);
};

} // namespace idhmfis
