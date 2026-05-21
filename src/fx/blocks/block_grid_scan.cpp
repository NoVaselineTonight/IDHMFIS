#include "block_grid_scan.h"
#include <cmath>
#include <algorithm>

// Grid Scan — appends animated H/V scan line segments to the point buffer.
// Effect #5 in R13: horizontal or vertical scan lines sweeping the field,
// stacked and staggered to create the classic raster laser aesthetic.

namespace idhmfis {

BlockGridScan::BlockGridScan()
{
    p_direction_  = { "direction",  0.f, 0.f,  0.f,  2.f, 1.f,   "" };
    p_line_count_ = { "line_count", 6.f, 0.f,  2.f, 32.f, 1.f,   "" };
    p_stagger_    = { "stagger",    0.5f, 0.f, 0.f,  1.f, 0.01f, "" };
    p_line_pts_   = { "line_pts",   8.f, 0.f,  2.f, 32.f, 1.f,   "" };
    p_color_r_    = { "color_r",    1.f, 0.f,  0.f,  1.f, 0.01f, "" };
    p_color_g_    = { "color_g",    0.f, 0.f,  0.f,  1.f, 0.01f, "" };
    p_color_b_    = { "color_b",    1.f, 0.f,  0.f,  1.f, 0.01f, "" };
    p_beat_sync_  = { "beat_sync",  0.f, 0.f,  0.f,  1.f, 1.f,   "" };
}

std::vector<FxParam*> BlockGridScan::params()
{
    return { &p_direction_, &p_line_count_, &p_stagger_,
             &p_line_pts_,
             &p_color_r_, &p_color_g_, &p_color_b_,
             &p_beat_sync_ };
}

const std::vector<FxParam*> BlockGridScan::params() const
{
    return { const_cast<FxParam*>(&p_direction_),
             const_cast<FxParam*>(&p_line_count_),
             const_cast<FxParam*>(&p_stagger_),
             const_cast<FxParam*>(&p_line_pts_),
             const_cast<FxParam*>(&p_color_r_),
             const_cast<FxParam*>(&p_color_g_),
             const_cast<FxParam*>(&p_color_b_),
             const_cast<FxParam*>(&p_beat_sync_) };
}

void BlockGridScan::process(PointBuffer& buf, float dt, const ExprContext& ctx)
{
    // Accumulate animation time; beat_sync overrides with bar phase
    const bool beat_sync = (p_beat_sync_.effective() >= 0.5f);
    if (beat_sync) {
        anim_ = static_cast<float>(ctx.bar);
    } else {
        anim_ += dt;  // animation advances at unit rate; wet/dry mix is handled by FxEngine
        anim_  = std::fmod(anim_, 1000.f);  // prevent float overflow over long shows
    }

    const int   lines   = std::clamp(static_cast<int>(
                              std::round(p_line_count_.effective())), 2, 32);
    const float stagger = p_stagger_.effective();  // 0..1
    const int   lpts    = std::clamp(static_cast<int>(
                              std::round(p_line_pts_.effective())), 2, 32);
    const int   dir     = std::clamp(static_cast<int>(
                              std::round(p_direction_.effective())), 0, 2);

    const uint8_t cr = static_cast<uint8_t>(
        std::clamp(p_color_r_.effective(), 0.f, 1.f) * 255.f);
    const uint8_t cg = static_cast<uint8_t>(
        std::clamp(p_color_g_.effective(), 0.f, 1.f) * 255.f);
    const uint8_t cb = static_cast<uint8_t>(
        std::clamp(p_color_b_.effective(), 0.f, 1.f) * 255.f);

    // Reserve space for additional points (worst case: both axes)
    const int axes  = (dir == 2) ? 2 : 1;
    buf.reserve(buf.size() + static_cast<size_t>(lines * axes * (lpts + 1)));

    // Helper: emit one scan line as a small segment sweeping perpendicular axis.
    // cross_pos: position along the sweep axis (-1..1)
    // is_horiz: true = horizontal line (fixed y, scanning x), false = vertical
    auto emit_line = [&](float cross_pos, bool is_horiz)
    {
        // Blank travel to line start
        float sx = is_horiz ? -1.f : cross_pos;
        float sy = is_horiz ? cross_pos : -1.f;
        buf.push_back(LaserPoint::from_norm(sx, sy, cr, cg, cb, true));

        for (int j = 0; j < lpts; ++j)
        {
            float u   = static_cast<float>(j) / static_cast<float>(lpts - 1);
            float along = u * 2.f - 1.f;  // -1..1 along the line direction
            float x   = is_horiz ? along : cross_pos;
            float y   = is_horiz ? cross_pos : along;
            buf.push_back(LaserPoint::from_norm(x, y, cr, cg, cb, false));
        }
    };

    // Emit horizontal scan lines
    if (dir == 0 || dir == 2)
    {
        for (int li = 0; li < lines; ++li)
        {
            float phase_off = stagger * static_cast<float>(li)
                            / static_cast<float>(lines);
            float phase = std::fmod(anim_ + phase_off, 1.f);
            // Triangle sweep: -1..+1 (full field height)
            float sweep = (phase < 0.5f) ? phase * 4.f - 1.f
                                         : 3.f - phase * 4.f;
            emit_line(sweep, true);
        }
    }

    // Emit vertical scan lines
    if (dir == 1 || dir == 2)
    {
        for (int li = 0; li < lines; ++li)
        {
            float phase_off = stagger * static_cast<float>(li)
                            / static_cast<float>(lines);
            float phase = std::fmod(anim_ + phase_off + 0.5f, 1.f); // offset so V/H not coincident
            float sweep = (phase < 0.5f) ? phase * 4.f - 1.f
                                         : 3.f - phase * 4.f;
            emit_line(sweep, false);
        }
    }
}

} // namespace idhmfis
