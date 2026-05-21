// safety_manager.cpp — Safety manager implementation.

#include "safety_manager.h"

namespace idhmfis {

void SafetyManager::blank_all(PointBuffer& buf)
{
    for (LaserPoint& pt : buf)
        pt.blanked = true;
}

void SafetyManager::apply(PointBuffer& buf, float dt_ms)
{
    // Priority 1: hardware interlock (e-stop, key switch)
    if (interlock_open) {
        blank_all(buf);
        return;
    }

    // Priority 2: scan fail (stalled mirror)
    // Only tick when there are lit points — if output is empty or all blanked the
    // laser is physically off and there is no scan-fail risk.  Without this guard,
    // the monitor accumulates still-time on every blank frame and latches 100 ms
    // after startup even when no content is playing.
    if (!scan_fail.config().enabled) {
        // Scan-fail disabled by operator (e.g. NDI-only / no-laser setup).
        // Clear any existing latch so output is not permanently blocked.
        if (scan_fail.is_triggered()) scan_fail.reset();
    } else {
        float sample_x = 0.f;
        float sample_y = 0.f;
        bool  has_lit  = false;

        for (const LaserPoint& pt : buf) {
            if (!pt.blanked) {
                sample_x = pt.nx();
                sample_y = pt.ny();
                has_lit  = true;
                break;
            }
        }

        if (has_lit) {
            if (scan_fail.tick(sample_x, sample_y, dt_ms)) {
                blank_all(buf);
                return;
            }
        } else if (!scan_fail.is_triggered()) {
            // Laser is off and not triggered — reset the still-counter so dark
            // frames don't cause spurious accumulation.
            scan_fail.reset_still();
        } else {
            // Scan-fail was triggered while laser was live.  Latch stays set
            // until the operator explicitly resets via the Safety panel.
            blank_all(buf);
            return;
        }
    }

    // Priority 3: Beam Attenuation Map
    if (bam.enabled)
        bam.apply(buf);
}

bool SafetyManager::is_safe() const
{
    if (interlock_open)      return false;
    if (scan_fail.is_triggered()) return false;
    return true;
}

std::string SafetyManager::status_string() const
{
    // Report most severe condition first
    if (interlock_open)           return "INTERLOCK";
    if (scan_fail.is_triggered()) return "SCAN FAIL";
    if (bam.enabled) {
        // Check whether any cell is actually attenuating
        const BamGrid& g = bam.grid();
        for (int r = 0; r < BamGrid::kSize; ++r)
            for (int c = 0; c < BamGrid::kSize; ++c)
                if (g.cells[r][c] < 255)
                    return "BAM ACTIVE";
    }
    return "OK";
}

} // namespace idhmfis
