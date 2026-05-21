// gen_particles.cpp — Deterministic particle field.
//
// Each particle's position is derived from a seed + t via a simple
// deterministic chaos function, so no mutable state is needed.
// Particles have velocity, gravity, and lifetime all encoded in the chaos.
//
// param_a: particle count multiplier (0..1 → 10..200)
// param_b: gravity   (0..1 → -1..+1 normalised)
// param_c: lifespan  (0..1 → 0.5..4 seconds)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

// Fast deterministic pseudo-random float in [-1, 1] from seed integer
static inline float det_rand(int seed)
{
    // XorShift32 + normalise
    unsigned u = static_cast<unsigned>(seed);
    u ^= u << 13;
    u ^= u >> 17;
    u ^= u << 5;
    return static_cast<float>(u & 0xFFFFu) / 32767.5f - 1.f;
}

class GenParticles final : public IGenerator {
public:
    const char* name()         const override { return "particles"; }
    const char* display_name() const override { return "Particles"; }
    const char* param_a_label()const override { return "Count"; }
    const char* param_b_label()const override { return "Gravity"; }
    const char* param_c_label()const override { return "Lifespan"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;

        int count = 10 + static_cast<int>(p.param_a * 190.f);
        count = std::min(count, p.point_count);

        float gravity  = (p.param_b * 2.f - 1.f) * 0.5f; // -0.5..+0.5
        float lifespan = 0.5f + p.param_c * 3.5f;         // 0.5..4 seconds
        float scale    = p.scale;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        pts.reserve(count);

        for (int i = 0; i < count; ++i)
        {
            // Each particle has a birth time offset: evenly distributed
            float birth_offset = static_cast<float>(i) / count * lifespan;
            float age = std::fmod(static_cast<float>(t) * p.speed + birth_offset,
                                  lifespan);
            float life_frac = age / lifespan; // 0=just born, 1=dying

            // Initial position (deterministic by seed)
            float px0 = det_rand(i * 7 + 1) * scale;
            float py0 = det_rand(i * 7 + 2) * scale * 0.2f; // spawn near bottom-ish

            // Initial velocity
            float vx = det_rand(i * 7 + 3) * 0.4f;
            float vy = det_rand(i * 7 + 4) * 0.8f + 0.3f; // mostly upward

            // Position at this age
            float px = px0 + vx * age * scale;
            float py = py0 + (vy * age + 0.5f * gravity * age * age) * scale;

            // Clamp to visible area
            px = std::clamp(px, -1.f, 1.f);
            py = std::clamp(py, -1.f, 1.f);

            // Fade out near end of life
            float brightness = (life_frac < 0.8f) ?
                1.f : (1.f - (life_frac - 0.8f) / 0.2f);

            Color4 col = ca.lerp(cb, life_frac);
            col.r *= brightness;
            col.g *= brightness;
            col.b *= brightness;

            // All particles are individual points; blanked travel between each
            pts.push_back(LaserPoint::from_norm(px, py,
                col.r8(), col.g8(), col.b8(), true));
            pts.push_back(LaserPoint::from_norm(px, py,
                col.r8(), col.g8(), col.b8(), false));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_particles()
{
    return std::make_unique<GenParticles>();
}

} // namespace idhmfis
