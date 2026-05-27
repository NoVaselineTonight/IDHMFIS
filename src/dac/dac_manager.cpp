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
    // Hold lifecycle_mutex_ for the entire function so that a concurrent stop()
    // call must wait until both scan_thread_ and output_thread_ exist before it
    // attempts to join them.  Without this, stop() could race into the window
    // between running_.exchange(true) and the thread constructions, find
    // non-joinable threads, return immediately, and delete the manager while
    // start() is still running — causing use-after-free zombie threads.
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mutex_);
    // If stop() already ran (stop_requested_ set under this same mutex), bail
    // immediately without creating threads.  This handles the race where a stop
    // thread deletes the manager (via shared_ptr) before the detached start thread
    // even enters start() — with shared_ptr the object stays alive, but we must
    // not create scan/output threads for a manager that has been torn down.
    if (stop_requested_.load(std::memory_order_relaxed)) return;
    if (running_.exchange(true)) return; // already running

    // Stamp start time so is_healthy() can grant a grace period before the
    // output thread's first iteration sets last_loop_ms_.
    {
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        started_at_ms_.store(now_ms, std::memory_order_relaxed);
    }

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

    // Open virtual DirectShow/NDI camera sidecar — registers as "IDHMFIS Laser Preview"
    // in the Windows Video Capture Sources category so Capture 2024 can open it as
    // a video input.  Stream index 0 uses the canonical fixed names; stream N>0 gets
    // indexed names so each head appears as a distinct NDI source.  Non-fatal.
    virtual_camera_.set_stream_index(stream_idx_);
    if (!virtual_camera_.open()) {
        log::warn("DacManager: virtual camera sidecar failed to open (non-fatal)");
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
    // Acquire lifecycle_mutex_ briefly so we cannot set running_=false and
    // proceed to join() while start() is still in its thread-creation section.
    // Once we release the lock, start() has either (a) not yet been called,
    // (b) already returned, or (c) seen running_=false and returned early —
    // in all three cases scan_thread_ and output_thread_ are in a defined state.
    {
        std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mutex_);
        // Set stop_requested_ under the lock so start() (which also holds this
        // lock) will see it and bail without creating threads.  This covers the
        // race where the stop thread runs before the start thread: stop() returns
        // early (running_ was false), but start() will also return early when it
        // finally executes — preventing zombie threads on a deleted manager.
        stop_requested_.store(true, std::memory_order_relaxed);
        if (!running_.exchange(false)) return;
    }
    // lifecycle_mutex_ is released here.  The threads loop on running_.load()
    // and will exit now that running_ is false.

    if (scan_thread_.joinable())   scan_thread_.join();
    if (output_thread_.joinable()) output_thread_.join();

    // Close CITP/CAEX sidecar first so it stops broadcasting
    if (citp_caex_.is_open()) citp_caex_.close();

    // Close IDN-Stream broadcast sidecar
    if (idn_sidecar_.is_open()) idn_sidecar_.close();

    // Close CommonLaserStream sidecar
    if (cls_sidecar_.is_open()) cls_sidecar_.close();

    // Close virtual DirectShow/NDI camera sidecar
    if (virtual_camera_.is_open()) virtual_camera_.close();

    // Close all candidates and release the shared_ptr under a single lock pass.
    {
        std::lock_guard<std::mutex> lk(candidates_mutex_);
        for (auto& dac : candidates_)
            if (dac && dac->is_open()) dac->close();
        active_sp_.reset(); // BUG #6: release shared ownership under the mutex
    }
    active_.store(nullptr, std::memory_order_release);
    log::info("DacManager: stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_candidates — create one driver instance per supported type
// ─────────────────────────────────────────────────────────────────────────────
void DacManager::build_candidates() {
    std::lock_guard<std::mutex> lk(candidates_mutex_);
    // BUG #5 + BUG #6: Null out active_ and active_sp_ before clearing the
    // vector so the output thread never holds a raw pointer or shared reference
    // into a destroyed element.  The output thread checks `if (dac && dac->is_open())`,
    // so nullptr is handled safely.
    active_sp_.reset();
    active_.store(nullptr, std::memory_order_release);
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

            // Close current active (BUG #6: reset shared_ptr first)
            active_sp_.reset();
            IDac* cur = active_.load(std::memory_order_relaxed);
            if (cur && cur->is_open()) cur->close();
            active_.store(nullptr, std::memory_order_release);

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
                // BUG #6: push first so shared_ptr aliases the unique_ptr owner,
                // then set both active_sp_ and the fast-path atomic hint.
                candidates_.push_back(std::move(forced));
                IDac* raw = candidates_.back().get();
                // Alias the unique_ptr managed object via a shared_ptr with a
                // no-op deleter: lifetime is owned by unique_ptr in candidates_.
                active_sp_ = std::shared_ptr<IDac>(candidates_.back().get(),
                                                   [](IDac*){});
                active_.store(raw, std::memory_order_release);
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

            IDac* cur = active_.load(std::memory_order_relaxed);
            if (cur && !cur->is_open()) {
                log::warn("DacManager: active DAC '%s' disconnected",
                          cur->type_name());
                // BUG #6: reset shared_ptr under the mutex before clearing atomic
                active_sp_.reset();
                active_.store(nullptr, std::memory_order_release);
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
                            // BUG #35 + BUG #6: update active_sp_ and active_ while
                            // still under candidates_mutex_ so the store is properly
                            // sequenced after the push_back / is_open check.
                            active_sp_ = std::shared_ptr<IDac>(dac.get(), [](IDac*){});
                            active_.store(dac.get(), std::memory_order_release);
                            log::info("DacManager: activated '%s'", type);
                            goto done_scan;
                        }
                        // Try to open
                        dac->set_point_rate(point_rate_);
                        if (dac->open()) {
                            // BUG #35 + BUG #6: store inside the mutex to guarantee
                            // visibility ordering after the open() call completes.
                            active_sp_ = std::shared_ptr<IDac>(dac.get(), [](IDac*){});
                            active_.store(dac.get(), std::memory_order_release);
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
                // BUG #6 + BUG #35: update active_sp_ under candidates_mutex_
                // (caller holds it) then store the fast-path atomic pointer.
                active_sp_ = std::shared_ptr<IDac>(dac.get(), [](IDac*){});
                active_.store(dac.get(), std::memory_order_release);
                log::info("DacManager: promoted '%s' as active DAC", type);
                return;
            }
            dac->set_point_rate(point_rate_);
            if (dac->open()) {
                active_sp_ = std::shared_ptr<IDac>(dac.get(), [](IDac*){});
                active_.store(dac.get(), std::memory_order_release);
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
        // Heartbeat: stamp the current time so is_healthy() can detect a stuck thread.
        {
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count();
            last_loop_ms_.store(now_ms, std::memory_order_relaxed);
        }

        // BUG #6: Obtain a shared_ptr ref under the mutex so the DAC object
        // cannot be destroyed while we are in send_points() below.
        std::shared_ptr<IDac> dac_ref;
        {
            std::lock_guard<std::mutex> lk(candidates_mutex_);
            dac_ref = active_sp_;
        }
        IDac* dac = dac_ref.get();

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
    // BUG #34: The output thread may observe active_=nullptr for one or more
    // output cycles while the scan thread processes the flag and installs the
    // new DAC.  This is intentional — the output thread already guards all
    // sends with `if (dac && dac->is_open())` so the null window is safe.
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

bool DacManager::is_healthy(int64_t timeout_ms) const {
    if (!running_.load(std::memory_order_acquire)) return false;
    int64_t last = last_loop_ms_.load(std::memory_order_relaxed);
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last == 0) {
        // Output thread has not yet completed its first iteration.
        // Grant a 3× grace period from when start() was called so that the
        // EtherDream scan, HIDAPI enumeration, and sidecar opens (which can
        // collectively take 5–10 s with 8 concurrent managers) don't trigger
        // a false-positive eviction by check_output_health.
        int64_t started = started_at_ms_.load(std::memory_order_relaxed);
        if (started == 0) return true;  // start() not yet fully entered
        return (now_ms - started) < (timeout_ms * 3);
    }
    return (now_ms - last) < timeout_ms;
}

} // namespace idhmfis
