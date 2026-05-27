// §B1 — 7-Step Point Optimizer Implementation

#include "point_optimizer.h"

#include <cmath>
#include <algorithm>
#include <vector>
#include <limits>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────
PointOptimizer::PointOptimizer(const OptimizerConfig& cfg)
    : cfg_(cfg)
{}

void PointOptimizer::set_config(const OptimizerConfig& cfg)
{
    cfg_ = cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline float pt_dist2(const LaserPoint& a, const LaserPoint& b)
{
    float dx = a.nx() - b.nx();
    float dy = a.ny() - b.ny();
    return dx*dx + dy*dy;
}

static inline float pt_dist(const LaserPoint& a, const LaserPoint& b)
{
    return std::sqrt(pt_dist2(a, b));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Optimize — run all 7 steps in order
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::optimize(const PointBuffer& input) const
{
    if (input.empty()) return {};

    PointBuffer buf = step_flatten(input);
    buf = step_anchor(buf);
    buf = step_color_edge_interp(buf);
    buf = step_blanking(buf);
    buf = step_path_ordering(buf);
    // step_path_ordering strips all blank-dwell copies and rebuilds with only
    // one blank travel point per segment.  Re-run anchor so galvos have proper
    // settle time before each lit segment in the reordered output.
    buf = step_anchor(buf);
    buf = step_density_norm(buf);
    buf = step_overscan_clip(buf);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 1 — flatten
//  Remove consecutive points that are at the same position (within epsilon).
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::step_flatten(const PointBuffer& in) const
{
    constexpr float kEps2 = 1e-5f * 1e-5f;

    if (in.empty()) return {};

    PointBuffer out;
    out.reserve(in.size());
    out.push_back(in[0]);

    for (size_t i = 1; i < in.size(); ++i)
    {
        const LaserPoint& prev = out.back();
        const LaserPoint& cur  = in[i];

        // Keep if position differs, OR if blanked state differs
        if (pt_dist2(prev, cur) > kEps2 || prev.blanked != cur.blanked)
            out.push_back(cur);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 2 — anchor
//  - At blank-to-lit transitions: insert blank_dwell copies of the lit point
//    with blanked=true before the actual lit point.
//  - At sharp corners (angle > threshold) among lit points: insert corner_dwell
//    copies of the corner point.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::step_anchor(const PointBuffer& in) const
{
    if (in.empty()) return {};

    int blank_dwell  = std::max(0, static_cast<int>(cfg_.blank_dwell));
    int corner_dwell = std::max(0, static_cast<int>(cfg_.corner_dwell));

    PointBuffer out;
    out.reserve(in.size() + in.size() / 4);

    for (size_t i = 0; i < in.size(); ++i)
    {
        const LaserPoint& cur = in[i];

        // ── Blank-to-lit transition ───────────────────────────────────────
        if (i > 0 && !cur.blanked && in[i-1].blanked)
        {
            // Insert blank_dwell blanked copies of the first lit point
            LaserPoint dwell = cur;
            dwell.blanked = true;
            for (int d = 0; d < blank_dwell; ++d)
                out.push_back(dwell);
        }

        // ── Sharp corner dwell ────────────────────────────────────────────
        // Check if cur is a lit corner: we need i-1 and i+1 both lit.
        if (corner_dwell > 0 && i > 0 && i + 1 < in.size() &&
            !cur.blanked && !in[i-1].blanked && !in[i+1].blanked)
        {
            // Vectors: prev→cur and cur→next
            float ax = cur.nx()      - in[i-1].nx();
            float ay = cur.ny()      - in[i-1].ny();
            float bx = in[i+1].nx() - cur.nx();
            float by = in[i+1].ny() - cur.ny();

            float len_a = std::sqrt(ax*ax + ay*ay);
            float len_b = std::sqrt(bx*bx + by*by);

            if (len_a > 1e-7f && len_b > 1e-7f)
            {
                float dot   = (ax*bx + ay*by) / (len_a * len_b);
                dot = std::clamp(dot, -1.f, 1.f);
                float angle = std::acos(dot);  // angle between segments

                if (angle > cfg_.corner_angle_threshold)
                {
                    for (int d = 0; d < corner_dwell; ++d)
                        out.push_back(cur);
                }
            }
        }

        out.push_back(cur);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 3 — color-edge-interp
//  At abrupt color transitions between adjacent lit points, insert 2 linearly
//  interpolated points to soften colour smearing.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::step_color_edge_interp(const PointBuffer& in) const
{
    if (in.size() < 2) return in;

    PointBuffer out;
    out.reserve(in.size() + in.size() / 8);

    // Threshold: consider "abrupt" if any channel diff > ~30/255
    constexpr int kColorThreshold = 30;

    out.push_back(in[0]);

    for (size_t i = 1; i < in.size(); ++i)
    {
        const LaserPoint& prev = out.back();
        const LaserPoint& cur  = in[i];

        if (!prev.blanked && !cur.blanked)
        {
            int dr = static_cast<int>(cur.r) - static_cast<int>(prev.r);
            int dg = static_cast<int>(cur.g) - static_cast<int>(prev.g);
            int db = static_cast<int>(cur.b) - static_cast<int>(prev.b);

            bool abrupt = (std::abs(dr) > kColorThreshold) ||
                          (std::abs(dg) > kColorThreshold) ||
                          (std::abs(db) > kColorThreshold);

            if (abrupt)
            {
                // Insert two interpolated points at t=1/3 and t=2/3
                for (int k = 1; k <= 2; ++k)
                {
                    float t  = static_cast<float>(k) / 3.f;
                    float ix = prev.nx() + (cur.nx() - prev.nx()) * t;
                    float iy = prev.ny() + (cur.ny() - prev.ny()) * t;
                    auto r8  = static_cast<uint8_t>(prev.r + static_cast<int>(dr * t));
                    auto g8  = static_cast<uint8_t>(prev.g + static_cast<int>(dg * t));
                    auto b8  = static_cast<uint8_t>(prev.b + static_cast<int>(db * t));
                    out.push_back(LaserPoint::from_norm(ix, iy, r8, g8, b8, false));
                }
            }
        }
        out.push_back(cur);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 4 — blanking
//  At the last lit point before a blank gap (>1 blanked point), insert 2 extra
//  dwell copies of the last lit point.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::step_blanking(const PointBuffer& in) const
{
    if (in.size() < 2) return in;

    PointBuffer out;
    out.reserve(in.size() + in.size() / 8);

    for (size_t i = 0; i < in.size(); ++i)
    {
        out.push_back(in[i]);

        // If this is a lit point followed by a long blank run
        if (!in[i].blanked && i + 1 < in.size() && in[i+1].blanked)
        {
            // Count the blank run length
            size_t blank_run = 0;
            for (size_t j = i + 1; j < in.size() && in[j].blanked; ++j)
                ++blank_run;

            if (blank_run > 1)
            {
                // Insert 2 extra dwell copies of the last lit point
                for (int d = 0; d < 2; ++d)
                    out.push_back(in[i]);
            }
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 5 — path-ordering
//  Split buffer at blanking gaps into segments, then reorder segments using
//  greedy nearest-neighbour to minimise total travel distance.
//  Also flips individual segments if doing so brings the end point closer to
//  the next segment's start.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct Segment {
    PointBuffer pts;    // does NOT include leading blank travel
    bool        valid = true;
};

// Split a PointBuffer into lit segments at blanking transitions.
// Each segment is a run of lit points (blanked=false).
// The leading blanked travel points are discarded; we'll rebuild them.
static std::vector<Segment> split_segments(const PointBuffer& in)
{
    std::vector<Segment> segs;
    Segment cur;

    for (const auto& pt : in)
    {
        if (pt.blanked)
        {
            if (!cur.pts.empty()) {
                segs.push_back(std::move(cur));
                cur = Segment{};
            }
        }
        else
        {
            cur.pts.push_back(pt);
        }
    }
    if (!cur.pts.empty())
        segs.push_back(std::move(cur));

    return segs;
}

// Rejoin segments with blanking travel points between them.
static PointBuffer rejoin_segments(const std::vector<Segment>& segs)
{
    PointBuffer out;
    for (const auto& seg : segs)
    {
        if (seg.pts.empty()) continue;
        // Blank travel to segment start
        LaserPoint blank = seg.pts.front();
        blank.blanked = true;
        out.push_back(blank);
        for (const auto& pt : seg.pts)
            out.push_back(pt);
    }
    return out;
}

} // anonymous namespace

PointBuffer PointOptimizer::step_path_ordering(const PointBuffer& in) const
{
    if (!cfg_.enable_reorder) return in;

    auto segs = split_segments(in);
    if (segs.size() <= 1) return in;

    size_t n = segs.size();
    std::vector<bool> used(n, false);
    std::vector<Segment> ordered;
    ordered.reserve(n);

    // Start from segment 0
    size_t cur_seg = 0;
    used[0] = true;
    ordered.push_back(std::move(segs[0]));

    for (size_t step = 1; step < n; ++step)
    {
        const LaserPoint& tail = ordered.back().pts.back();

        float best_dist = std::numeric_limits<float>::max();
        size_t best_idx = 0;
        bool   best_flip = false;

        for (size_t j = 0; j < n; ++j)
        {
            if (used[j]) continue;
            const Segment& cand = segs[j];
            if (cand.pts.empty()) continue;

            float d_fwd = pt_dist(tail, cand.pts.front());
            float d_rev = pt_dist(tail, cand.pts.back());

            if (d_fwd < best_dist) { best_dist = d_fwd; best_idx = j; best_flip = false; }
            if (d_rev < best_dist) { best_dist = d_rev; best_idx = j; best_flip = true;  }
        }

        used[best_idx] = true;
        Segment& chosen = segs[best_idx];
        if (best_flip)
            std::reverse(chosen.pts.begin(), chosen.pts.end());
        ordered.push_back(std::move(chosen));
        cur_seg = best_idx;
        (void)cur_seg; // suppress unused variable warning
    }

    return rejoin_segments(ordered);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 6 — density-norm
//  Budget = target_pps / 60 points per frame.
//  - Interpolate gaps that are too long (long segments → insert intermediate pts)
//  - Skip duplicate-adjacent points on very dense segments
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::step_density_norm(const PointBuffer& in) const
{
    if (in.size() < 2) return in;

    int budget = cfg_.target_pps / 60;
    if (budget <= 0) return in;

    // First pass: compute total path length (only lit segments)
    float total_len = 0.f;
    for (size_t i = 1; i < in.size(); ++i)
    {
        if (!in[i].blanked && !in[i-1].blanked)
            total_len += pt_dist(in[i-1], in[i]);
    }

    if (total_len < 1e-6f) {
        // H-15: degenerate frame — all lit segments have zero length (identical
        // positions, all-blank, or a single-point frame). Returning `in` unchanged
        // could pass thousands of duplicate points to the DAC, wasting bandwidth
        // and creating a visible bright spot. Clamp to the caller-supplied budget
        // so the DAC never receives more points than it requested.
        if (static_cast<int>(in.size()) <= budget) return in;
        return PointBuffer(in.begin(), in.begin() + static_cast<ptrdiff_t>(budget));
    }

    // Points per unit length
    float pts_per_unit = static_cast<float>(budget) / total_len;

    PointBuffer out;
    out.reserve(static_cast<size_t>(budget) + 16);
    out.push_back(in[0]);

    for (size_t i = 1; i < in.size(); ++i)
    {
        const LaserPoint& prev = out.empty() ? in[i-1] : out.back();
        const LaserPoint& cur  = in[i];

        if (cur.blanked || prev.blanked) {
            out.push_back(cur);
            continue;
        }

        float seg_len = pt_dist(prev, cur);
        if (seg_len < 1e-7f) continue; // skip zero-length

        int extra_pts = static_cast<int>(seg_len * pts_per_unit) - 1;

        if (extra_pts > 0 && extra_pts < 1000) {
            // Interpolate
            for (int k = 1; k <= extra_pts; ++k) {
                float t  = static_cast<float>(k) / static_cast<float>(extra_pts + 1);
                float ix = prev.nx() + (cur.nx() - prev.nx()) * t;
                float iy = prev.ny() + (cur.ny() - prev.ny()) * t;
                auto  r8 = static_cast<uint8_t>(prev.r + static_cast<int>((static_cast<int>(cur.r) - static_cast<int>(prev.r)) * t));
                auto  g8 = static_cast<uint8_t>(prev.g + static_cast<int>((static_cast<int>(cur.g) - static_cast<int>(prev.g)) * t));
                auto  b8 = static_cast<uint8_t>(prev.b + static_cast<int>((static_cast<int>(cur.b) - static_cast<int>(prev.b)) * t));
                out.push_back(LaserPoint::from_norm(ix, iy, r8, g8, b8, false));
            }
        }
        out.push_back(cur);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step 7 — overscan-clip
//  Remove any point where |x| > 1+margin OR |y| > 1+margin.
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer PointOptimizer::step_overscan_clip(const PointBuffer& in) const
{
    if (!cfg_.enable_overscan_clip) return in;

    float lim = 1.f + cfg_.overscan_margin;

    PointBuffer out;
    out.reserve(in.size());

    for (const auto& pt : in)
    {
        float nx = pt.nx();
        float ny = pt.ny();
        if (std::abs(nx) <= lim && std::abs(ny) <= lim)
            out.push_back(pt);
    }
    return out;
}

} // namespace idhmfis
