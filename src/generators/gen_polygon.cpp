// gen_polygon.cpp — Regular N-sided polygon outline with optional fill mode.
//
// param_a : radius   (0..1 → 0.1..1.0)
// param_b : sides    (0..1 → 3..32)
// param_c : rotation fraction (0..1 → 0..360 deg static offset, animated by speed)
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
static std::vector<float> poly_scanline_intersect(
    const std::vector<float>& vx, const std::vector<float>& vy, float y)
{
    std::vector<float> xs;
    int num_verts = static_cast<int>(vx.size()) - 1; // last vertex == first (closed)
    for (int i = 0; i < num_verts; ++i) {
        int j = i + 1;
        float y0 = vy[i], y1 = vy[j];
        if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
            float t = (y - y0) / (y1 - y0);
            xs.push_back(vx[i] + t * (vx[j] - vx[i]));
        }
    }
    std::sort(xs.begin(), xs.end());
    return xs;
}

class GenPolygon final : public IGenerator {
public:
    const char* name()          const override { return "polygon"; }
    const char* display_name()  const override { return "Polygon"; }
    const char* param_a_label() const override { return "Radius"; }
    const char* param_b_label() const override { return "Sides"; }
    const char* param_c_label() const override { return "Rotation Offset"; }

    std::vector<GeneratorParamDef> param_defs() const override {
        return {
            { "param_a",   "Radius",               0.f, 1.f, 0.5f, "%.2f" },
            { "param_b",   "Sides",                 0.f, 1.f, 0.3f, "%.2f" },
            { "param_c",   "Rotation Offset",       0.f, 1.f, 0.f,  "%.2f" },
            { "fill_mode", "Fill Mode (NDI/HDMI)",  0.f, 1.f, 0.f,  "bool" },
        };
    }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count + 1);

        float radius = (0.1f + p.param_a * 0.9f) * p.scale;
        int   sides  = std::max(3, static_cast<int>(p.param_b * 29.f + 3.5f));
        sides = std::min(sides, 32);

        float base_angle = p.rotation
                         + p.param_c * 2.f * 3.14159265f
                         + static_cast<float>(t) * p.speed * 2.f * 3.14159265f;
        float cx = p.pan;
        float cy = p.tilt;

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        static constexpr float kTwoPi = 2.f * 3.14159265f;
        float angle_step = kTwoPi / static_cast<float>(sides);

        // Vertex positions (sides+1 so last == first for closed polygon)
        std::vector<float> vx(sides + 1), vy(sides + 1);
        for (int i = 0; i <= sides; ++i) {
            float a = base_angle - kTwoPi * 0.25f + static_cast<float>(i % sides) * angle_step;
            vx[i] = cx + radius * std::cos(a);
            vy[i] = cy + radius * std::sin(a);
        }

        // Check fill mode
        auto it = p.extra_params.find("fill_mode");
        bool fill = (it != p.extra_params.end() && it->second > 0.5f);

        if (fill) {
            // Scanline fill using polygon intersection
            int max_pts = std::min(p.point_count, 4000);
            float ymin = vy[0], ymax = vy[0];
            for (int i = 1; i < sides; ++i) {
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
                auto xs = poly_scanline_intersect(vx, vy, ly);
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
        int n = std::max(sides * 2, p.point_count);
        int pts_per_side = std::max(2, n / sides);

        for (int s = 0; s < sides; ++s) {
            for (int i = 0; i < pts_per_side; ++i) {
                float frac = static_cast<float>(i) / pts_per_side;
                float x = vx[s] + (vx[s + 1] - vx[s]) * frac;
                float y = vy[s] + (vy[s + 1] - vy[s]) * frac;
                bool blank = (s == 0 && i == 0);
                pts.push_back(LaserPoint::from_norm(x, y, r, g, b, blank));
            }
        }

        // Close back to first vertex
        pts.push_back(LaserPoint::from_norm(vx[0], vy[0], r, g, b, false));

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_polygon()
{
    return std::make_unique<GenPolygon>();
}

} // namespace idhmfis
