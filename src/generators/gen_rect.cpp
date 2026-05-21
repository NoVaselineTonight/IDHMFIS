// gen_rect.cpp — Rectangle outline with optional rounded corners.
//
// param_a : width         (0..1 → 0.1..2.0, default maps 0.75 → 1.6)
// param_b : height        (0..1 → 0.1..2.0, default maps 0.55 → 1.2)
// param_c : corner_radius (0..1 → 0..0.5)
// rotation: rotation in radians (animated by speed)
// speed   : angular_speed Hz
// pan/tilt: centre x/y
// color_a : beam colour
// scale   : overall scale
// fill_mode (extra_param): 0 = outline, 1 = solid fill (NDI/HDMI)

#include "igenerator.h"
#include <cmath>
#include <algorithm>
#include <numbers>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

static inline void rotate2d(float& x, float& y, float angle)
{
    float c = std::cos(angle);
    float s = std::sin(angle);
    float nx = x * c - y * s;
    float ny = x * s + y * c;
    x = nx;
    y = ny;
}

class GenRect final : public IGenerator {
public:
    const char* name()          const override { return "rect"; }
    const char* display_name()  const override { return "Rectangle"; }
    const char* param_a_label() const override { return "Width"; }
    const char* param_b_label() const override { return "Height"; }
    const char* param_c_label() const override { return "Corner Radius"; }

