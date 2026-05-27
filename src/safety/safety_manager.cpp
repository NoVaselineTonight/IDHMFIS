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
    //
    // SAFETY INVARIANT: if the scan-fail latch has fired we ALWAYS blank output,
    // even when the monitor is administratively disabled.  Disabling the monitor
    // does not resolve the physical stall condition; the operator must explicitly
    // acknowledge the fault via ResetScanFail (which calls scan_fail.reset()).
    // Silently auto-clearing a latched fault when the user flips the enable
    // toggle would allow full-power output into a potentially stalled beam.
    if (scan_fail.is_triggered()) {
        blank_all(buf);
        return;
    }

    if (scan_fail.config().enabled) {
        // Compute the centroid of all lit points to use as the position sample.
        // Using only the first lit point causes false-positive triggering when a
        // static anchor point (e.g. text baseline) happens to be the first in the
        // buffer while the rest of the frame is actively scanning.  The centroid
        // moves whenever any part of the frame geometry changes, so it gives a
        // more representative read of scanner motion without compromising real
        // stall detection: a physically stalled beam produces zero centroid
        // displacement regardless of which points are in the frame.
        float sum_x  = 0.f;
        float sum_y  = 0.f;
        int   n_lit  = 0;

        for (const LaserPoint& pt : buf) {
            if (!pt.blanked) {
                sum_x += pt.nx();
                sum_y += pt.ny();
                ++n_lit;
            }
        }

        if (n_lit > 0) {
            float sample_x = sum_x / static_cast<float>(n_lit);
            float sample_y = sum_y / static_cast<float>(n_lit);
            if (scan_fail.tick(sample_x, sample_y, dt_ms)) {
                blank_all(buf);
                return;
            }
        } else {
            // Laser is off and not triggered — reset the still-counter so dark
            // frames don't cause spurious accumulation.
            scan_fail.reset_still();
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
