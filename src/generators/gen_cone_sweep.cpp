// Cone Sweep generator — 3D cone projection pattern (top-down conic section)
#include "igenerator.h"
#include <cmath>
#include <memory>

namespace idhmfis {
namespace {

static constexpr float kPi  = 3.14159265358979323846f;
static constexpr float k2Pi = 6.28318530717958647692f;

class ConeSweepGenerator final : public IGenerator {
public:
    const char* name()         const override { return "cone_sweep"; }
    const char* display_name() const override { return "Cone Sweep"; }
    const char* param_a_label()const override { return "Aperture Angle"; }
    const char* param_b_label()const override { return "Height"; }
    const char* param_c_label()const override { return "Rotation Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override {
        PointBuffer pts;

        float aperture = p.param_a * kPi * 0.8f + 0.1f;  // 0.1 to 0.9*pi radians
        float height   = p.param_b * p.scale;
        float rot      = static_cast<float>(t * p.param_c * p.speed);
        int   spokes   = 8;

        // The cone apex is at center, "base" is at the top of the screen.
        // Each spoke radiates from a ring at 'height' to the apex.
        // We project the 3D cone onto 2D by treating Z as depth falloff.
        pts.reserve(spokes * 4 + 32);

        for (int i = 0; i < spokes; ++i) {
            float angle = k2Pi * i / spokes + rot;
            float rim_x = std::sin(angle) * std::sin(aperture) * p.scale;
            float rim_y = std::cos(aperture) * height;
            float rim_z = std::cos(angle) * std::sin(aperture); // depth

            // Simple perspective: scale by 1 / (1 + depth * 0.3)
            float depth_scale = 1.f / (1.f + rim_z * 0.3f);
            rim_x *= depth_scale;
            rim_y *= depth_scale;

            float frac = (float)i / spokes;
            Color4 col = p.color_a.lerp(p.color_b, frac);
            float bright = 0.5f + 0.5f * (1.f - rim_z);  // front brighter
            uint8_t r = (uint8_t)(col.r * p.intensity * bright * 255.f);
            uint8_t g = (uint8_t)(col.g * p.intensity * bright * 255.f);
            uint8_t b = (uint8_t)(col.b * p.intensity * bright * 255.f);

            // Spoke from apex (center) to rim
            pts.push_back(LaserPoint::from_norm(0.f, 0.f, 0, 0, 0, true));
            pts.push_back(LaserPoint::from_norm(0.f, 0.f, r, g, b, false));
            pts.push_back(LaserPoint::from_norm(rim_x, rim_y, r, g, b, false));
        }

        // Draw the rim circle (top of cone)
        int ring_pts = 64;
        pts.push_back(LaserPoint::from_norm(
            std::sin(rot) * std::sin(aperture) * p.scale,
            std::cos(aperture) * height, 0, 0, 0, true));
        for (int i = 0; i <= ring_pts; ++i) {
            float angle = k2Pi * i / ring_pts + rot;
            float rx    = std::sin(angle) * std::sin(aperture) * p.scale;
            float ry    = std::cos(aperture) * height;
            float rz    = std::cos(angle) * std::sin(aperture);
            float ds    = 1.f / (1.f + rz * 0.3f);
            rx *= ds; ry *= ds;

            float frac = (float)i / ring_pts;
            Color4 col = p.color_a.lerp(p.color_b, frac);
            uint8_t r = (uint8_t)(col.r * p.intensity * 255.f);
            uint8_t g = (uint8_t)(col.g * p.intensity * 255.f);
            uint8_t b = (uint8_t)(col.b * p.intensity * 255.f);
            pts.push_back(LaserPoint::from_norm(rx, ry, r, g, b, i == 0));
        }

        return pts;
    }
};

} // anonymous namespace

std::unique_ptr<IGenerator> make_gen_cone_sweep()
{
    return std::make_unique<ConeSweepGenerator>();
}

} // namespace idhmfis
