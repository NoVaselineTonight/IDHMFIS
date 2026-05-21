#pragma once
// Helios DAC driver.
//
// The Helios DAC is an open-source USB laser DAC by Gitle Mikkelsen.
// Protocol specification: https://github.com/Gitle-m/helios_dac
//
// Hardware:
//   USB VID 0x04D8  PID 0xFEED
//   Bulk endpoint EP1-OUT for point data (64-byte max-packet, 64-byte transfers)
//   Control transfers for status / firmware queries
//
// Dynamic libusb loading:
//   libusb-1.0 headers are NOT required at compile time.
//   All libusb function pointers are loaded at runtime from libusb-1.0.dll
//   (Windows) or libusb-1.0.so (Linux/macOS).
//   If the DLL is absent the driver gracefully returns is_open()==false.

#include "idac.h"
#include "dac_interface.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Helios protocol constants (from public spec, no external header needed)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kHeliosVid          = 0x04D8;
static constexpr uint16_t kHeliosPid          = 0xFEED;
static constexpr int      kHeliosEndpointOut  = 0x02;  // EP2-OUT bulk
static constexpr int      kHeliosEndpointIn   = 0x81;  // EP1-IN  bulk
static constexpr int      kHeliosCtrlTimeout  = 32;    // ms
static constexpr int      kHeliosBulkTimeout  = 8;     // ms
static constexpr int      kHeliosMaxPoints    = 0x1000; // 4096 per frame
static constexpr int      kHeliosDefaultPPS   = 30000;
static constexpr int      kHeliosMaxPPS       = 65000;
static constexpr int      kHeliosMinPPS       = 7000;

// Status byte values (polled via interrupt EP)
static constexpr uint8_t  kHeliosStatusBusy   = 0x00;
static constexpr uint8_t  kHeliosStatusReady  = 0x01;

// Control request codes
static constexpr uint8_t  kHeliosCtrlFwVersion = 0x04;
static constexpr uint8_t  kHeliosCtrlStop      = 0x01;
static constexpr uint8_t  kHeliosCtrlStart     = 0x02;

// ─────────────────────────────────────────────────────────────────────────────
//  On-wire point struct (packed, little-endian)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct HeliosPoint {
    uint16_t x;   // 0–0xFFF normalised  (not ±32767; hardware uses 12-bit)
    uint16_t y;
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  i;   // intensity / blanking
};
#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
//  HeliosDac — real hardware driver
// ─────────────────────────────────────────────────────────────────────────────
class HeliosDac final : public IDac, public IDacOutput {
public:
    // device_index: which Helios to open if multiple are connected (0-based).
    explicit HeliosDac(int device_index = 0);
    ~HeliosDac() override;

    // ── IDac interface ───────────────────────────────────────────────────────
    bool      open()                          override;
    void      close()                         override;
    bool      is_open()              const    override;
    DacStatus status()               const    override;
    bool      set_point_rate(int pps)         override;
    int       max_point_rate()       const    override { return kHeliosMaxPPS; }
    int       min_point_rate()       const    override { return kHeliosMinPPS; }
    int       send_points(const PointBuffer&) override;
    const char* type_name()          const    override { return "helios"; }

    // ── IDacOutput interface ─────────────────────────────────────────────────
    bool        connect()                     override { return open(); }
    void        disconnect()                  override { close(); }
    bool        is_connected()       const    override { return is_open(); }
    int         negotiated_pps()     const    override;
    int         max_pps()            const    override { return kHeliosMaxPPS; }
    bool        send_frame(const PointBuffer& pts, int target_pps) override;
    std::string id()                 const    override;
    std::string name()               const    override { return "Helios DAC #" + std::to_string(device_index_); }

private:
    // Opaque libusb handle stored as void* to avoid libusb header dependency
    void* ctx_         = nullptr;
    void* dev_handle_  = nullptr;

    int    device_index_     = 0;
    int    point_rate_       = kHeliosDefaultPPS;
    int    negotiated_pps_   = kHeliosDefaultPPS;
    bool   open_             = false;

    mutable std::mutex mutex_;
    mutable DacStatus  cached_status_;

    // Dynamic function pointers — populated by load_libusb()
    bool load_libusb();
    bool find_device();
    bool poll_ready(int timeout_ms = 10);
    void send_frame(const HeliosPoint* pts, int count);

    static HeliosPoint convert_point(const LaserPoint& p);

    // libusb function pointer table (opaque to avoid header)
    struct LibusbFuncs;
    std::unique_ptr<LibusbFuncs> fn_;

    // Library handle (HMODULE on Windows, void* elsewhere)
    void* lib_handle_ = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  HeliosEmulatedDac — software emulation, no hardware required
//  Useful for smoke-testing the pipeline without a physical device.
// ─────────────────────────────────────────────────────────────────────────────
class HeliosEmulatedDac final : public IDac {
public:
    HeliosEmulatedDac() = default;
    ~HeliosEmulatedDac() override = default;

    bool open() override {
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }

    DacStatus status() const override {
        DacStatus s;
        s.connected    = open_;
        s.point_rate   = point_rate_;
        s.buffer_free  = kHeliosMaxPoints;
        s.device_name  = "Helios (Emulated)";
        s.driver_version = "emu-1.0";
        return s;
    }

    bool set_point_rate(int pps) override {
        if (pps < kHeliosMinPPS || pps > kHeliosMaxPPS) return false;
        point_rate_ = pps;
        return true;
    }
    int max_point_rate() const override { return kHeliosMaxPPS; }
    int min_point_rate() const override { return kHeliosMinPPS; }

    int send_points(const PointBuffer& pts) override {
        if (!open_) return 0;
        // Points are intentionally discarded — this is a null sink.
        points_sent_ += static_cast<uint64_t>(pts.size());
        return static_cast<int>(pts.size());
    }

    const char* type_name() const override { return "helios_emulated"; }

    uint64_t total_points_sent() const { return points_sent_; }

private:
    bool     open_        = false;
    int      point_rate_  = kHeliosDefaultPPS;
    uint64_t points_sent_ = 0;
};

} // namespace idhmfis
