#include "render_bus.h"
#include <cstring>

namespace idhmfis {

RenderBus::RenderBus() {
    for (auto& b : pool_.in_use) b.store(false, std::memory_order_relaxed);
}

RenderBus::~RenderBus() = default;

RenderFrame* RenderBus::FramePool::acquire() {
    for (int j = 0; j < kPoolSize; ++j) {
        int i = (next_free + j) % kPoolSize;
        bool expected = false;
        if (in_use[i].compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            next_free = (i + 1) % kPoolSize;
            return &frames[i];
        }
    }
    return nullptr; // pool exhausted → caller must drop
}

void RenderBus::FramePool::release(const RenderFrame* f) {
    for (int i = 0; i < kPoolSize; ++i) {
        if (&frames[i] == f) {
            in_use[i].store(false, std::memory_order_release);
            return;
        }
    }
}

void RenderBus::submit(RenderFrame frame) {
    stats_.submitted.fetch_add(1, std::memory_order_relaxed);

    // --- DAC path: keep only the latest frame (atomic swap, zero queued latency) ---
    // A FIFO queue accumulates frames when the DAC runs slower than the engine
    // (e.g. 60 Hz DAC vs 1 kHz engine), producing 100+ ms of render latency.
    // Swapping to latest-wins matches the NDI strategy and keeps output fresh.
    RenderFrame* dac_slot = pool_.acquire();
    if (dac_slot) {
        *dac_slot = frame;  // copy — NDI path will move the same frame below
        RenderFrame* old = dac_latest_.exchange(dac_slot, std::memory_order_acq_rel);
        if (old) {
            pool_.release(old);
            stats_.dropped_dac.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        stats_.dropped_dac.fetch_add(1, std::memory_order_relaxed);
    }

    // --- NDI path: keep only the latest ---
    RenderFrame* ndi_slot = pool_.acquire();
    if (ndi_slot) {
        *ndi_slot = std::move(frame);
        RenderFrame* old = ndi_latest_.exchange(ndi_slot, std::memory_order_acq_rel);
        if (old) {
            pool_.release(old);
            stats_.dropped_ndi.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        stats_.dropped_ndi.fetch_add(1, std::memory_order_relaxed);
    }
}

const RenderFrame* RenderBus::poll_dac() {
    return dac_latest_.exchange(nullptr, std::memory_order_acq_rel);
}

const RenderFrame* RenderBus::poll_ndi_latest() {
    return ndi_latest_.exchange(nullptr, std::memory_order_acq_rel);
}

void RenderBus::release_dac(const RenderFrame* f) { pool_.release(f); }
void RenderBus::release_ndi(const RenderFrame* f) { pool_.release(f); }

void RenderBus::reset_stats() {
    stats_.submitted.store(0);
    stats_.dropped_dac.store(0);
    stats_.dropped_ndi.store(0);
}

} // namespace idhmfis
