// gen_grid.cpp — Regular laser grid.
//
// Draws a grid of horizontal + vertical lines within the normalised [-1,1] square.
// Blanked travel between non-adjacent lines.
//
// param_a: horizontal line density  (0..1 -> 2..20 lines)
// param_b: vertical line density    (0..1 -> 2..20 lines)
// param_c: grid rotation animation  (0..1 -> 0..pi/4)

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

namespace {

class GenGrid final : public IGenerator {
public:
    const char* name()         const override { return "grid"; }
    const char* display_name() const override { return "Grid"; }
    const char* param_a_label()const override { return "H Lines"; }
    const char* param_b_label()const override { return "V Lines"; }
    const char* param_c_label()const override { return "Rotate"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        int h_lines = 2 + static_cast<int>(p.param_a * 18.f); // 2..20
        int v_lines = 2 + static_cast<int>(p.param_b * 18.f); // 2..20

        float grid_rot = p.param_c * 3.14159265f / 4.f
                       + static_cast<float>(t) * p.speed * 0.2f;
        float scale = p.scale;

        float cos_r = std::cos(grid_rot);
        float sin_r = std::sin(grid_rot);

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        int pts_per_line = std::max(2, p.point_count / (h_lines + v_lines));

        auto rot = [&](float x, float y, float& ox, float& oy)
        {
            ox = x * cos_r - y * sin_r;
            oy = x * sin_r + y * cos_r;
        };

        // Horizontal lines
        for (int r = 0; r < h_lines; ++r)
        {
            float y = -scale + scale * 2.f * static_cast<float>(r)
                      / (h_lines > 1 ? h_lines - 1 : 1);
            float frac = static_cast<float>(r) / (h_lines > 1 ? h_lines - 1 : 1);
            Color4 col = ca.lerp(cb, frac);

            float x_start = (r % 2 == 0) ? -scale :  scale;
            float x_end   = (r % 2 == 0) ?  scale : -scale;

            for (int i = 0; i <= pts_per_line; ++i)
            {
                float u  = static_cast<float>(i) / pts_per_line;
                float fx = x_start + (x_end - x_start) * u;
                float rx, ry;
                rot(fx, y, rx, ry);
                pts.push_back(LaserPoint::from_norm(rx, ry,
                    col.r8(), col.g8(), col.b8(), i == 0));
            }
        }

        // Vertical lines
        for (int c = 0; c < v_lines; ++c)
        {
            float x = -scale + scale * 2.f * static_cast<float>(c)
                      / (v_lines > 1 ? v_lines - 1 : 1);
            float frac = static_cast<float>(c) / (v_lines > 1 ? v_lines - 1 : 1);
            Color4 col = ca.lerp(cb, frac);

            float y_start = (c % 2 == 0) ? -scale :  scale;
            float y_end   = (c % 2 == 0) ?  scale : -scale;

            for (int i = 0; i <= pts_per_line; ++i)
            {
                float u  = static_cast<float>(i) / pts_per_line;
                float fy = y_start + (y_end - y_start) * u;
                float rx, ry;
                rot(x, fy, rx, ry);
                pts.push_back(LaserPoint::from_norm(rx, ry,
                    col.r8(), col.g8(), col.b8(), i == 0));
            }
        }

        return pts;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_grid()
{
    return std::make_unique<GenGrid>();
}

} // namespace idhmfis
