// gen_star.cpp — Star polygon outline with optional fill mode.
//
// Alternates outer/inner radius vertices to form a classic N-pointed star.
//
// param_a : outer_radius   (0..1 → 0.1..1.0)
// param_b : inner_radius   (0..1 → 0.05..0.9)
// param_c : number of points (0..1 → 2..32)
// rotation: base rotation (animated by speed)
// speed   : angular_speed Hz
// pan/tilt: centre x/y
// color_a : beam colour
// scale   : overall scale
// fill_mode (extra_param): 0 = outline, 1 = solid fill (NDI/HDMI)

#include "igenerator.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace idhmfis {

// Return x intersections of scanline y with the polygon edges
static std::vector<float> star_scanline_intersect(
    const std::vector<float>& vx, const std::vector<float>& vy, float y)
{
    std::vector<float> xs;
    int num_verts = static_cast<int>(vx.size());
    for (int i = 0; i < num_verts; ++i) {
        int j = (i + 1) % num_verts;
        float y0 = vy[i], y1 = vy[j];
        if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
            float t = (y - y0) / (y1 - y0);
            xs.push_back(vx[i] + t * (vx[j] - vx[i]));
        }
    }
    std::sort(xs.begin(), xs.end());
    return xs;
}

class GenStar final : public IGenerator {
public:
    const char* name()          const override { return "star"; }
    const char* display_name()  const override { return "Star"; }
    const char* param_a_label() const override { return "Outer Radius"; }
    const char* param_b_label() const override { return "Inner Radius"; }
    const char* param_c_label() const override { return "Points"; }

    std::vector<GeneratorParamDef> param_defs() const override {
        return {
            { "param_a",   "Outer Radius",         0.f, 1.f, 0.5f, "%.2f" },
            { "param_b",   "Inner Radius",          0.f, 1.f, 0.3f, "%.2f" },
            { "param_c",   "Points",                0.f, 1.f, 0.1f, "%.2f" },
            { "fill_mode", "Fill Mode (NDI/HDMI)",  0.f, 1.f, 0.f,  "bool" },
        };
    }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count + 2);

        float outer = (0.1f + p.param_a * 0.9f) * p.scale;
        float inner = (0.05f + p.param_b * 0.85f) * p.scale;
        int   npts  = std::max(2, static_cast<int>(p.param_c * 30.f + 2.5f));
        npts = std::min(npts, 32);

        float base_angle = p.rotation
                         + static_cast<float>(t) * p.speed * 2.f * 3.14159265f;
        float cx = p.pan;
        float cy = p.tilt;

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        // Star has 2*npts vertices alternating outer/inner
        int total_verts = 2 * npts;

        static constexpr float kTwoPi = 2.f * 3.14159265f;
        float angle_step = kTwoPi / static_cast<float>(total_verts);

        // Build star vertex positions
        std::vector<float> vx(total_verts), vy(total_verts);
        for (int i = 0; i < total_verts; ++i) {
            float a = base_angle - kTwoPi * 0.25f + static_cast<float>(i) * angle_step;
            float rad = (i % 2 == 0) ? outer : inner;
            vx[i] = cx + rad * std::cos(a);
            vy[i] = cy + rad * std::sin(a);
        }

        // Check fill mode
        auto it = p.extra_params.find("fill_mode");
        bool fill = (it != p.extra_params.end() && it->second > 0.5f);

        if (fill) {
            // Scanline fill using polygon intersection
            int max_pts = std::min(p.point_count, 4000);
            float ymin = vy[0], ymax = vy[0];
            for (int i = 1; i < total_verts; ++i) {
                if (vy[i] < ymin) ymin = vy[i];
                if (vy[i] > ymax) ymax = vy[i];
            }
            float line_gap = 0.04f * p.scale;
            if (line_gap < 0.001f) line_gap = 0.001f;
            int num_lines = std::max(1, static_cast<int>((ymax - ymin) / line_gap));
            int pts_per_line = std::max(2, max_pts / std::max(num_lines, 1));
            line_gap = (ymax - ymin) / static_cast<float>(num_lines);

            bool first_pt = true;
            for (int li = 0; li <= num_lines; ++li) {
                float ly = ymin + static_cast<float>(li) * line_gap;
                auto xs = star_scanline_intersect(vx, vy, ly);
                if (xs.size() < 2u) continue;
                bool left_to_right = (li % 2 == 0);
                float x0 = left_to_right ? xs.front() : xs.back();
                float x1 = left_to_right ? xs.back()  : xs.front();
                for (int pi = 0; pi < pts_per_line; ++pi) {
                    float frac = static_cast<float>(pi) / (pts_per_line - 1);
                    float px = x0 + (x1 - x0) * frac;
                    bool blank = first_pt || (pi == 0);
                    pts.push_back(LaserPoint::from_norm(px, ly, r, g, b, blank));
                    first_pt = false;
                }
            }
            return pts;
        }

        // Outline mode
        int n_draw = std::max(p.point_count, total_verts + 1);
        // Points per segment between consecutive star vertices
        int pts_per_seg = std::max(2, n_draw / total_verts);

        // Emit segments
        for (int seg = 0; seg < total_verts; ++seg) {
            int next = (seg + 1) % total_verts;
            for (int i = 0; i < pts_per_seg; ++i) {
                float frac = static_cast<float>(i) / pts_per_seg;
                float x = vx[seg] + (vx[next] - vx[seg]) * frac;
                float y = vy[seg] + (vy[next] - vy[seg]) * frac;
                bool blank = (seg == 0 && i == 0);
                pts.push_back(LaserPoint::from_norm(x, y, r, g, b, blank));
            }
        }

        // Close back to first vertex
        pts.push_back(LaserPoint::from_norm(vx[0], vy[0], r, g, b, false));

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_star()
{
    return std::make_unique<GenStar>();
}

} // namespace idhmfis
