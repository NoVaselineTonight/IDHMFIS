#pragma once
// Point rate negotiation utilities.
//
// The negotiator bridges the show engine's desired point rate with the
// hardware's actual capabilities, and computes the correct number of
// points to generate per video frame.
//
// Math:
//   points_per_frame = round(point_rate / fps)
//
//   For 30,000 pps @ 60 fps: 500 points/frame
//   For 30,000 pps @ 30 fps: 1000 points/frame
//
// Negotiation rules (in priority order):
//   1. If desired is in [dac_min, dac_max], use it directly.
//   2. If desired < dac_min, use dac_min.
//   3. If desired > dac_max, use dac_max.
//   4. Optionally clamp to the project-global [kMinPointRate, kMaxPointRate].

#include "core/types.h"
#include <algorithm>
#include <cmath>

namespace idhmfis {

class PointRateNegotiator {
public:
    // ─────────────────────────────────────────────────────────────────────────
    //  negotiate — choose the best achievable point rate
    //
    //  Parameters:
    //    desired_pps        — what the show engine wants
    //    dac_min / dac_max  — hardware capability range
    //    frame_point_count  — how many points the generator will produce per
    //                         video frame (used for a sanity check only)
    //    output_fps         — video frame rate (for the sanity check)
    //
    //  Returns the clamped / negotiated point rate in pps.
    // ─────────────────────────────────────────────────────────────────────────
    static int negotiate(int desired_pps,
                         int dac_min,
                         int dac_max,
                         int frame_point_count,
                         int output_fps) {
        // Clamp to hardware limits
        int negotiated = std::clamp(desired_pps, dac_min, dac_max);

        // Clamp to project-global safety limits
        negotiated = std::clamp(negotiated, kMinPointRate, kMaxPointRate);

        // Sanity: if the negotiated rate would make each video frame carry
        // more than 8192 points (a pathological configuration), warn by
        // capping.  This prevents the DAC from being asked to consume
        // more data than it can buffer in one frame period.
        if (output_fps > 0 && frame_point_count > 0) {
            static constexpr int kMaxPointsPerFrame = 8192;
            int max_safe_rate = kMaxPointsPerFrame * output_fps;
            if (negotiated > max_safe_rate)
                negotiated = max_safe_rate;
        }

        return negotiated;
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  points_per_frame — how many laser points fit in one video frame
    //
    //  Returns the nearest integer ≥ 1.
    //  Example: 30000 pps / 60 fps = 500 pts/frame
    // ─────────────────────────────────────────────────────────────────────────
    static int points_per_frame(int point_rate, int fps) {
        if (fps <= 0 || point_rate <= 0) return 1;
        return std::max(1, static_cast<int>(
            std::round(static_cast<double>(point_rate) /
                       static_cast<double>(fps))));
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  frame_period_us — microseconds between video frames
    // ─────────────────────────────────────────────────────────────────────────
    static int frame_period_us(int fps) {
        if (fps <= 0) return 16667;
        return static_cast<int>(1'000'000 / fps);
    }

    // ─────────────────────────────────────────────────────────────────────────
    //  point_period_ns — nanoseconds between consecutive DAC output points
    // ─────────────────────────────────────────────────────────────────────────
    static int64_t point_period_ns(int point_rate) {
        if (point_rate <= 0) return 33333LL;
        return static_cast<int64_t>(1'000'000'000LL / point_rate);
    }
};

} // namespace idhmfis
