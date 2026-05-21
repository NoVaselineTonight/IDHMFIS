// bam.cpp — Beam Attenuation Map implementation.
// Safety-critical: incorrect attenuation can cause audience eye injury.

#include "bam.h"
#include <algorithm>
#include <cmath>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  BamGrid
// ─────────────────────────────────────────────────────────────────────────────

float BamGrid::sample(float x, float y) const
{
    // Map normalized -1..1 to 0..kSize-1
    float fx = (x + 1.f) * 0.5f * static_cast<float>(kSize - 1);
    float fy = (y + 1.f) * 0.5f * static_cast<float>(kSize - 1);

    // Clamp to valid grid range
    int col = static_cast<int>(fx);
    int row = static_cast<int>(fy);
    col = std::clamp(col, 0, kSize - 1);
    row = std::clamp(row, 0, kSize - 1);

    return static_cast<float>(cells[row][col]) / 255.f;
}

void BamGrid::set_rect(float x0, float y0, float x1, float y1, uint8_t val)
{
    // Normalize so (x0,y0) <= (x1,y1)
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);

    // Map to cell indices (inclusive on both ends)
    auto to_col = [](float x) {
        float fx = (x + 1.f) * 0.5f * static_cast<float>(kSize - 1);
        return std::clamp(static_cast<int>(fx), 0, kSize - 1);
    };
    auto to_row = [](float y) {
        float fy = (y + 1.f) * 0.5f * static_cast<float>(kSize - 1);
        return std::clamp(static_cast<int>(fy), 0, kSize - 1);
    };

    int c0 = to_col(x0);
    int c1 = to_col(x1);
    int r0 = to_row(y0);
    int r1 = to_row(y1);

    for (int r = r0; r <= r1; ++r)
        for (int c = c0; c <= c1; ++c)
            cells[r][c] = val;
}

// ─────────────────────────────────────────────────────────────────────────────
//  BamProcessor
// ─────────────────────────────────────────────────────────────────────────────

BamProcessor::BamProcessor()
{
    rebuild_cache();
}

void BamProcessor::rebuild_cache()
{
    // Start from the BAM grid values
    for (int r = 0; r < BamGrid::kSize; ++r)
        for (int c = 0; c < BamGrid::kSize; ++c)
            cache_[r][c] = static_cast<float>(grid_.cells[r][c]) / 255.f;

    // Rasterize each enabled safety zone (takes the minimum — most restrictive wins)
    for (const SafetyZone& z : zones_) {
        if (z.enabled && !z.polygon.empty())
            rasterize_zone(z);
    }
}

void BamProcessor::apply(PointBuffer& buf) const
{
    if (!enabled)
        return;

    for (LaserPoint& pt : buf) {
        // Only process lit points
        if (pt.blanked)
            continue;

        // A point with all-zero colour is effectively off; skip it
        if (pt.r == 0 && pt.g == 0 && pt.b == 0)
            continue;

        float nx = pt.nx();
        float ny = pt.ny();

        // Map to cell
        float fx = (nx + 1.f) * 0.5f * static_cast<float>(BamGrid::kSize - 1);
        float fy = (ny + 1.f) * 0.5f * static_cast<float>(BamGrid::kSize - 1);
        int col = std::clamp(static_cast<int>(fx), 0, BamGrid::kSize - 1);
        int row = std::clamp(static_cast<int>(fy), 0, BamGrid::kSize - 1);

        float atten = cache_[row][col];

        if (atten < 0.01f) {
            // Full block — blank the point
            pt.blanked = true;
        } else if (atten < 0.999f) {
            // Partial attenuation — dim colours
            pt.r = static_cast<uint8_t>(static_cast<float>(pt.r) * atten);
            pt.g = static_cast<uint8_t>(static_cast<float>(pt.g) * atten);
            pt.b = static_cast<uint8_t>(static_cast<float>(pt.b) * atten);
        }
        // atten >= 0.999: full pass, no modification
    }
}

bool BamProcessor::point_in_polygon(float x, float y,
                                     const std::vector<std::pair<float, float>>& poly) const
{
    // Ray casting algorithm: cast a ray in the +x direction from (x,y)
    // and count edge crossings. Odd count = inside.
    int n = static_cast<int>(poly.size());
    if (n < 3)
        return false;

    bool inside = false;
    int j = n - 1;

    for (int i = 0; i < n; ++i) {
        float xi = poly[static_cast<size_t>(i)].first;
        float yi = poly[static_cast<size_t>(i)].second;
        float xj = poly[static_cast<size_t>(j)].first;
        float yj = poly[static_cast<size_t>(j)].second;

        // Check if the ray from (x,y) going right crosses edge (xi,yi)-(xj,yj)
        bool straddles_y = ((yi > y) != (yj > y));
        if (straddles_y) {
            // x coordinate of edge intersection with horizontal line y
            float x_intersect = (xj - xi) * (y - yi) / (yj - yi) + xi;
            if (x < x_intersect)
                inside = !inside;
        }

        j = i;
    }

    return inside;
}

void BamProcessor::rasterize_zone(const SafetyZone& z)
{
    float zone_atten = std::clamp(z.attenuation, 0.f, 1.f);

    // Walk every cell and test whether its centre lies inside the polygon
    for (int r = 0; r < BamGrid::kSize; ++r) {
        for (int c = 0; c < BamGrid::kSize; ++c) {
            // Cell centre in normalized coordinates
            float cx = (static_cast<float>(c) / static_cast<float>(BamGrid::kSize - 1)) * 2.f - 1.f;
            float cy = (static_cast<float>(r) / static_cast<float>(BamGrid::kSize - 1)) * 2.f - 1.f;

            if (point_in_polygon(cx, cy, z.polygon)) {
                // Most restrictive wins: take minimum attenuation
                if (zone_atten < cache_[r][c])
                    cache_[r][c] = zone_atten;
            }
        }
    }
}

} // namespace idhmfis
