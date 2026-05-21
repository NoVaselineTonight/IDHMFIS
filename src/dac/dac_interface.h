#pragma once
// IDacOutput — unified point-rate-negotiation interface for all DAC drivers.
//
// This interface extends the capabilities of the existing IDac class with:
//   - Explicit connect/disconnect lifecycle (mirrors open/close semantics)
//   - Point rate negotiation: negotiated_pps() returns the rate the hardware
//     actually agreed to after a BEGIN/PREPARE exchange.
//   - send_frame(pts, target_pps): one-shot frame submit with inline negotiation.
//   - Human-readable id() and name() accessors.
//
// All existing DAC drivers (HeliosDac, EtherDreamDac, IdnSender) implement this
// interface in addition to IDac.

#include "core/types.h"
#include <string>

namespace idhmfis {

class IDacOutput {
public:
    virtual ~IDacOutput() = default;

    // Open the hardware connection.  Returns true on success.
    virtual bool connect() = 0;

    // Close the hardware connection and release resources.
    virtual void disconnect() = 0;

    // True if the connection is currently open and usable.
    virtual bool is_connected() const = 0;

    // The point rate actually agreed to by the hardware after negotiation.
    // Returns 0 if not yet connected / not yet negotiated.
    virtual int negotiated_pps() const = 0;

    // Absolute hardware maximum (a property of the device type, not state).
    virtual int max_pps() const = 0;

    // Submit one frame to the DAC, requesting target_pps.
    // The driver performs any necessary negotiation and rate-clamp internally.
    // Returns the number of points actually sent (may be < pts.size() on error).
    virtual bool send_frame(const PointBuffer& pts, int target_pps) = 0;

    // Unique string identifier.
    // Convention: serial number for USB devices, "IP:port" for network devices.
    virtual std::string id() const = 0;

    // Human-readable display name (e.g. "Helios #0", "EtherDream @ 192.168.1.5").
    virtual std::string name() const = 0;
};

} // namespace idhmfis
