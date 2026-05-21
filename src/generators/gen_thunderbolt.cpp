// gen_thunderbolt.cpp — Lightning bolt / thunder strike generator.
//
// Renders one or more jagged polylines (lightning channels) from the
// centre of the field toward a configurable target point.  A fast pseudo-
// random seed is advanced every 1/speed seconds so the bolt re-jags on
// each cycle — giving the characteristic "flickering discharge" look.
//
// Sub-branches (controlled by density) fork off from random mid-points,
// each at ~60–120° to the main channel with 50% shorter reach.
//
// param_a : target_x      (0..1  →  -1..1, default 0.5 → 0.0)
// param_b : target_y      (0..1  →  -1..1, default 0.0 → -1.0)
// param_c : jaggedness    (0=arrow-straight, 1=maximum chaotic fractal)
//
// speed   : bolt re-randomise rate (Hz; 0.5–8 useful range)
// scale   : amplitude of lateral zigzag (multiplied by jaggedness)
// density : branch probability 0..1 (0=no branches, 1=heavy branching)
// rotation: rotate the whole bolt around the origin (radians)

#include "igenerator.h"
#include <cmath>
#include <algorithm>
#include <utility>

namespace idhmfis {
namespace {

static constexpr float kTwoPi = 6.28318530717958647692f;

using Pt2 = std::pair<float,float>;

static float pr(unsigned& seed) {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 16) / 65535.f;
}

static void subdivide(
    std::vector<Pt2>& pts,
    float x0, float y0,
    float x1, float y1,
    int depth, float jag,
    unsigned& seed)
{
    if (depth == 0) {
        pts.push_back({x1, y1});
        return;
    }
    float mx = (x0 + x1) * 0.5f;
    float my = (y0 + y1) * 0.5f;
    float dx = x1 - x0, dy = y1 - y0;
    float len = std::sqrt(dx*dx + dy*dy);
    if (len < 1e-6f) { pts.push_back({x1, y1}); return; }
    float nx = -dy / len, ny = dx / len;
    float disp = (pr(seed) - 0.5f) * jag * len * 0.7f;
    mx += nx * disp;
    my += ny * disp;
    subdivide(pts, x0, y0, mx, my, depth-1, jag*0.6f, seed);
    subdivide(pts, mx, my, x1, y1, depth-1, jag*0.6f, seed);
}

class GenThunderbolt final : public IGenerator {
public:
    const char* name()          const override { return "thunderbolt"; }
    const char* display_name()  const override { return "Thunderbolt"; }
    const char* param_a_label() const override { return "Target X"; }
    const char* param_b_label() const override { return "Target Y"; }
    const char* param_c_label() const override { return "Jaggedness"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer out;

        float tx = p.pan  + (p.param_a * 2.f - 1.f) * p.scale;
        float ty = p.tilt + (p.param_b * 2.f - 1.f) * p.scale;
        float jag = std::max(0.001f, p.param_c);

        double period = (p.speed > 0.f) ? 1.0 / static_cast<double>(p.speed) : 0.5;
        unsigned seed = static_cast<unsigned>(std::floor(t / period)) * 2654435761u;
        seed ^= static_cast<unsigned>(tx * 1234.5f) ^ static_cast<unsigned>(ty * 5678.9f);

        float ox = p.pan, oy = p.tilt;

        // Rotate target around origin
        float cos_r = std::cos(p.rotation), sin_r = std::sin(p.rotation);
        float dx = tx - ox, dy = ty - oy;
        float rtx = ox + dx*cos_r - dy*sin_r;
        float rty = oy + dx*sin_r + dy*cos_r;

        // Main bolt
        std::vector<Pt2> bolt;
        bolt.push_back({ox, oy});
        subdivide(bolt, ox, oy, rtx, rty, 5, jag * p.scale, seed);

        // Emit main bolt
        for (size_t i = 0; i < bolt.size(); ++i) {
            float u = static_cast<float>(i) / static_cast<float>(bolt.size() - 1);
            Color4 c = p.color_a.lerp(p.color_b, u);
            bool blank = (i == 0);
            out.push_back(LaserPoint::from_norm(bolt[i].first, bolt[i].second,
                c.r8(), c.g8(), c.b8(), blank));
        }

        // Sub-branches
        float main_len = std::sqrt((rtx-ox)*(rtx-ox) + (rty-oy)*(rty-oy));
        float main_angle = std::atan2(rty - oy, rtx - ox);

        for (size_t i = 2; i + 2 < bolt.size(); ++i) {
            if (pr(seed) > p.density * 0.8f) continue;

            float bx0 = bolt[i].first, by0 = bolt[i].second;
            float bangle = main_angle + (pr(seed) - 0.5f) * kTwoPi * 0.5f;
            float blen   = main_len * (0.2f + pr(seed) * 0.3f);
            float bx1 = bx0 + std::cos(bangle) * blen;
            float by1 = by0 + std::sin(bangle) * blen;

            std::vector<Pt2> branch;
            branch.push_back({bx0, by0});
            subdivide(branch, bx0, by0, bx1, by1, 3, jag * 0.5f, seed);

            for (size_t j = 0; j < branch.size(); ++j) {
                float u = static_cast<float>(j) / static_cast<float>(branch.size() - 1);
                Color4 c = p.color_b.lerp(p.color_a, u * 0.5f);
                bool blank = (j == 0);
                out.push_back(LaserPoint::from_norm(branch[j].first, branch[j].second,
                    c.r8(), c.g8(), c.b8(), blank));
            }
        }

        return out;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_thunderbolt()
{
    return std::make_unique<GenThunderbolt>();
}

} // namespace idhmfis
