#pragma once
// ZoneManager — owns the list of output zones and performs per-zone routing.
// Called from the engine thread each frame; all methods are non-threadsafe.

#include "zone_types.h"
#include "../core/types.h"

#include <vector>
#include <unordered_map>

namespace idhmfis {

class ZoneManager {
public:
    ZoneManager();

    // ── Zone CRUD ────────────────────────────────────────────────────────────
    int   add_zone();                        // returns new zone id
    void  remove_zone(int id);
    Zone* get_zone(int id);
    const Zone* get_zone(int id) const;
    std::vector<Zone>&       zones();
    const std::vector<Zone>& zones() const;

    // ── Transform / colour helpers (used internally and by preview) ──────────

    // Apply geometric transform to a PointBuffer.
    // Operations applied in order: scale, shear, rotate, translate.
    // If use_keystone: bilinear corner-warp added on top.
    PointBuffer apply_transform(const PointBuffer& src, const ZoneTransform& t) const;

    // Apply per-zone colour correction and intensity scaling in-place.
    void apply_color(PointBuffer& buf, float r, float g, float b, float intensity) const;

    // ── Test pattern generator ───────────────────────────────────────────────
    // Returns a PointBuffer representing the requested test shape.
    // 0=circle  1=crosshair  2=box  3=star  4=scanlines
    PointBuffer test_pattern(int pattern_type, int point_count = 512) const;

    // ── Per-tick routing ─────────────────────────────────────────────────────
    // Transforms master PointBuffer into per-zone buffers ready for DAC output.
    // Zones that are disabled or blind are excluded.
    // If any zone has solo=true, only solo zones are included.
    // Returns: zone_id -> transformed PointBuffer.
    std::unordered_map<int, PointBuffer> route(const PointBuffer& master) const;

private:
    std::vector<Zone> zones_;
    int next_id_ = 1;
};

} // namespace idhmfis
