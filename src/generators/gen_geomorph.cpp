// gen_geomorph.cpp — Geometric morph between two regular polygons.
//
// Interpolates vertex-by-vertex between an n-gon and an m-gon.
// Uses the "max sides" approach: both shapes are traced at max(n,m) vertices
// by cycling (wrapping) the smaller polygon.
//
// param_a: source sides  (0..1 → 3..16)
// param_b: target sides  (0..1 → 3..16)
// param_c: morph amount  (0..1)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenGeomorph final : public IGenerator {
public:
    const char* name()         const override { return "geomorph"; }
    const char* display_name() const override { return "Geomorph"; }
    const char* param_a_label()const override { return "Source Sides"; }
    const char* param_b_label()const override { return "Target Sides"; }
    const char* param_c_label()const override { return "Morph"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        int n_src = 3 + static_cast<int>(p.param_a * 13.f); // 3..16
        int n_dst = 3 + static_cast<int>(p.param_b * 13.f); // 3..16
        float morph = std::clamp(p.param_c, 0.f, 1.f);
        float scale = p.scale;
        float anim  = static_cast<float>(t) * p.speed;

        static constexpr float kTwoPi = 2.f * 3.14159265f;

        // Use the larger side count for the trace resolution
        int n_pts = std::max(p.point_count,
            std::max(n_src, n_dst) * 4); // at least 4 pts per side
        n_pts = std::max(n_pts, p.point_count);

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        // Travel to start
        pts.push_back(LaserPoint::from_norm(scale, 0.f,
            ca.r8(), ca.g8(), ca.b8(), true));

        for (int i = 0; i <= n_pts; ++i)
        {
            float frac  = static_cast<float>(i % n_pts) / n_pts;
            float theta = frac * kTwoPi + anim * 0.3f;

            // Source polygon: compute point on polygon edge at angle theta
            float seg_src = kTwoPi / n_src;

            // For a regular polygon we want the actual polygon surface, not just vertices.
            // Compute the point on the polygon edge at angle theta.
            float half_seg_src  = seg_src / 2.f;
            float base_angle_s  = std::floor(theta / seg_src) * seg_src;
            float local_s       = theta - base_angle_s;
            float perp_cos_s    = std::cos(half_seg_src);
            // polygon radius at this angle: r/cos(local - half_seg)
            float r_src         = perp_cos_s / std::cos(local_s - half_seg_src);

            float seg_dst = kTwoPi / n_dst;
            float half_seg_dst  = seg_dst / 2.f;
            float base_angle_d  = std::floor(theta / seg_dst) * seg_dst;
            float local_d       = theta - base_angle_d;
            float perp_cos_d    = std::cos(half_seg_dst);
            float r_dst         = perp_cos_d / std::cos(local_d - half_seg_dst);

            float r = scale * ((1.f - morph) * r_src + morph * r_dst);
            r = std::clamp(r, 0.f, 2.f);

            float x = r * std::cos(theta);
            float y = r * std::sin(theta);

            Color4 col = ca.lerp(cb, morph);

            pts.push_back(LaserPoint::from_norm(x, y,
                col.r8(), col.g8(), col.b8(), false));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_geomorph()
{
    return std::make_unique<GenGeomorph>();
}

} // namespace idhmfis
