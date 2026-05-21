#pragma once
// Scan Fail Monitor — detects when galvanometer mirrors have stopped scanning.
// A stationary laser beam at full power can cause immediate eye and skin damage.
// IEC 60825-1 and EN 60825-1 require scan-fail protection for Class 3B/4 lasers.

#include <cmath>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  ScanFailMonitor
//  Called every engine tick with the current output point coordinates.
//  Triggers (and latches) when the beam position has not moved enough for
//  max_still_ms milliseconds.
// ─────────────────────────────────────────────────────────────────────────────
class ScanFailMonitor {
public:
    struct Config {
        float max_still_ms = 100.f;  // kill beam if position unchanged for this long
        float min_velocity = 0.01f;  // minimum movement per ms to consider "scanning"
        bool  enabled      = true;
    };

    void configure(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }

    // Call each engine tick with current output point (after optimizer).
    // x, y: normalized coordinates -1..1.
    // dt_ms: elapsed time in milliseconds since last tick.
    // Returns true if scan fail is detected (beam must be killed).
    bool tick(float x, float y, float dt_ms);

    // Reset state (called on engine stop/reset or after interlock is cleared).
    void reset()
    {
        still_ms_  = 0.f;
        last_x_    = 0.f;
        last_y_    = 0.f;
        triggered_ = false;
    }

    // Reset only the still-time accumulator without clearing the trigger latch.
    // Use when the laser is known to be off so no spurious still-time accumulates.
    void reset_still() { still_ms_ = 0.f; last_x_ = 0.f; last_y_ = 0.f; }

    bool is_triggered() const { return triggered_; }

private:
    Config cfg_;
    float last_x_    = 0.f;
    float last_y_    = 0.f;
    float still_ms_  = 0.f;
    bool  triggered_ = false;
};

} // namespace idhmfis
