// dac_manager.cpp
// Hot-plug scanning + real-time output thread.

#include "dac_manager.h"
#include "helios_dac.h"
#include "etherdream_dac.h"
#include "idn_sender.h"
#include "laserdock_dac.h"
#include "citp_caex_dac.h"
#include "idn_stream_sidecar.h"
#include "common_laser_stream.h"
#include "point_rate_negotiator.h"
#include "core/logger.h"
#include "core/thread_utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Scan interval and output timing
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int kScanIntervalMs    = 2000; // hot-plug poll every 2 s
static constexpr int kKeepaliveMs       = 33;   // re-send frame if idle > 33 ms
static constexpr int kOutputSleepUs     = 100;  // tight-poll sleep when no frame

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
DacManager::DacManager(RenderBus& bus, int stream_idx)
    : bus_(bus)
    , stream_idx_(stream_idx)
    , citp_caex_(stream_idx)
{}

DacManager::~DacManager() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::start() {
    if (running_.exchange(true)) return; // already running

    build_candidates();

    // Open CITP/CAEX sidecar unconditionally — Capture needs it running even
    // when a physical DAC is the active primary output.
    (void)citp_caex_.set_point_rate(point_rate_);
    if (!citp_caex_.open()) {
        log::warn("DacManager: CITP/CAEX sidecar failed to open (non-fatal)");
    }

    // Open IDN-Stream broadcast sidecar unconditionally so IDN-capable
    // receivers (IDN-Toolbox, OpenIDN hardware) discover this source at startup.
    // The sidecar broadcasts to 255.255.255.255:7255; it is zero-cost when no
    // receiver is listening.
    if (!idn_sidecar_.open()) {
        log::warn("DacManager: IDN-Stream sidecar failed to open (non-fatal)");
    }

    // Open CommonLaserStream sidecar — TCP server port 7256 + UDP broadcast.
    // Mirrors every laser frame to any tool that speaks the CLS1 protocol
    // (LaserBoy, OpenLase, custom receivers, etc.).
    cls_sidecar_.set_point_rate(point_rate_);
    if (!cls_sidecar_.open()) {
        log::warn("DacManager: CommonLaserStream sidecar failed to open (non-fatal)");
    }

    scan_thread_   = std::thread([this]{ scan_thread_fn(); });
    output_thread_ = std::thread([this]{ output_thread_fn(); });

    // Elevate scan thread to above-normal (it mostly sleeps)
    set_thread_priority(scan_thread_,   ThreadPriority::AboveNormal);

    // Output thread is elevated INSIDE output_thread_fn() via
    // set_thread_priority_current() so it takes effect on the right thread.
    log::info("DacManager: started");
}

