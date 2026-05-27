// zone_manager.cpp — ZoneManager implementation.
// All heavy-lifting stays in plain float arithmetic on LaserPoint coordinates.

#include "zone_manager.h"

#include <cmath>
#include <algorithm>
#include <cassert>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor — seed with one default zone so single-projector shows work
//  without any explicit zone setup.
// ─────────────────────────────────────────────────────────────────────────────
ZoneManager::ZoneManager()
{
    Zone z;
    z.id   = next_id_++;
    z.name = "Zone 1";
    zones_.push_back(std::move(z));
}

// ─────────────────────────────────────────────────────────────────────────────
//  CRUD
// ─────────────────────────────────────────────────────────────────────────────
int ZoneManager::add_zone()
{
    Zone z;
    z.id   = next_id_++;
    z.name = "Zone " + std::to_string(z.id);
    int id = z.id;
    zones_.push_back(std::move(z));
    return id;
}

void ZoneManager::remove_zone(int id)
{
    zones_.erase(
        std::remove_if(zones_.begin(), zones_.end(),
                       [id](const Zone& z){ return z.id == id; }),
        zones_.end());
}

Zone* ZoneManager::get_zone(int id)
{
    for (auto& z : zones_)
        if (z.id == id) return &z;
    return nullptr;
}

const Zone* ZoneManager::get_zone(int id) const
{
    for (const auto& z : zones_)
        if (z.id == id) return &z;
    return nullptr;
}

std::vector<Zone>& ZoneManager::zones()             { return zones_; }
const std::vector<Zone>& ZoneManager::zones() const { return zones_; }

// ─────────────────────────────────────────────────────────────────────────────
//  apply_transform
//  Transform order: scale -> shear -> rotate -> translate.
//  Keystone warp is a bilinear blend of per-corner offsets, applied after all
//  affine transforms so the warp is in output space.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer ZoneManager::apply_transform(const PointBuffer& src,
                                          const ZoneTransform& t) const
{
    // BUG #56 fix: NaN or non-positive scale propagates to the int16_t cast → UB.
    // Sanitize scale before use; non-finite or zero scale is treated as identity (1.0).
    const float scale_x = (std::isfinite(t.scale_x) && t.scale_x > 0.f) ? t.scale_x : 1.f;
    const float scale_y = (std::isfinite(t.scale_y) && t.scale_y > 0.f) ? t.scale_y : 1.f;

    // Pre-compute rotation coefficients (degrees -> radians).
    const float rad  = t.rotation * (3.14159265358979323846f / 180.f);
    const float cr   = std::cos(rad);
    const float sr   = std::sin(rad);

    PointBuffer dst;
    dst.reserve(src.size());

    for (const LaserPoint& in : src)
    {
        float x = in.nx();
        float y = in.ny();

        // 1. Scale (using sanitized values — see BUG #56 fix above)
        x *= scale_x;
        y *= scale_y;

        // 2. Shear  (x' = x + shear_x*y,  y' = y + shear_y*x)
        float xs = x + t.shear_x * y;
        float ys = y + t.shear_y * x;
        x = xs;
        y = ys;

        // 3. Rotation
        float xr = x * cr - y * sr;
        float yr = x * sr + y * cr;
        x = xr;
        y = yr;

        // 4. Translate
        x += t.offset_x;
        y += t.offset_y;

        // 5. Keystone warp (bilinear corner offsets).
        // The four corners of the normalised frame are TL(-1,1) TR(1,1) BL(-1,-1) BR(1,-1).
        // We compute bilinear weights from the point position *before* clamping,
        // so the warp is applied in a stable (-1..1) space.
        if (t.use_keystone)
        {
            // Map x,y to [0,1] for bilinear weight computation, clamped to avoid
            // extrapolation artefacts at screen edges.
            float wx = std::clamp((x + 1.f) * 0.5f, 0.f, 1.f);
            float wy = std::clamp((y + 1.f) * 0.5f, 0.f, 1.f);

            // Bilinear blend of corner delta-offsets.
            // Weights: BL (1-wx)(1-wy), BR wx(1-wy), TL (1-wx)wy, TR wx*wy
            float w_bl = (1.f - wx) * (1.f - wy);
            float w_br =        wx  * (1.f - wy);
            float w_tl = (1.f - wx) *        wy;
            float w_tr =        wx  *        wy;

            float dx = w_tl * t.kstone_tl[0] + w_tr * t.kstone_tr[0]
                     + w_bl * t.kstone_bl[0] + w_br * t.kstone_br[0];
            float dy = w_tl * t.kstone_tl[1] + w_tr * t.kstone_tr[1]
                     + w_bl * t.kstone_bl[1] + w_br * t.kstone_br[1];
            x += dx;
            y += dy;
        }

        LaserPoint out = in;
        // BUG #56 fix: clamp to [-32768, 32767] float range before cast to guard
        // against NaN/Inf from any upstream arithmetic (shear, rotation, keystone).
        // std::clamp on NaN is unspecified, so replace NaN with 0 first.
        float cx = std::isfinite(x) ? x : 0.f;
        float cy = std::isfinite(y) ? y : 0.f;
        out.x = static_cast<int16_t>(std::clamp(cx * 32767.f, -32768.f, 32767.f));
        out.y = static_cast<int16_t>(std::clamp(cy * 32767.f, -32768.f, 32767.f));
        dst.push_back(out);
    }
    return dst;
}

