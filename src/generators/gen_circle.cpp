// gen_circle.cpp — Closed circle outline with optional fill mode.
//
// param_a : radius          (0..1 → 0.1..1.0, default 0.8)
// param_b : start_angle_deg (0..1 → 0..360, animated by speed)
// param_c : unused (reserved)
// speed   : angular_speed Hz (animates start_angle)
// color_a : beam colour
// scale   : overall size multiplier
// pan/tilt: centre x/y offset
// fill_mode (extra_param): 0 = outline, 1 = solid fill (NDI/HDMI)

#include "igenerator.h"
#include <cmath>

namespace idhmfis {

class GenCircle final : public IGenerator {
public:
    const char* name()          const override { return "circle"; }
    const char* display_name()  const override { return "Circle"; }
    const char* param_a_label() const override { return "Radius"; }
    const char* param_b_label() const override { return "Start Angle"; }
    const char* param_c_label() const override { return "Unused"; }

    std::vector<GeneratorParamDef> param_defs() const override {
        return {
            { "param_a",   "Radius",               0.f, 1.f, 0.8f, "%.2f" },
            { "param_b",   "Start Angle",           0.f, 1.f, 0.f,  "%.2f" },
            { "param_c",   "Unused",                0.f, 1.f, 0.f,  "%.2f" },
            { "fill_mode", "Fill Mode (NDI/HDMI)",  0.f, 1.f, 0.f,  "bool" },
        };
    }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        int n = std::max(2, p.point_count);

        // Radius: param_a 0..1 → 0.1..1.0
        float radius = 0.1f + p.param_a * 0.9f;
        radius *= p.scale;

        // Centre from pan/tilt
        float cx = p.pan;
        float cy = p.tilt;

        // Start angle: param_b encodes 0..360 deg offset, speed animates it
        float start_angle = p.param_b * 2.f * 3.14159265f
                          + static_cast<float>(t) * p.speed * 2.f * 3.14159265f;

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        // Check fill mode
        auto it = p.extra_params.find("fill_mode");
        bool fill = (it != p.extra_params.end() && it->second > 0.5f);

        if (fill) {
            // Horizontal chord scanline fill
            int max_pts = std::min(p.point_count, 4000);
            float line_gap = 0.04f * p.scale;
            if (line_gap < 0.001f) line_gap = 0.001f;
            int num_lines = std::max(1, static_cast<int>((2.f * radius) / line_gap));
            int pts_per_line = std::max(2, max_pts / std::max(num_lines, 1));
            line_gap = (2.f * radius) / static_cast<float>(num_lines);

            pts.reserve(num_lines * pts_per_line + 2);
            bool first_pt = true;
            for (int li = 0; li <= num_lines; ++li) {
                float ly = -radius + static_cast<float>(li) * line_gap;
                // Half-chord width at this y
                float disc = radius * radius - ly * ly;
                if (disc <= 0.f) continue;
                float hchord = std::sqrt(disc);
                // Boustrophedon direction
                bool left_to_right = (li % 2 == 0);
                float x0 = left_to_right ? -hchord : hchord;
                float x1 = left_to_right ?  hchord : -hchord;
                for (int pi = 0; pi < pts_per_line; ++pi) {
                    float frac = static_cast<float>(pi) / (pts_per_line - 1);
                    float px = cx + x0 + (x1 - x0) * frac;
                    float py = cy + ly;
                    bool blank = first_pt || (pi == 0);
                    pts.push_back(LaserPoint::from_norm(px, py, r, g, b, blank));
                    first_pt = false;
                }
            }
            return pts;
        }

        pts.reserve(n + 1);

        static constexpr float kTwoPi = 2.f * 3.14159265f;

        for (int i = 0; i <= n; ++i) {
            // i == n closes the circle back to the first point
            float angle = start_angle + static_cast<float>(i % n) / n * kTwoPi;
            float x = cx + radius * std::cos(angle);
            float y = cy + radius * std::sin(angle);
            // Blank travel to very first point; lit from index 1 onward
            bool blank = (i == 0);
            pts.push_back(LaserPoint::from_norm(x, y, r, g, b, blank));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_circle()
{
    return std::make_unique<GenCircle>();
}

} // namespace idhmfis
