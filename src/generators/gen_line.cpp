// gen_line.cpp — Simple line segment, optionally scanning.
//
// param_a : x1 endpoint  (0..1 → -1..1, default 0.1 → -0.8)
// param_b : x2 endpoint  (0..1 → -1..1, default 0.9 →  0.8)
// param_c : scan_speed   (0..1 → 0.1..20 Hz); scan_mode enabled when > 0
// rotation: encodes y position of both endpoints as a single tilt
//           (actual y1/y2 are both = tilt, i.e. horizontal line by default)
//           → y offset of endpoints: tilt ± pan
// pan     : half-spread of y endpoints (0 = both same y = straight horizontal)
// speed   : sweep animation speed multiplier
// color_a : beam colour
// scale   : segment scale

#include "igenerator.h"
#include <cmath>
#include <algorithm>

namespace idhmfis {

class GenLine final : public IGenerator {
public:
    const char* name()          const override { return "line"; }
    const char* display_name()  const override { return "Line"; }
    const char* param_a_label() const override { return "X Start (0=left)"; }
    const char* param_b_label() const override { return "X End (1=right)"; }
    const char* param_c_label() const override { return "Scan Speed"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        int n = std::max(2, p.point_count);
        pts.reserve(n);

        // Map param_a/b: 0..1 → -1..1
        float x1 = (p.param_a * 2.f - 1.f) * p.scale;
        float x2 = (p.param_b * 2.f - 1.f) * p.scale;

        // y positions encoded via tilt ± pan
        float y1 = p.tilt - p.pan;
        float y2 = p.tilt + p.pan;

        // Scan mode: param_c > 0 → sweep line back and forth
        if (p.param_c > 0.001f) {
            float scan_speed = 0.1f + p.param_c * 19.9f;  // 0.1..20 Hz
            float phase = static_cast<float>(t) * scan_speed * 2.f * 3.14159265f * p.speed;
            float sweep = std::sin(phase); // -1..1
            // Animate x offset by sweeping the whole line segment
            float line_half_w = (x2 - x1) * 0.5f;
            float center_x = (x1 + x2) * 0.5f;
            float offset = sweep * (1.f - std::abs(line_half_w));
            x1 = center_x - line_half_w + offset;
            x2 = center_x + line_half_w + offset;
        }

        Color4 col = p.color_a;
        uint8_t r = col.r8(), g = col.g8(), b = col.b8();

        for (int i = 0; i < n; ++i) {
            float frac = (n > 1) ? static_cast<float>(i) / (n - 1) : 0.f;
            float x = x1 + (x2 - x1) * frac;
            float y = y1 + (y2 - y1) * frac;
            bool blank = (i == 0);
            pts.push_back(LaserPoint::from_norm(x, y, r, g, b, blank));
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_line()
{
    return std::make_unique<GenLine>();
}

} // namespace idhmfis