// ─────────────────────────────────────────────────────────────────────────────
//  apply_color — multiply each lit point's RGB by the per-zone correction.
// ─────────────────────────────────────────────────────────────────────────────
void ZoneManager::apply_color(PointBuffer& buf,
                               float r, float g, float b,
                               float intensity) const
{
    const float fr = std::clamp(r * intensity, 0.f, 1.f);
    const float fg = std::clamp(g * intensity, 0.f, 1.f);
    const float fb = std::clamp(b * intensity, 0.f, 1.f);

    for (LaserPoint& pt : buf)
    {
        if (!pt.blanked)
        {
            pt.r = static_cast<uint8_t>(pt.r * fr);
            pt.g = static_cast<uint8_t>(pt.g * fg);
            pt.b = static_cast<uint8_t>(pt.b * fb);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  test_pattern — geometric test shapes for projector alignment.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer ZoneManager::test_pattern(int pattern_type, int point_count) const
{
    // BUG #25 fix: many pattern branches divide by point_count or derived quantities.
    // Enforce a minimum so none of those denominators can be zero or negative.
    if (point_count < 2) return {};

    PointBuffer buf;

    static constexpr float kPi    = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.f * kPi;

    // Convenience: add a lit point at normalised coordinates.
    auto lit = [](float nx, float ny, uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
        return LaserPoint::from_norm(nx, ny, r, g, b, false);
    };
    auto blank = [](float nx, float ny) {
        return LaserPoint::from_norm(nx, ny, 0, 0, 0, true);
    };

    switch (pattern_type)
    {
    // ── 0: Circle ─────────────────────────────────────────────────────────
    case 0:
    {
        buf.reserve(static_cast<size_t>(point_count));
        for (int i = 0; i < point_count; ++i)
        {
            float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(point_count);
            buf.push_back(lit(std::cos(angle), std::sin(angle)));
        }
        break;
    }

    // ── 1: Crosshair ──────────────────────────────────────────────────────
    // Horizontal line then vertical line, each 256 pts; blank gap at origin.
    case 1:
    {
        // BUG #25 fix: half - 1 is used as a divisor; need half >= 2 (point_count >= 4).
        if (point_count < 4) return {};
        const int half = point_count / 2;
        const float gap = 0.05f;
        buf.reserve(static_cast<size_t>(point_count) + 4);

        // Horizontal arm (left → right, skip small gap at centre)
        for (int i = 0; i < half; ++i)
        {
            float x = -1.f + 2.f * static_cast<float>(i) / static_cast<float>(half - 1);
            if (std::fabs(x) < gap)
                buf.push_back(blank(x, 0.f));
            else
                buf.push_back(lit(x, 0.f));
        }
        // Jump to start of vertical arm (blanked travel move)
        buf.push_back(blank(0.f, -1.f));

        // Vertical arm (bottom → top, skip small gap at centre)
        for (int i = 0; i < half; ++i)
        {
            float y = -1.f + 2.f * static_cast<float>(i) / static_cast<float>(half - 1);
            if (std::fabs(y) < gap)
                buf.push_back(blank(0.f, y));
            else
                buf.push_back(lit(0.f, y));
        }
        break;
    }

    // ── 2: Box ────────────────────────────────────────────────────────────
    // Square outline from (-0.8,-0.8) to (0.8,0.8).
    case 2:
    {
        // BUG #25 fix: side - 1 is used as a divisor; need side >= 2 (point_count >= 8).
        if (point_count < 8) return {};
        const float ext = 0.8f;
        const int side  = point_count / 4;
        buf.reserve(static_cast<size_t>(point_count) + 4);

        // Bottom edge: left -> right
        buf.push_back(blank(-ext, -ext));  // jump to start
        for (int i = 0; i < side; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(side - 1);
            buf.push_back(lit(-ext + 2.f * ext * t, -ext));
        }
        // Right edge: bottom -> top
        for (int i = 0; i < side; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(side - 1);
            buf.push_back(lit(ext, -ext + 2.f * ext * t));
        }
        // Top edge: right -> left
        for (int i = 0; i < side; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(side - 1);
            buf.push_back(lit(ext - 2.f * ext * t, ext));
        }
        // Left edge: top -> bottom
        for (int i = 0; i < side; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(side - 1);
            buf.push_back(lit(-ext, ext - 2.f * ext * t));
        }
        break;
    }

    // ── 3: Star ───────────────────────────────────────────────────────────
    // 5-pointed star drawn as a continuous stroke using the standard
    // {0,2,4,1,3,0} inner/outer point sequence.
    case 3:
    {
        // BUG #25 fix: seg_pts = point_count/5 is used as a divisor; need seg_pts >= 1
        // (point_count >= 5). Also need point_count >= 10 for the inner/outer vertices
        // to be evenly distributed (each segment has at least 2 points).
        if (point_count < 10) return {};
        // Build outer and inner vertices of a regular 5-point star.
        const float outer_r = 0.85f;
        const float inner_r = 0.35f;
        const int   tips    = 5;

        struct Vec2 { float x, y; };
        Vec2 pts[10];
        for (int i = 0; i < tips; ++i)
        {
            float outer_a = kPi * 0.5f + kTwoPi * static_cast<float>(i) / static_cast<float>(tips);
            float inner_a = outer_a + kPi / static_cast<float>(tips);
            pts[2 * i]     = { outer_r * std::cos(outer_a), outer_r * std::sin(outer_a) };
            pts[2 * i + 1] = { inner_r * std::cos(inner_a), inner_r * std::sin(inner_a) };
        }

        // Stroke order for a classic 5-point star: 0,4,8,2,6,0
        const int order[] = { 0, 4, 8, 2, 6, 0 };
        const int seg_pts = point_count / 5;

        buf.push_back(blank(pts[0].x, pts[0].y));  // travel to start

        for (int seg = 0; seg < 5; ++seg)
        {
            Vec2 from = pts[order[seg]];
            Vec2 to   = pts[order[seg + 1]];
            for (int i = 0; i <= seg_pts; ++i)
            {
                float s = static_cast<float>(i) / static_cast<float>(seg_pts);
                float x = from.x + (to.x - from.x) * s;
                float y = from.y + (to.y - from.y) * s;
                buf.push_back(lit(x, y));
            }
        }
        break;
    }

    // ── 4: Scanlines ──────────────────────────────────────────────────────
    // 8 horizontal lines evenly spaced from y=-0.8 to y=0.8, 64 pts each.
    case 4:
    default:
    {
        const int   num_lines = 8;
        const int   pts_per   = 64;
        const float y_min     = -0.8f;
        const float y_max     =  0.8f;
        const float x_min     = -0.8f;
        const float x_max     =  0.8f;

        buf.reserve(static_cast<size_t>(num_lines * pts_per + num_lines));

        for (int li = 0; li < num_lines; ++li)
        {
            float y = y_min + (y_max - y_min) * static_cast<float>(li)
                              / static_cast<float>(num_lines - 1);
            bool left_to_right = (li % 2 == 0);
            float x_start = left_to_right ? x_min : x_max;
            float x_end   = left_to_right ? x_max : x_min;

            // Blank jump to line start
            buf.push_back(blank(x_start, y));

            for (int pi = 0; pi < pts_per; ++pi)
            {
                float t = static_cast<float>(pi) / static_cast<float>(pts_per - 1);
                float x = x_start + (x_end - x_start) * t;
                buf.push_back(lit(x, y));
            }
        }
        break;
    }
    } // switch

    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  route — distribute master buffer to each enabled zone.
// ─────────────────────────────────────────────────────────────────────────────
std::unordered_map<int, PointBuffer>
ZoneManager::route(const PointBuffer& master) const
{
    std::unordered_map<int, PointBuffer> result;

    // Determine if any zone has solo active.
    bool any_solo = false;
    for (const Zone& z : zones_)
        if (z.enabled && z.solo) { any_solo = true; break; }

    for (const Zone& z : zones_)
    {
        if (!z.enabled) continue;
        if (z.blind)    continue;
        if (any_solo && !z.solo) continue;

        if (z.show_test)
        {
            // Test pattern replaces the master content entirely.
            result[z.id] = test_pattern(z.test_pattern);
        }
        else
        {
            // Copy master, apply geometric transform, then colour correction.
            PointBuffer buf = apply_transform(master, z.transform);
            apply_color(buf, z.color_r, z.color_g, z.color_b, z.intensity);
            result[z.id] = std::move(buf);
        }
    }

    return result;
}

} // namespace idhmfis
