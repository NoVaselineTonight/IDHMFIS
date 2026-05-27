#pragma once
// Scan Fail Monitor — detects when galvanometer mirrors have stopped scanning.
// A stationary laser beam at full power can cause immediate eye and skin damage.
// IEC 60825-1 and EN 60825-1 require scan-fail protection for Class 3B/4 lasers.

#include <atomic>
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

    // Operator-acknowledged full reset — clears the trigger latch AND the
    // still-time accumulator.  MUST only be called in response to an explicit
    // operator action (e.g. the ResetScanFail command from the Safety panel).
    // Do NOT call this automatically on disable or on engine restart while
    // laser hardware may still be connected: a latched fault means the scanner
    // physically stalled and requires human inspection before resuming output.
    // BUG #7 / #63 fix: triggered_ and still_ms_ are written here (UI thread)
    // and read/written in tick() (engine thread); use atomic stores for safety.
    // last_x_/last_y_ are intentionally NOT reset here — see reset_still() note.
    void reset()
    {
        still_ms_.store(0.f, std::memory_order_seq_cst);
        last_x_    = 0.f;
        last_y_    = 0.f;
        triggered_.store(false, std::memory_order_seq_cst);
    }

    // Reset only the still-time accumulator without clearing the trigger latch.
    // Use when the laser is known to be off so no spurious still-time accumulates.
    // Never clears triggered_ — latch removal requires an explicit operator reset().
    // NOTE: last_x_/last_y_ are intentionally NOT reset here. Zeroing them would
    // cause a false large velocity spike on the next tick (position jumps from
    // wherever the beam is to 0,0), potentially masking a real stall. H-14 fix.
    void reset_still() { still_ms_.store(0.f, std::memory_order_seq_cst); }

    // BUG #7 fix: use seq_cst load so the engine thread's trigger write is
    // always visible to the UI thread that calls is_triggered().
    bool is_triggered() const { return triggered_.load(std::memory_order_seq_cst); }

private:
    Config cfg_;
    // last_x_ / last_y_: only written from tick() (engine thread); plain float is fine.
    float last_x_    = 0.f;
    float last_y_    = 0.f;
    // BUG #7 / #63 fix: still_ms_ and triggered_ are written by tick() (engine thread)
    // and reset by reset()/reset_still() (UI thread) — must be atomic.
    std::atomic<float> still_ms_{0.f};
    std::atomic<bool>  triggered_{false};
};

} // namespace idhmfis
