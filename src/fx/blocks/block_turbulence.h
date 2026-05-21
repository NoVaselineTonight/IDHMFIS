#pragma once
#include "../fx_block.h"

namespace idhmfis {

// Noise-based distortion: displaces each point using value noise.
class BlockTurbulence : public IFxBlock {
public:
    BlockTurbulence();

    const char* name()        const override { return "Turbulence"; }
    const char* category()    const override { return "Distortion"; }
    const char* description() const override {
        return "Displaces each point using fractal value noise. Creates organic turbulent motion.";
    }

    void process(PointBuffer& buf, float dt, const ExprContext& ctx) override;

    std::vector<FxParam*>       params()       override;
    const std::vector<FxParam*> params() const override;

private:
    FxParam p_amount_;   // 0..0.5
    FxParam p_scale_;    // 0.5..4
    FxParam p_speed_;    // Hz 0..4
    FxParam p_octaves_;  // 1..4

    float anim_t_ = 0.f;

    // Smooth noise helpers
    static float hash2(float x, float y);
    static float smooth_noise(float x, float y);
    float fbm(float x, float y, int octaves) const;
};

} // namespace idhmfis
