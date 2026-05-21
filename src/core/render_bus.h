#pragma once
// Render Bus — owns the canonical point stream and fans it out to:
//   1. DacOutputThread  (real-time priority)
//   2. NdiRasterizer    (GPU, 60+ fps)
// The render bus is the hot path. No heap allocation. No locks.
// PointBuffers are double-buffered; consumers read the committed buffer.

#include "types.h"
#include <atomic>
#include <array>
#include <functional>
#include <memory>

namespace idhmfis {

class RenderBus {
public:
    RenderBus();
    ~RenderBus();

    // Called by show engine (single producer) to submit a rendered frame.
    // Non-blocking. Drops frame if consumer is too slow (counted).
    void submit(RenderFrame frame);

    // Called by DAC output thread to pull the next frame.
    // Returns nullptr if nothing available.
    const RenderFrame* poll_dac();

    // Called by NDI rasterizer to pull the latest frame (not every frame matters;
    // NDI gets the most recent one — it may skip frames to stay real-time).
    const RenderFrame* poll_ndi_latest();

    // Release a frame back to the bus after consumption.
    void release_dac(const RenderFrame* f);
    void release_ndi(const RenderFrame* f);

    // Statistics
    uint64_t frames_submitted()  const { return stats_.submitted.load(); }
    uint64_t frames_dropped_dac()const { return stats_.dropped_dac.load(); }
    uint64_t frames_dropped_ndi()const { return stats_.dropped_ndi.load(); }
    void reset_stats();

private:
    static constexpr int kPoolSize = 8;

    struct FramePool {
        std::array<RenderFrame, kPoolSize> frames;
        std::array<std::atomic_bool, kPoolSize> in_use{};
        int next_free = 0;

        RenderFrame* acquire();
        void release(const RenderFrame* f);
    };

    FramePool            pool_;
    // Both DAC and NDI use "latest frame" atomic swap — zero queued latency.
    std::atomic<RenderFrame*> dac_latest_{nullptr};
    std::atomic<RenderFrame*> ndi_latest_{nullptr};
    RenderFrame*              ndi_prev_{nullptr};

    struct Stats {
        std::atomic<uint64_t> submitted{0};
        std::atomic<uint64_t> dropped_dac{0};
        std::atomic<uint64_t> dropped_ndi{0};
    } stats_;
};

} // namespace idhmfis
