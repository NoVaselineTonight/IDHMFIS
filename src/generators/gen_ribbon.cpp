// gen_ribbon.cpp — Flowing Bezier ribbon that undulates over time.
//
// The ribbon is defined by a central cubic Bezier spine that oscillates.
// Two parallel edge paths offset by ±width/2 perpendicular to the spine tangent.
//
// param_a: undulation frequency  (0..1 → 0.5..6 Hz)
// param_b: width                 (0..1 → 0..0.5 normalised)
// param_c: length                (0..1 → 0.2..1.0 normalised)

#include "igenerator.h"
#include <cmath>
#include <algorithm>
#include <array>

namespace idhmfis {

class GenRibbon final : public IGenerator {
public:
    const char* name()         const override { return "ribbon"; }
    const char* display_name() const override { return "Ribbon"; }
    const char* param_a_label()const override { return "Undulation"; }
    const char* param_b_label()const override { return "Width"; }
    const char* param_c_label()const override { return "Length"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        pts.reserve(p.point_count);

        float freq   = 0.5f + p.param_a * 5.5f;
        float width  = p.param_b * 0.5f;
        float length = 0.2f + p.param_c * 0.8f;
        float anim   = static_cast<float>(t) * p.speed;
        float scale  = p.scale;

        int n = p.point_count / 2; // half for each edge

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;

        // Sample spine + normals
        struct SpinePoint { float x, y, nx, ny; };
        std::vector<SpinePoint> spine(n);

        for (int i = 0; i < n; ++i)
        {
            float u  = static_cast<float>(i) / (n - 1); // 0..1 along ribbon
            float x  = (u * 2.f - 1.f) * length * scale;

            // Two sine waves at different frequencies for complex undulation
            float y  = scale * 0.3f * (
                std::sin(freq * u * 6.28318f + anim) +
                0.4f * std::sin(freq * 2.3f * u * 6.28318f + anim * 1.7f));

            // Numerical tangent
            float du = 0.01f;
            float u2 = std::min(u + du, 1.f);
            float x2 = (u2 * 2.f - 1.f) * length * scale;
            float y2 = scale * 0.3f * (
                std::sin(freq * u2 * 6.28318f + anim) +
                0.4f * std::sin(freq * 2.3f * u2 * 6.28318f + anim * 1.7f));

            float tx = x2 - x;
            float ty = y2 - y;
            float len = std::sqrt(tx*tx + ty*ty);
            if (len < 1e-6f) { tx = 1.f; ty = 0.f; } else { tx /= len; ty /= len; }

            // Normal = perpendicular to tangent
            spine[i] = { x, y, -ty, tx };
        }

        // Top edge (forward)
        pts.push_back(LaserPoint::from_norm(
            spine[0].x + spine[0].nx * width,
            spine[0].y + spine[0].ny * width,
            ca.r8(), ca.g8(), ca.b8(), true));

        for (int i = 0; i < n; ++i)
        {
            float frac = static_cast<float>(i) / (n - 1);
            float ex   = spine[i].x + spine[i].nx * width;
            float ey   = spine[i].y + spine[i].ny * width;
            Color4 col = ca.lerp(cb, frac);
            pts.push_back(LaserPoint::from_norm(ex, ey,
                col.r8(), col.g8(), col.b8(), false));
        }

        // Bottom edge (reverse)
        pts.push_back(LaserPoint::from_norm(
            spine[n-1].x - spine[n-1].nx * width,
            spine[n-1].y - spine[n-1].ny * width,
            cb.r8(), cb.g8(), cb.b8(), true));

        for (int i = n - 1; i >= 0; --i)
        {
            float frac = static_cast<float>(i) / (n - 1);
            float ex   = spine[i].x - spine[i].nx * width;
            float ey   = spine[i].y - spine[i].ny * width;
            Color4 col = cb.lerp(ca, frac);
            pts.push_back(LaserPoint::from_norm(ex, ey,
                col.r8(), col.g8(), col.b8(), false));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_ribbon()
{
    return std::make_unique<GenRibbon>();
}

} // namespace idhmfis
