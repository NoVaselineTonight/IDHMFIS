// gen_sierpinski.cpp — Animated Sierpinski triangle / gasket fractal.
//
// Iterates the classic "chaos game" algorithm: three vertices, repeatedly
// pick a random vertex and jump half-way toward it.  The resulting dot cloud
// converges to the Sierpinski gasket.  Because laser shows need line
// segments rather than dots, we draw connected lines between consecutive
// chaos-game points, which traces a ghostly fractal web.
//
// param_a : depth       (0..1 → 4..11 chaos-game iterations per frame)
// param_b : rotation    (0..1 → 0..2π; vertex triangle rotation)
// param_c : variant     (0=triangle gasket, 0.33=carpet/square, 0.66=pentagon, 1=hexagon)
//
// speed   : continuous rotation rate (rad/s)
// scale   : bounding radius (0..1 normalised space)
// density : colour-by-depth gradient: 0=solid, 1=full gradient spectrum
// rotation: static rotation offset added to speed

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {
namespace {

static constexpr float kTwoPi = 6.28318530717958647692f;

static float pr2(unsigned& s) {
    s = s * 1664525u + 1013904223u;
    return static_cast<float>(s >> 16) / 65535.f;
}

class GenSierpinski final : public IGenerator {
public:
    const char* name()          const override { return "sierpinski"; }
    const char* display_name()  const override { return "Sierpinski"; }
    const char* param_a_label() const override { return "Iterations"; }
    const char* param_b_label() const override { return "Rotation"; }
    const char* param_c_label() const override { return "Variant"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer out;

        // Number of points capped to point_count budget
        int iters = 4 + static_cast<int>(p.param_a * 7.f);  // 4..11
        int n_pts = std::min(1 << iters, std::max(16, p.point_count));

        // Variant selects polygon order (3=triangle, 4=square, 5=pentagon, 6=hexagon)
        int sides = 3 + static_cast<int>(p.param_c * 3.99f);  // 3..6

        // Rotation over time
        float angle_base = p.rotation + static_cast<float>(t) * p.speed
                         + p.param_b * kTwoPi;

        // Build anchor vertices
        std::vector<float> vx(static_cast<size_t>(sides)), vy(static_cast<size_t>(sides));
        for (int i = 0; i < sides; ++i) {
            float a = angle_base + kTwoPi * static_cast<float>(i) / static_cast<float>(sides);
            vx[static_cast<size_t>(i)] = p.pan  + p.scale * std::cos(a);
            vy[static_cast<size_t>(i)] = p.tilt + p.scale * std::sin(a);
        }

        // Chaos game seed (stable per frame to prevent shimmer)
        unsigned seed = 0xCAFEBABEu;

        // Run chaos game — track (cx,cy) between iterations
        float cx = p.pan, cy = p.tilt;
        // Warm up
        for (int i = 0; i < 64; ++i) {
            int v = static_cast<int>(pr2(seed) * static_cast<float>(sides)) % sides;
            cx = (cx + vx[static_cast<size_t>(v)]) * 0.5f;
            cy = (cy + vy[static_cast<size_t>(v)]) * 0.5f;
        }

        // First point — blank travel
        out.push_back(LaserPoint::from_norm(cx, cy,
            p.color_a.r8(), p.color_a.g8(), p.color_a.b8(), true));

        for (int i = 0; i < n_pts; ++i) {
            int v = static_cast<int>(pr2(seed) * static_cast<float>(sides)) % sides;
            cx = (cx + vx[static_cast<size_t>(v)]) * 0.5f;
            cy = (cy + vy[static_cast<size_t>(v)]) * 0.5f;

            // Colour by fractional depth (density controls how much the gradient sweeps)
            float u = p.density > 0.f
                ? std::fmod(static_cast<float>(i) / static_cast<float>(n_pts) * p.density * 4.f, 1.f)
                : 0.f;
            Color4 c = p.color_a.lerp(p.color_b, u);

            out.push_back(LaserPoint::from_norm(cx, cy,
                c.r8(), c.g8(), c.b8(), false));
        }

        return out;
    }
};

} // namespace

std::unique_ptr<IGenerator> make_gen_sierpinski()
{
    return std::make_unique<GenSierpinski>();
}

} // namespace idhmfis
