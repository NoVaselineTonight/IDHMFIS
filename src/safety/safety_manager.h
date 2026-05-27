#pragma once
// SafetyManager — aggregates all safety subsystems (BAM, scan-fail, interlock).
// The apply() method is the single point of truth for making a frame safe to output.

#include "bam.h"
#include "scan_fail.h"
#include <atomic>
#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  SafetyManager
// ─────────────────────────────────────────────────────────────────────────────
class SafetyManager {
public:
    BamProcessor    bam;
    ScanFailMonitor scan_fail;

    // Hardware interlock (key switch / e-stop).
    // true = interlock open = e-stop triggered = output must be blanked.
    // BUG #7 fix: atomic so UI-thread writes and engine-thread reads are race-free.
    std::atomic<bool> interlock_open{false};

    // Apply all safety systems to a PointBuffer.
    // If interlock_open or scan_fail is triggered, ALL points are blanked.
    // Otherwise BAM attenuation is applied.
    // dt_ms: elapsed time in milliseconds (forwarded to scan_fail).
    void apply(PointBuffer& buf, float dt_ms);

    // Returns true if all safety systems are in a safe (pass-through) state.
    bool is_safe() const;

    // Human-readable status for the UI indicator.
    // Returns "OK", "BAM ACTIVE", "SCAN FAIL", or "INTERLOCK".
    // When multiple conditions are active, the most severe takes priority.
    std::string status_string() const;

private:
    // Blank every point in the buffer (used for interlock / scan-fail kill).
    static void blank_all(PointBuffer& buf);
};

} // namespace idhmfis
