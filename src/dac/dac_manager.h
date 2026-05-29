#pragma once
// DacManager — hot-plug scanning and real-time output orchestration.
//
// Responsibilities:
//   1. Periodic background scan for newly connected / disconnected DACs
//   2. Automatic selection of the active DAC (priority: Helios > EtherDream
//      > LaserDock > IDN)
//   3. Real-time output thread running at TIME_CRITICAL priority
//   4. Latency measurement (frame available → first point out)
//   5. Idle keep-alive: re-send last frame when no new frame arrives
//   6. CITP/CAEX sidecar — always open alongside the primary DAC so Capture
//      receives a laser feed regardless of which hardware is active.
//   7. IDN-Stream broadcast sidecar — mirrors frames to UDP 255.255.255.255:7255
//      so IDN-capable receivers (IDN-Toolbox, OpenIDN) can visualise the feed.
//   8. Virtual DirectShow camera — registers as "IDHMFIS Laser Preview" in the
//      Windows Video Capture Sources category for Capture 2024 video input.

#include "idac.h"
#include "citp_caex_dac.h"
#include "idn_stream_sidecar.h"
#include "common_laser_stream.h"
#include "capture/virtual_camera.h"
#include "core/render_bus.h"
#include "core/timer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace idhmfis {

// Key identifying a DacManager by its hardware binding.
// Used by reconcile_laser_managers() to detect reusable vs. stale managers.
struct DacHardwareKey {
    std::string dac_type;
    std::string dac_address;
    int         ordinal;
    bool operator==(const DacHardwareKey& o) const {
        return dac_type == o.dac_type && dac_address == o.dac_address && ordinal == o.ordinal;
    }
};

class DacManager {
public:
    explicit DacManager(RenderBus& bus, int stream_idx = 0);
    ~DacManager();

    // Start background scan thread and real-time output thread.
    void start();

    // Stop both threads and close the active DAC.
    void stop();

    // Snapshot of all known DAC statuses (including disconnected candidates).
    std::vector<DacStatus> all_status() const;

    // Currently active DAC pointer (nullptr if none connected).
    // Caller must NOT cache this beyond one output cycle.
    IDac* active_dac() const;

    // True if both background threads are running (set by start(), cleared by stop()).
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    // True if the output thread has ticked within the last timeout_ms milliseconds.
    // Returns true while the manager is still starting (last_loop_ms_ == 0) to avoid
    // false positives during the probe/connect phase.
    bool is_healthy(int64_t timeout_ms = 3000) const;

    // True if this manager has been started but the output thread hasn't yet
    // completed its first loop iteration AND started_at_ms_ is < 30 s ago.
    // Distinguishes a freshly-started manager from a zombie (stop_requested_ set
    // but running_ still false) so the health checker won't false-positive on startup.
    bool is_starting() const {
        // Output thread has ticked — fully running, not starting.
        if (last_loop_ms_.load(std::memory_order_relaxed) != 0) return false;
        // start() has never been called.
        int64_t started = started_at_ms_.load(std::memory_order_relaxed);
        if (started == 0) return false;
        // start() was called recently.  running_ may still be false in the gap
        // between start() returning and the output thread executing its first
        // iteration — this is intentionally checked AFTER last_loop_ms_ so a
        // fully-running manager is never mistaken for "still starting".
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return (now_ms - started) < 30000;
    }

    // Clear the stop_requested_ flag so a restarted manager can call start() again.
    // Only call this when you own the last reference and intend to re-use the object.
    void clear_stop_requested() { stop_requested_.store(false, std::memory_order_release); }

    // Bus slot index this manager was constructed for (0 = main bus,
    // 1 = extra_laser_buses_[0], etc.).  Exposed so the engine can detect
    // stale bus assignments when output streams are reordered.
    int ordinal() const { return stream_idx_; }

    // Change the output point rate across all candidates.
    void set_point_rate(int pps);

    // Force a specific DAC type / address (bypasses auto-selection).
    // type: "helios", "helios_emulated", "etherdream", "idn", "laserdock"
    // address: IP address for EtherDream / IDN, ignored for USB
    void force_dac(const std::string& type, const std::string& address = "");

    // Latency statistics
    double last_mean_latency_ms() const;
    double last_p99_latency_ms()  const;

    // CITP/CAEX stream identity — source name broadcast via PLoc beacons.
    // Returns e.g. "IDHMFIS" for stream 0, "IDHMFIS-2" for stream 2.
    const std::string& citp_stream_name() const { return citp_caex_.source_name(); }
    uint16_t           citp_tcp_port()    const { return citp_caex_.tcp_port(); }

