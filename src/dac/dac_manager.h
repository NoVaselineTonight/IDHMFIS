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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace idhmfis {

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

    // Active DAC is a raw pointer into candidates_ (never owns memory).
    // Written only by scan_thread; read by output_thread.
    std::atomic<IDac*>            active_{ nullptr };

    std::atomic<bool>             running_{ false };
    std::thread                   scan_thread_;
    std::thread                   output_thread_;

    int                           point_rate_  = kDefaultPointRate;

    // Latency ring buffer (128 samples)
    mutable std::mutex            stats_mutex_;
    RollingStats<128>             latency_;

    // Last known good frame — replayed when buffer is hungry (keep-alive)
    PointBuffer                   last_frame_;
    mutable std::mutex            last_frame_mutex_;

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
