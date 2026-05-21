// gen_triangle.cpp — Triangle outline (equilateral or scalene).
//
// param_a : size / scale factor (0..1 → 0.1..1.0)
// param_b : rotation_deg fraction (0..1 → 0..360, animated by speed)
// param_c : vertex squish — equilateral when 0.5, scalene when != 0.5
//           (0..1 → vertex 0 y offset -0.5..+0.5 relative to baseline)
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

// Return x intersections of scanline y with the polygon edges (num_verts, closed)
static std::vector<float> scanline_intersect(
    const float* vx, const float* vy, int num_verts, float y)
{
    std::vector<float> xs;
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

class GenTriangle final : public IGenerator {
public:
    const char* name()          const override { return "triangle"; }
    const char* display_name()  const override { return "Triangle"; }
    const char* param_a_label() const override { return "Size"; }
    const char* param_b_label() const override { return "Rotation"; }
    const char* param_c_label() const override { return "Vertex Offset"; }

    std::vector<GeneratorParamDef> param_defs() const override {
        return {
            { "param_a",   "Size",                  0.f, 1.f, 0.5f, "%.2f" },
            { "param_b",   "Rotation",               0.f, 1.f, 0.f,  "%.2f" },
            { "param_c",   "Vertex Offset",          0.f, 1.f, 0.5f, "%.2f" },
            { "fill_mode", "Fill Mode (NDI/HDMI)",   0.f, 1.f, 0.f,  "bool" },
        };
    }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count + 1);

        float size  = (0.1f + p.param_a * 0.9f) * p.scale;
        float angle = p.rotation
                    + p.param_b * 2.f * 3.14159265f
                    + static_cast<float>(t) * p.speed * 2.f * 3.14159265f;
        float cx = p.pan;
        float cy = p.tilt;

        // param_c 0.5 = equilateral (no offset), other values shift top vertex
        float v_offset = (p.param_c - 0.5f) * 2.f * size * 0.5f;

        // 3 vertices of equilateral triangle, top at 90 deg
        static constexpr float kTwoPi = 2.f * 3.14159265f;
        float vx[3], vy[3];
        for (int i = 0; i < 3; ++i) {
            float a = static_cast<float>(i) / 3.f * kTwoPi - 3.14159265f * 0.5f;
            vx[i] = size * std::cos(a);
            vy[i] = size * std::sin(a);
        }
        // Apply param_c offset to top vertex (index 0)
        vy[0] += v_offset;

        // Rotate vertices
        for (int i = 0; i < 3; ++i) {
            float c = std::cos(angle);
            float s = std::sin(angle);
            float nx = vx[i] * c - vy[i] * s;
            float ny = vx[i] * s + vy[i] * c;
            vx[i] = nx + cx;
            vy[i] = ny + cy;
        }

        // Check fill mode
        auto it = p.extra_params.find("fill_mode");
        bool fill = (it != p.extra_params.end() && it->second > 0.5f);

        if (fill) {
            // Scanline fill
            int max_pts = std::min(p.point_count, 4000);
            float ymin = vy[0], ymax = vy[0];
            for (int i = 1; i < 3; ++i) {
                if (vy[i] < ymin) ymin = vy[i];
                if (vy[i] > ymax) ymax = vy[i];
            }
            float line_gap = 0.04f * p.scale;
            if (line_gap < 0.001f) line_gap = 0.001f;
            int num_lines = std::max(1, static_cast<int>((ymax - ymin) / line_gap));
            int pts_per_line = std::max(2, max_pts / std::max(num_lines, 1));
            line_gap = (ymax - ymin) / static_cast<float>(num_lines);

            uint8_t fr = p.color_a.r8(), fg = p.color_a.g8(), fb = p.color_a.b8();
            bool first_pt = true;
            for (int li = 0; li <= num_lines; ++li) {
                float ly = ymin + static_cast<float>(li) * line_gap;
                auto xs = scanline_intersect(vx, vy, 3, ly);
                if (xs.size() < 2u) continue;
                bool left_to_right = (li % 2 == 0);
                float x0 = left_to_right ? xs.front() : xs.back();
                float x1 = left_to_right ? xs.back()  : xs.front();
                for (int pi = 0; pi < pts_per_line; ++pi) {
                    float frac = static_cast<float>(pi) / (pts_per_line - 1);
                    float px = x0 + (x1 - x0) * frac;
                    bool blank = first_pt || (pi == 0);
                    pts.push_back(LaserPoint::from_norm(px, ly, fr, fg, fb, blank));
                    first_pt = false;
                }
            }
            return pts;
        }

        // Distribute points across 3 sides
        int n = std::max(6, p.point_count);
        // Equal distribution across 3 sides
        int pts_per_side = n / 3;
        if (pts_per_side < 2) pts_per_side = 2;

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        for (int side = 0; side < 3; ++side) {
            int a_idx = side;
            int b_idx = (side + 1) % 3;
            for (int i = 0; i < pts_per_side; ++i) {
                float frac = static_cast<float>(i) / pts_per_side;
                float x = vx[a_idx] + (vx[b_idx] - vx[a_idx]) * frac;
                float y = vy[a_idx] + (vy[b_idx] - vy[a_idx]) * frac;
                bool blank = (side == 0 && i == 0);
                pts.push_back(LaserPoint::from_norm(x, y, r, g, b, blank));
            }
        }

        // Close back to first vertex
        pts.push_back(LaserPoint::from_norm(vx[0], vy[0], r, g, b, false));

        return pts;
    }

};

std::unique_ptr<IGenerator> make_gen_triangle()
{
    return std::make_unique<GenTriangle>();
}

} // namespace idhmfis
