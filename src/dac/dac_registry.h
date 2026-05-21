#pragma once
// DacRegistry — background DAC hot-plug scanner and device inventory.
//
// The registry maintains a live list of DacDescriptors and notifies callers
// via on_changed when devices are added or removed.  It runs a background
// scan_loop() thread that:
//   - Probes for Helios USB DACs via dynamic libusb (skips gracefully if absent)
//   - Probes for EtherDream DACs via UDP broadcast discovery (port 7654)
//   - Checks reachability of any configured IDN-Stream targets
//
// Thread safety: all public methods are thread-safe.  on_changed may be fired
// from the scan thread — the callback must not block for long.

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  DAC type tag
// ─────────────────────────────────────────────────────────────────────────────
enum class DacType { Helios, EtherDream, IdnStream, Unknown };

// ─────────────────────────────────────────────────────────────────────────────
//  DacDescriptor — lightweight description of a discovered (or lost) device
// ─────────────────────────────────────────────────────────────────────────────
struct DacDescriptor {
    DacType     type       = DacType::Unknown;
    std::string id;          // unique: serial/index (Helios), IP:port (EtherDream/IDN)
    std::string name;        // human-readable display name
    int         max_pps    = 30000;
    bool        connected  = false;
    bool        active     = false; // currently producing output
};

// ─────────────────────────────────────────────────────────────────────────────
//  DacRegistry
// ─────────────────────────────────────────────────────────────────────────────
class DacRegistry {
public:
    DacRegistry()  = default;
    ~DacRegistry() { stop_scan(); }

    // Non-copyable, non-movable (owns a thread).
    DacRegistry(const DacRegistry&)            = delete;
    DacRegistry& operator=(const DacRegistry&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    // Start the background scan loop.  Safe to call multiple times.
    void start_scan();

    // Stop the scan loop and join the thread.  Blocks until the thread exits.
    void stop_scan();

    // ── Device list ─────────────────────────────────────────────────────────

    // Returns a snapshot of all currently known devices.
    // Thread-safe; called from the engine thread.
    std::vector<DacDescriptor> list() const;

    // ── Selection ───────────────────────────────────────────────────────────

    // Mark a device as the desired output target.
    void select(const std::string& id);

    // Returns the currently selected device id, or "" if none.
    std::string selected_id() const;

    // ── IDN-Stream targets ───────────────────────────────────────────────────

    // Register a host:port that the scan loop should poll for reachability.
    // If omitted the port defaults to 7255.
    void add_idn_target(const std::string& host, uint16_t port = 7255);

    // ── Scanner callbacks (called from internal scanner threads) ─────────────

    // Notify the registry that a device was found.
    void on_found(DacDescriptor desc);

    // Notify the registry that a device was lost.
    void on_lost(const std::string& id);

    // ── Hot-plug notification ────────────────────────────────────────────────

    // Fired on the scan thread whenever the device list changes.
    // Signature: void(const std::vector<DacDescriptor>&)
    std::function<void(const std::vector<DacDescriptor>&)> on_changed;

private:
    void scan_loop();

    // Sub-probes called from scan_loop()
    void probe_helios();
    void probe_etherdream();
    void probe_idn();

    mutable std::mutex          mutex_;
    std::vector<DacDescriptor>  devices_;
    std::string                 selected_id_;

    // IDN targets registered by the user
    struct IdnTarget { std::string host; uint16_t port; };
    std::vector<IdnTarget>      idn_targets_;

    std::atomic<bool>           scanning_{ false };
    std::thread                 scan_thread_;
};

} // namespace idhmfis
