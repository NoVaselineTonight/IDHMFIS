#pragma once
// IDac — abstract interface that every DAC driver must implement.
// Drivers are instantiated by DacManager; consumers interact only with IDac.

#include "core/types.h"
#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Status snapshot returned by IDac::status().
//  Snapshot is cheap to copy and safe to read from any thread.
// ─────────────────────────────────────────────────────────────────────────────
struct DacStatus {
    bool        connected      = false;
    int         point_rate     = 0;
    int         buffer_free    = 0;    // points of free space in DAC FIFO
    float       temperature    = 0.f;  // degrees C, if available
    std::string device_name;
    std::string driver_version;
    std::string error;                 // last error string, "" if none
};

// ─────────────────────────────────────────────────────────────────────────────
//  IDac — pure virtual DAC driver interface
// ─────────────────────────────────────────────────────────────────────────────
class IDac {
public:
    virtual ~IDac() = default;

    // Open the device. Returns true on success.
    // May be called again after close() to reconnect.
    virtual bool open()  = 0;

    // Close the device and release all hardware resources.
    virtual void close() = 0;

    // Returns true if the device is currently open and usable.
    virtual bool is_open() const = 0;

    // Snapshot of current device status.  Thread-safe read.
    virtual DacStatus status() const = 0;

    // Change the output point rate.  Effective immediately if possible.
    // Returns false if the rate is out of range or cannot be applied.
    virtual bool set_point_rate(int pps) = 0;

    // Hardware limits reported by this driver.
    virtual int max_point_rate() const = 0;
    virtual int min_point_rate() const = 0;

    // Send a batch of points to the DAC.
    // Non-blocking if the internal FIFO has room.
    // Returns the number of points actually enqueued (may be less than pts.size()
    // if the buffer is nearly full — caller may retry remaining points).
    virtual int send_points(const PointBuffer& pts) = 0;

    // Short ASCII identifier, e.g. "helios", "etherdream", "idn", "laserdock".
    virtual const char* type_name() const = 0;

    // Network address (IP string) used to connect to this DAC, if applicable.
    // Returns "" for non-network DACs.
    virtual std::string target_address() const { return ""; }
};

} // namespace idhmfis
