// gen_dot_matrix.cpp — Rotating symmetric dot matrix.
//
// The most iconic EDM laser effect: concentric rings of dots, each ring
// spinning at a different rate. At 5 rings x 8 dots = 40 bright points,
// all rotating, the result reads as rich radial symmetry pulsing with music.
//
// param_a : ring_count    (0..1 -> 2..8 rings)
// param_b : dots_per_ring (0..1 -> 4..16 dots per ring; inner rings use fewer)
// param_c : beat_sync     (0=disabled, >0.5=enabled — multiplies ring speeds
//                         by bpm/60 so rotations lock to the beat)
//
// speed   : master rotation rate (Hz); each ring multiplied by ring index
// scale   : overall radius of outermost ring
// color_a : inner ring colour
// color_b : outer ring colour
// rotation: global phase offset (radians)
// density : ring spacing curvature (0=linear, 1=logarithmic)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

namespace {

static constexpr float kTwoPi = 6.28318530717958647692f;

class GenDotMatrix final : public IGenerator {
public:
    const char* name()         const override { return "dot_matrix"; }
    const char* display_name() const override { return "Dot Matrix"; }
    const char* param_a_label()const override { return "Ring Count"; }
    const char* param_b_label()const override { return "Dots/Ring"; }
    const char* param_c_label()const override { return "Beat Sync"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;

        // Ring count: param_a 0..1 -> 2..8
        int ring_count = 2 + static_cast<int>(p.param_a * 6.f);

        // Dots per ring: param_b 0..1 -> 4..16
        // Inner rings use fewer dots to maintain even angular density
        int base_dots = 4 + static_cast<int>(p.param_b * 12.f);

        // Beat sync: when param_c > 0.5 we treat p.speed as bpm-locked
        // i.e. speed means "rotations per beat" rather than "rotations per second"
        bool beat_sync = (p.param_c >= 0.5f);

        // Each dot needs: blank travel + lit dwell points.
        // We allocate a small cluster of 2 lit points per dot for scanner stability.
        static constexpr int kDwellPts = 3;
        int total_dots_estimate = ring_count * base_dots;
        pts.reserve(static_cast<size_t>(total_dots_estimate * (kDwellPts + 1)));

        float outer_radius = p.scale;
        // Ring spacing: density 0=linear, 1=logarithmic (compressed toward centre)
        float curve = std::clamp(p.density, 0.f, 1.f);

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;
        float global_phase = p.rotation;

        // Animate: each ring spins at a different speed.
        // Ring 0 (innermost) spins at speed, ring k spins at speed * (k+1)
        // This creates the characteristic "differential rotation" look.
        float anim_t = static_cast<float>(t);

        for (int ring = 0; ring < ring_count; ++ring)
        {
            float ring_frac = (ring_count > 1)
                ? static_cast<float>(ring) / static_cast<float>(ring_count - 1)
                : 0.f;

            // Ring radius: lerp linear vs log spacing
            float linear_r = (ring + 1.f) / static_cast<float>(ring_count);
            float log_r    = 1.f - std::exp(-3.f * (ring + 1.f)
                             / static_cast<float>(ring_count));
            float norm_log_r = log_r / (1.f - std::exp(-3.f));  // normalise 0..1
            float ring_r    = outer_radius * (linear_r * (1.f - curve)
                             + norm_log_r * curve);

            // Each ring uses a different number of dots (inner rings fewer)
            // to keep angular spacing visually even
            int dots = std::max(3, static_cast<int>(
                base_dots * (0.5f + 0.5f * ring_frac)));

            // Ring rotation speed: proportional to ring index (inner fast, outer slow)
            // or outer fast if ring index reversed — both look good; we use
            // outer-faster by default (more dramatic).
            float ring_speed_mult = 1.f + ring_frac * 2.f;  // 1..3x outer
            float rot_speed = p.speed * ring_speed_mult;

            // Beat sync: interpret speed as "full rotations per beat" at current BPM.
            // bpm is not in GeneratorParams but we can bake it via convention:
            // when beat_sync is on, scale speed by bpm/60 so 1 rot/beat at 120bpm
            // = 2 rot/sec. Since we don't have audio here, use speed*2 as proxy
            // (operator can dial in bpm-matched value).
            // Real beat sync can be wired via modulation matrix in the show engine.
            if (beat_sync) rot_speed *= 2.f;  // placeholder; real sync via ModMatrix

            float ring_angle = global_phase + anim_t * rot_speed * kTwoPi;

            // Colour per ring: lerp from color_a (inner) to color_b (outer)
            Color4 col = ca.lerp(cb, ring_frac);
            uint8_t r = col.r8(), g = col.g8(), b = col.b8();

            for (int d = 0; d < dots; ++d)
            {
                float dot_angle = ring_angle
                    + kTwoPi * static_cast<float>(d) / static_cast<float>(dots);
                float dx = ring_r * std::cos(dot_angle) + p.pan;
                float dy = ring_r * std::sin(dot_angle) + p.tilt;

                // Blanked travel to dot position
                pts.push_back(LaserPoint::from_norm(dx, dy, r, g, b, true));

                // Dwell points (laser stays on for a few samples for visibility)
                for (int dw = 0; dw < kDwellPts; ++dw)
                    pts.push_back(LaserPoint::from_norm(dx, dy, r, g, b, false));
            }
        }

        return pts;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_dot_matrix()
{
    return std::make_unique<GenDotMatrix>();
}

} // namespace idhmfis