    // IDN-Stream sidecar master enable — when false, no IDN frames are broadcast.
    // Thread-safe: written from UI thread, read from output thread.
    void set_idn_sidecar_enabled(bool en) { idn_sidecar_enabled_.store(en, std::memory_order_relaxed); }

private:
    // Thread entry points
    void scan_thread_fn();
    void output_thread_fn();

    // Attempt to open the best available DAC among candidates_.
    // Called from scan_thread_fn() whenever a candidate is disconnected.
    void promote_best_candidate();

    // Build the initial candidate list (called once from start()).
    void build_candidates();

    // Probe network for EtherDream devices.
    void probe_etherdream();

    RenderBus& bus_;
    int                            stream_idx_{ 0 };

    mutable std::mutex            candidates_mutex_;
    std::vector<std::unique_ptr<IDac>> candidates_;

    // Active DAC lifetime management (BUG #6):
    //   active_sp_ is a shared_ptr that extends DAC object lifetime while the
    //   output thread holds a local copy.  Written only under candidates_mutex_.
    //   active_ is a fast-path atomic hint; the output thread must obtain
    //   active_sp_ under candidates_mutex_ to get a safe, reference-counted handle.
    std::shared_ptr<IDac>         active_sp_;         // guarded by candidates_mutex_
    std::atomic<IDac*>            active_{ nullptr };  // fast-path hint (not safe alone)

    std::atomic<bool>             running_{ false };
    // Set by stop() (under lifecycle_mutex_) before returning.
    // Checked by start() (under lifecycle_mutex_) to prevent thread creation when
    // stop() already ran — guards the window where a stop thread deletes the manager
    // while a start thread has been launched but not yet entered start().
    std::atomic<bool>             stop_requested_{ false };
    // Serialises start() against stop(): start() holds this for its entire body
    // so stop() cannot proceed (and the manager cannot be deleted) until start()
    // has finished creating scan_thread_ and output_thread_.
    std::mutex                    lifecycle_mutex_;
    std::thread                   scan_thread_;
    std::thread                   output_thread_;

    int                           point_rate_  = kDefaultPointRate;

    // Latency ring buffer (128 samples)
    mutable std::mutex            stats_mutex_;
    RollingStats<128>             latency_;

    // Last known good frame — replayed when buffer is hungry (keep-alive)
    PointBuffer                   last_frame_;
    mutable std::mutex            last_frame_mutex_;

    // Heartbeat: output_thread_fn() writes the current epoch-ms at the top of
    // every loop iteration.  is_healthy() reads it to detect a stuck thread.
    std::atomic<int64_t>          last_loop_ms_{ 0 };

    // start() epoch-ms stamp: set once when start() begins creating threads.
    // is_healthy() uses this to grant a grace period (3× timeout_ms) while the
    // output thread hasn't yet executed its first iteration — the window between
    // start() returning and last_loop_ms_ first being written by the output thread.
    std::atomic<int64_t>          started_at_ms_{ 0 };

    // Forced-DAC override
    std::string                   forced_type_;
    std::string                   forced_address_;
    std::atomic<bool>             force_flag_{ false };

    // CITP/CAEX sidecar — opened unconditionally so Capture always sees a feed.
    // Lives outside candidates_ so it is never promoted as the primary DAC.
    CitpCaexDac                   citp_caex_;

    // IDN-Stream broadcast sidecar — mirrors every frame to UDP 255.255.255.255:7255
    // so IDN-capable receivers (IDN-Toolbox, OpenIDN hardware) can visualise the feed.
    // Opened unconditionally in start(); zero-cost when no receiver is listening.
    IdnStreamSidecar              idn_sidecar_;
    std::atomic<bool>             idn_sidecar_enabled_{ true };

    // CommonLaserStream sidecar — TCP server port 7256 + UDP broadcast (CLS1 protocol).
    // Mirrors every frame to open-source and custom laser tools (LaserBoy, OpenLase, etc.).
    // Opened unconditionally in start(); zero-cost when no client is connected.
    CommonLaserStream             cls_sidecar_;

    // Virtual DirectShow camera — registers as a Windows video capture device
    // named "IDHMFIS Laser Preview" so Capture 2024 can open it as a video input.
    // Opened unconditionally in start(); non-fatal if it fails on non-Windows.
    VirtualCamera                 virtual_camera_;
};

} // namespace idhmfis