    std::vector<GeneratorParamDef> param_defs() const override {
        return {
            { "param_a",   "Width",              0.f, 1.f, 0.75f, "%.2f" },
            { "param_b",   "Height",             0.f, 1.f, 0.55f, "%.2f" },
            { "param_c",   "Corner Radius",      0.f, 1.f, 0.f,   "%.2f" },
            { "fill_mode", "Fill Mode (NDI/HDMI)", 0.f, 1.f, 0.f, "bool" },
        };
    }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count + 4);

        // Dimensions: param 0..1 → 0.1..2.0
        float half_w = (0.1f + p.param_a * 1.9f) * 0.5f * p.scale;
        float half_h = (0.1f + p.param_b * 1.9f) * 0.5f * p.scale;
        float corner = p.param_c * 0.5f * std::min(p.scale, 1.f);
        corner = std::min(corner, std::min(half_w, half_h));

        float angle = p.rotation + static_cast<float>(t) * p.speed * 2.f * 3.14159265f;
        float cx = p.pan;
        float cy = p.tilt;

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        // Check fill mode
        auto it = p.extra_params.find("fill_mode");
        bool fill = (it != p.extra_params.end() && it->second > 0.5f);

        if (fill) {
            // Scanline fill: horizontal lines from top to bottom
            // Use ~3 ILDA units spacing (normalized units: half_h*2 range)
            int max_pts = std::min(p.point_count, 4000);
            // Estimate scanline spacing to stay within max_pts
            // Each scanline is ~(2*half_w / spacing_per_line) points wide
            // Number of scanlines = (2*half_h) / line_gap
            // Total pts ~ scanlines * pts_per_line
            // Choose line_gap so total ~ max_pts
            float line_gap = 0.04f * p.scale;
            if (line_gap < 0.001f) line_gap = 0.001f;
            int num_lines = std::max(1, static_cast<int>((2.f * half_h) / line_gap));
            int pts_per_line = std::max(2, max_pts / std::max(num_lines, 1));
            // Recalculate line_gap to evenly space lines
            line_gap = (2.f * half_h) / static_cast<float>(num_lines);

            bool first_pt = true;
            for (int li = 0; li <= num_lines; ++li) {
                float ly = -half_h + static_cast<float>(li) * line_gap;
                // Scan left to right, then right to left (boustrophedon)
                bool left_to_right = (li % 2 == 0);
                float x0 = left_to_right ? -half_w : half_w;
                float x1 = left_to_right ?  half_w : -half_w;
                for (int pi = 0; pi < pts_per_line; ++pi) {
                    float frac = static_cast<float>(pi) / (pts_per_line - 1);
                    float lx = x0 + (x1 - x0) * frac;
                    float px = lx, py = ly;
                    rotate2d(px, py, angle);
                    px += cx; py += cy;
                    // Blank: travel to very first point or at start of each new line
                    bool blank = first_pt || (pi == 0);
                    pts.push_back(LaserPoint::from_norm(px, py, r, g, b, blank));
                    first_pt = false;
                }
            }
            return pts;
        }

        // Total target points distributed across perimeter
        int n = std::max(8, p.point_count);

        // If no corner radius, build a simple 4-side rectangle
        if (corner < 0.001f) {
            // 4 corners in order: TL → TR → BR → BL → TL
            float corners_x[5] = { -half_w,  half_w,  half_w, -half_w, -half_w };
            float corners_y[5] = {  half_h,  half_h, -half_h, -half_h,  half_h };

            // Distribute points proportionally to side lengths
            // All 4 sides are equal length pair (two widths, two heights)
            float perimeter = 4.f * (half_w + half_h) * 2.f;
            (void)perimeter; // unused, using n directly

            int pts_per_side_w = std::max(2, static_cast<int>(n * (half_w / (2.f * (half_w + half_h))) + 0.5f));
            int pts_per_side_h = std::max(2, static_cast<int>(n * (half_h / (2.f * (half_w + half_h))) + 0.5f));

            auto emit_side = [&](float x0, float y0, float x1, float y1, int npts, bool first_blanked) {
                for (int i = 0; i < npts; ++i) {
                    float frac = static_cast<float>(i) / (npts - 1);
                    float px = x0 + (x1 - x0) * frac;
                    float py = y0 + (y1 - y0) * frac;
                    rotate2d(px, py, angle);
                    px += cx; py += cy;
                    bool blank = (first_blanked && i == 0);
                    pts.push_back(LaserPoint::from_norm(px, py, r, g, b, blank));
                }
            };

            emit_side(corners_x[0], corners_y[0], corners_x[1], corners_y[1], pts_per_side_w, true);
            emit_side(corners_x[1], corners_y[1], corners_x[2], corners_y[2], pts_per_side_h, false);
            emit_side(corners_x[2], corners_y[2], corners_x[3], corners_y[3], pts_per_side_w, false);
            emit_side(corners_x[3], corners_y[3], corners_x[4], corners_y[4], pts_per_side_h, false);
        } else {
            // Rounded rectangle: 4 straight sides + 4 quarter-circle arcs
            // Corner arc centres
            float ax[4] = {  half_w - corner, -(half_w - corner), -(half_w - corner),  half_w - corner };
            float ay[4] = {  half_h - corner,   half_h - corner,  -(half_h - corner), -(half_h - corner) };
            // Arc start angles (degrees → rad): TR, TL, BL, BR
            static constexpr float kPi     = 3.14159265f;
            static constexpr float kHalfPi = kPi * 0.5f;
            float arc_start[4] = { 0.f, kHalfPi, kPi, kPi * 1.5f };

            // Perimeter segments: straight side then arc at each corner
            // Points per arc corner
            int pts_arc = std::max(4, n / 16);
            // Points per straight side
            float perimeter_straight = 8.f * (half_w + half_h - 2.f * corner);
            float perimeter_arcs     = 2.f * kPi * corner;
            float total_p = perimeter_straight + perimeter_arcs;
            (void)total_p;

            // Emit in order: starting at top-right corner arc, going CCW
            bool first_point = true;

            // Order: top side, TL arc, left side, BL arc, bottom side, BR arc, right side, TR arc
            struct Side { float x0, y0, x1, y1; int arc_idx; };
            Side sides[4] = {
                // top (right to left)
                {  half_w - corner,  half_h,   -(half_w - corner),  half_h,  1 },
                // left (top to bottom)
                { -(half_w),         half_h - corner, -(half_w), -(half_h - corner), 2 },
                // bottom (left to right)
                { -(half_w - corner), -half_h,   half_w - corner,  -half_h, 3 },
                // right (bottom to top)
                {  half_w,          -(half_h - corner),  half_w,   half_h - corner, 0 },
            };

            int pts_straight_w = std::max(2, static_cast<int>(n * (half_w - corner) / (2.f * (half_w + half_h)) + 0.5f));
            int pts_straight_h = std::max(2, static_cast<int>(n * (half_h - corner) / (2.f * (half_w + half_h)) + 0.5f));
            int pts_per_side[4] = { pts_straight_w, pts_straight_h, pts_straight_w, pts_straight_h };

            for (int s = 0; s < 4; ++s) {
                const Side& sd = sides[s];
                int nseg = pts_per_side[s];

                // Emit straight segment (skip first point on non-first segments to avoid dup)
                int istart = first_point ? 0 : 1;
                for (int i = istart; i < nseg; ++i) {
                    float frac = static_cast<float>(i) / (nseg - 1);
                    float px = sd.x0 + (sd.x1 - sd.x0) * frac;
                    float py = sd.y0 + (sd.y1 - sd.y0) * frac;
                    rotate2d(px, py, angle);
                    px += cx; py += cy;
                    pts.push_back(LaserPoint::from_norm(px, py, r, g, b, first_point && i == 0));
                    first_point = false;
                }

                // Emit corner arc
                int ai = sd.arc_idx;
                float a_start = arc_start[ai];
                for (int i = 1; i <= pts_arc; ++i) {
                    float frac = static_cast<float>(i) / pts_arc;
                    float a_ang = a_start + frac * kHalfPi;
                    float px = ax[ai] + corner * std::cos(a_ang);
                    float py = ay[ai] + corner * std::sin(a_ang);
                    rotate2d(px, py, angle);
                    px += cx; py += cy;
                    pts.push_back(LaserPoint::from_norm(px, py, r, g, b, false));
                }
            }
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_rect()
{
    return std::make_unique<GenRect>();
}

} // namespace idhmfis