void DacManager::stop() {
    if (!running_.exchange(false)) return;

    if (scan_thread_.joinable())   scan_thread_.join();
    if (output_thread_.joinable()) output_thread_.join();

    // Close CITP/CAEX sidecar first so it stops broadcasting
    if (citp_caex_.is_open()) citp_caex_.close();

    // Close IDN-Stream broadcast sidecar
    if (idn_sidecar_.is_open()) idn_sidecar_.close();

    // Close CommonLaserStream sidecar
    if (cls_sidecar_.is_open()) cls_sidecar_.close();

    // Close all candidates
    {
        std::lock_guard<std::mutex> lk(candidates_mutex_);
        for (auto& dac : candidates_)
            if (dac && dac->is_open()) dac->close();
    }

    active_.store(nullptr);
    log::info("DacManager: stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_candidates — create one driver instance per supported type
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::build_candidates() {
    std::lock_guard<std::mutex> lk(candidates_mutex_);
    candidates_.clear();

    // Probe up to 4 physical Helios DACs (cover multi-DAC rigs)
    for (int i = 0; i < 4; ++i)
        candidates_.push_back(std::make_unique<HeliosDac>(i));

    // Always include the emulated Helios as a last resort
    candidates_.push_back(std::make_unique<HeliosEmulatedDac>());

    // EtherDream — discovered address will be set by probe_etherdream()
    candidates_.push_back(std::make_unique<EtherDreamDac>());

    // LaserDock
    candidates_.push_back(std::make_unique<LaserDockDac>());

    // IDN sender is opt-in only (address must be provided via force_dac)

    // NOTE: CitpCaexDac is NOT added to candidates_ here.  It lives as the
    // citp_caex_ sidecar member and is opened unconditionally in start(), so
    // Capture always receives a feed regardless of which primary DAC is active.
}

// ─────────────────────────────────────────────────────────────────────────────
//  scan_thread_fn — periodic hot-plug scan
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::scan_thread_fn() {
    set_thread_name("dac-scan");
    log::info("DacManager: scan thread started");

    while (running_.load()) {
        // Handle forced-DAC override
        if (force_flag_.exchange(false)) {
            std::lock_guard<std::mutex> lk(candidates_mutex_);

            // Close current active
            IDac* cur = active_.load();
            if (cur && cur->is_open()) cur->close();
            active_.store(nullptr);

            // Create the forced driver and attempt to open
            std::unique_ptr<IDac> forced;
            if (forced_type_ == "helios_emulated") {
                forced = std::make_unique<HeliosEmulatedDac>();
            } else if (forced_type_ == "etherdream") {
                forced = std::make_unique<EtherDreamDac>(forced_address_);
            } else if (forced_type_ == "idn") {
                forced = std::make_unique<IdnSender>(forced_address_);
            } else if (forced_type_ == "laserdock") {
                forced = std::make_unique<LaserDockDac>();
            } else if (forced_type_ == "citp_caex") {
                forced = std::make_unique<CitpCaexDac>();
            } else {
                forced = std::make_unique<HeliosDac>();
            }

            forced->set_point_rate(point_rate_);
            if (forced->open()) {
                active_.store(forced.get());
                candidates_.push_back(std::move(forced));
                log::info("DacManager: forced DAC '%s' activated",
                          forced_type_.c_str());
            } else {
                log::warn("DacManager: forced DAC '%s' failed to open",
                          forced_type_.c_str());
            }
        }

        // Normal auto-scan: check if active DAC is still connected
        {
            std::lock_guard<std::mutex> lk(candidates_mutex_);

            IDac* cur = active_.load();
            if (cur && !cur->is_open()) {
                log::warn("DacManager: active DAC '%s' disconnected",
                          cur->type_name());
                active_.store(nullptr);
                cur = nullptr;
            }

            // Priority order: real Helios > LaserDock > EtherDream > emulated
            static const char* kPriority[] = {
                "helios", "laserdock", "etherdream", "helios_emulated"
            };

            if (!cur) {
                for (const char* type : kPriority) {
                    for (auto& dac : candidates_) {
                        if (std::string(dac->type_name()) != type) continue;
                        if (dac->is_open()) {
                            active_.store(dac.get());
                            log::info("DacManager: activated '%s'", type);
                            goto done_scan;
                        }
                        // Try to open
                        dac->set_point_rate(point_rate_);
                        if (dac->open()) {
                            active_.store(dac.get());
                            log::info("DacManager: opened and activated '%s'", type);
                            goto done_scan;
                        }
                    }
                }
            }
            done_scan:;
        }

        // Probe EtherDream on the network periodically
        probe_etherdream();

        // Sleep between scans
        for (int i = 0; i < (kScanIntervalMs / 50) && running_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    log::info("DacManager: scan thread exiting");
}

// ─────────────────────────────────────────────────────────────────────────────
//  promote_best_candidate — select the highest-priority open DAC
//
//  Priority order (per spec §2.4): Helios > EtherDream > LaserDock > IDN.
//  Caller must hold candidates_mutex_.
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::promote_best_candidate() {
    static constexpr const char* kPriority[] = {
        "helios", "etherdream", "laserdock", "idn", "helios_emulated"
    };

    for (const char* type : kPriority) {
        for (auto& dac : candidates_) {
            if (std::string(dac->type_name()) != type) continue;
            if (dac->is_open()) {
                active_.store(dac.get());
                log::info("DacManager: promoted '%s' as active DAC", type);
                return;
            }
            dac->set_point_rate(point_rate_);
            if (dac->open()) {
                active_.store(dac.get());
                log::info("DacManager: opened and promoted '%s'", type);
                return;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  probe_etherdream — non-blocking EtherDream UDP discover
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::probe_etherdream() {
    // Short discover (100 ms) — long enough to catch a broadcast
    auto devices = EtherDreamDac::discover(100);
    if (devices.empty()) return;

    std::lock_guard<std::mutex> lk(candidates_mutex_);

    for (auto& [ip, bcast] : devices) {
        // Check if we already have this device
        bool already = false;
        for (auto& dac : candidates_) {
            if (std::string(dac->type_name()) != "etherdream") continue;
            if (dac->target_address() == ip) { already = true; break; }
        }
        if (!already) {
            auto ed = std::make_unique<EtherDreamDac>(bcast, ip);
            log::info("DacManager: discovered EtherDream at %s", ip.c_str());
            candidates_.push_back(std::move(ed));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  output_thread_fn — real-time laser output
//
//  Loop logic:
//    1. Poll RenderBus for next frame
//    2. If frame available: timestamp arrival, send points, record latency
//    3. If no frame: optionally send keep-alive (repeat last frame)
//    4. Tight-sleep to avoid spinning full CPU when idle
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::output_thread_fn() {
    set_thread_name("dac-output");
    set_thread_priority_current(ThreadPriority::Realtime);
    log::info("DacManager: output thread started (TIME_CRITICAL)");

    using Clock = std::chrono::steady_clock;

    auto last_frame_time = Clock::now();

    while (running_.load()) {
        IDac* dac = active_.load();

        // Poll for the next render frame
        const RenderFrame* frame = bus_.poll_dac();

        if (frame) {
            // --- Latency measurement start ---
            // The frame timestamp is set by the show engine when it calls
            // submit(). We measure from that timestamp to now.
            double arrival_s = HRTimer::now_s();
            double latency_ms = (arrival_s - frame->timestamp) * 1000.0;
            if (latency_ms < 0.0) latency_ms = 0.0; // clock wrap guard

            // Record latency
            {
                std::lock_guard<std::mutex> lk(stats_mutex_);
                latency_.push(latency_ms);
            }

            // Send points to active DAC
            if (dac && dac->is_open() && !frame->points.empty()) {
                // Negotiate point rate from frame metadata
                int negotiated = PointRateNegotiator::negotiate(
                    static_cast<int>(frame->point_rate),
                    dac->min_point_rate(),
                    dac->max_point_rate(),
                    static_cast<int>(frame->points.size()),
                    kDefaultNDIFPS);

                dac->set_point_rate(negotiated);
                dac->send_points(frame->points);
            }

            // CITP/CAEX sidecar — always forward every frame so Capture sees
            // the live feed regardless of which primary DAC is active.
            // Empty frames are intentionally sent: they keep Capture's "signal
            // present" state alive and prevent the feed from being marked dead.
            if (citp_caex_.is_open()) {
                citp_caex_.send_points(frame->points);
            }

            // IDN-Stream broadcast sidecar — mirror every frame to UDP broadcast
            // so IDN-capable receivers can visualise the feed.
            if (idn_sidecar_.is_open() && idn_sidecar_enabled_.load(std::memory_order_relaxed) && !frame->points.empty()) {
                idn_sidecar_.send_points(frame->points);
            }

            // CommonLaserStream sidecar — TCP/UDP CLS1 mirror for open receivers.
            if (cls_sidecar_.is_open() && !frame->points.empty()) {
                cls_sidecar_.send_points(frame->points);
            }

            // Virtual DirectShow camera — always push (even empty = black frame)
            // so the SHM sequence counter advances and the DLL delivers at 30fps.
            if (virtual_camera_.is_open()) {
                virtual_camera_.push_frame(frame->points);
            }

            // Cache this frame for keep-alive
            {
                std::lock_guard<std::mutex> lk(last_frame_mutex_);
                last_frame_ = frame->points;
            }

            last_frame_time = Clock::now();
            bus_.release_dac(frame);

        } else {
            // No new frame available.
            // If the DAC is hungry (past keep-alive window) re-send last frame.
            auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               Clock::now() - last_frame_time).count();

            if (idle_ms >= kKeepaliveMs) {
                std::lock_guard<std::mutex> lk(last_frame_mutex_);
                if (!last_frame_.empty()) {
                    if (dac && dac->is_open())
                        dac->send_points(last_frame_);
                    // Keep-alive for CITP/CAEX sidecar (last_frame_ may be empty —
                    // that is intentional, see send_points() contract).
                    if (citp_caex_.is_open())
                        citp_caex_.send_points(last_frame_);
                    // Keep-alive for IDN-Stream broadcast sidecar
                    if (idn_sidecar_.is_open() && idn_sidecar_enabled_.load(std::memory_order_relaxed))
                        idn_sidecar_.send_points(last_frame_);
                    // Keep-alive for CommonLaserStream sidecar
                    if (cls_sidecar_.is_open())
                        cls_sidecar_.send_points(last_frame_);
                    // Keep-alive for virtual DirectShow camera
                    if (virtual_camera_.is_open())
                        virtual_camera_.push_frame(last_frame_);
                    last_frame_time = Clock::now();
                }
            }

            // Tight-sleep to avoid 100% CPU spin
            std::this_thread::sleep_for(std::chrono::microseconds(kOutputSleepUs));
        }
    }

    log::info("DacManager: output thread exiting");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────
std::vector<DacStatus> DacManager::all_status() const {
    std::lock_guard<std::mutex> lk(candidates_mutex_);
    std::vector<DacStatus> out;
    out.reserve(candidates_.size());
    for (const auto& dac : candidates_)
        out.push_back(dac->status());
    return out;
}

IDac* DacManager::active_dac() const {
    return active_.load();
}

void DacManager::set_point_rate(int pps) {
    std::lock_guard<std::mutex> lk(candidates_mutex_);
    point_rate_ = pps;
    for (auto& dac : candidates_)
        dac->set_point_rate(pps);
    (void)citp_caex_.set_point_rate(pps);
    cls_sidecar_.set_point_rate(pps);
}

void DacManager::force_dac(const std::string& type, const std::string& address) {
    {
        std::lock_guard<std::mutex> lk(candidates_mutex_);
        forced_type_    = type;
        forced_address_ = address;
    }
    force_flag_.store(true, std::memory_order_release);
}

double DacManager::last_mean_latency_ms() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    return latency_.mean();
}

double DacManager::last_p99_latency_ms() const {
    std::lock_guard<std::mutex> lk(stats_mutex_);
    return latency_.p99();
}

} // namespace idhmfis
