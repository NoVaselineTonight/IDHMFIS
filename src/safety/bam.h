#pragma once
// Beam Attenuation Map (BAM) — legal requirement for audience-scanning laser shows.
// 64x64 grid superior to Beyond's 32x32. Each cell is 0..255 (0=full block, 255=full pass).

#include "../core/types.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  BamGrid — 64x64 attenuation grid
//  Covers the normalized output space (-1..1 in both axes).
//  cells[row][col]: row=y axis, col=x axis.
// ─────────────────────────────────────────────────────────────────────────────
struct BamGrid {
    static constexpr int kSize = 64;
    uint8_t cells[kSize][kSize];  // [row][col]

    BamGrid() { memset(cells, 255, sizeof(cells)); }

    // Sample attenuation at normalized position x,y (-1..1).
    // Returns 0..1 where 0=full block, 1=full pass.
    float sample(float x, float y) const;

    // Set a rectangular region to the given attenuation value (0..255).
    // Coordinates are normalized -1..1; clamped to grid bounds.
    void set_rect(float x0, float y0, float x1, float y1, uint8_t val);

    // Clear to full pass (all cells = 255)
    void clear() { memset(cells, 255, sizeof(cells)); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  SafetyZone — polygon exclusion zone rasterized into the attenuation cache
// ─────────────────────────────────────────────────────────────────────────────
struct SafetyZone {
    std::string name;
    bool enabled = true;
    std::vector<std::pair<float, float>> polygon;  // normalized -1..1 vertices
    float attenuation = 0.f;  // 0=full block, 1=full pass
};

// ─────────────────────────────────────────────────────────────────────────────
//  BamProcessor — applies the BAM grid and safety zones to a PointBuffer
// ─────────────────────────────────────────────────────────────────────────────
class BamProcessor {
public:
    BamProcessor();

    BamGrid& grid() { return grid_; }
    const BamGrid& grid() const { return grid_; }

    std::vector<SafetyZone>& safety_zones() { return zones_; }
    const std::vector<SafetyZone>& safety_zones() const { return zones_; }

    // Apply BAM + safety zones to a PointBuffer.
    // Points in fully blocked cells (cache < 0.01) are blanked (blanked=true).
    // Points in attenuated cells are dimmed proportionally.
    void apply(PointBuffer& buf) const;

    // Rebuild the effective attenuation cache from grid + rasterized safety polygons.
    // Must be called whenever grid or zones change.
    void rebuild_cache();

    // BUG #30 fix: atomic so UI-thread writes and engine-thread reads are race-free.
    std::atomic<bool> enabled{true};

private:
    BamGrid grid_;
    std::vector<SafetyZone> zones_;
    // Cached effective attenuation per cell (combined grid + rasterized zones).
    // H-13: cache_ is written by rebuild_cache() on the UI thread and read by
    // apply() on the engine thread — protect with a mutex.
    mutable std::mutex cache_mtx_;
    float cache_[BamGrid::kSize][BamGrid::kSize]{};

    // Point-in-polygon test using ray casting algorithm.
    bool point_in_polygon(float x, float y,
                          const std::vector<std::pair<float, float>>& poly) const;

    // BUG #31 fix: DEAD CODE — do NOT call directly.
    // rasterize_zone() historically wrote to cache_ without holding cache_mtx_,
    // creating a data race with apply(). It is now a no-op. Use rebuild_cache().
    void rasterize_zone(const SafetyZone& z);
};

} // namespace idhmfis
