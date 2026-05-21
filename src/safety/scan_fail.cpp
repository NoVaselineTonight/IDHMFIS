// scan_fail.cpp — Scan fail monitor implementation.
// Safety-critical: must never fail to detect a stalled beam.

#include "scan_fail.h"
#include <cmath>

namespace idhmfis {

bool ScanFailMonitor::tick(float x, float y, float dt_ms)
{
    if (!cfg_.enabled)
        return false;

    // Once triggered, stay triggered until reset() is called.
    // The interlock must be cleared by an operator, not automatically.
    if (triggered_)
        return true;

    // Guard against pathological dt values (e.g. first tick, debugger pause)
    if (dt_ms <= 0.f || dt_ms > 1000.f) {
        last_x_ = x;
        last_y_ = y;
        return false;
    }

    float dx = x - last_x_;
    float dy = y - last_y_;

    // Velocity in normalized units per millisecond
    float dist = std::sqrt(dx * dx + dy * dy);
    float velocity = dist / dt_ms;

    if (velocity < cfg_.min_velocity) {
        // Beam is effectively still
        still_ms_ += dt_ms;

        if (still_ms_ >= cfg_.max_still_ms) {
            triggered_ = true;
            return true;
        }
    } else {
        // Beam is moving — reset the still counter
        still_ms_ = 0.f;
    }

    last_x_ = x;
    last_y_ = y;
    return false;
}

} // namespace idhmfis
