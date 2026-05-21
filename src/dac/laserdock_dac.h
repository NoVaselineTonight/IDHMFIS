#pragma once
// LaserDock DAC driver.
//
// LaserDock is a low-cost USB laser DAC.
// Hardware: USB VID 0x1FC9  PID 0x04D8 (typical LaserDock/World Laser Dock)
//
// Communication: USB HID
//   - 64-byte OUT reports (report ID 0x00 or vendor-specific)
//   - Each report encodes up to 3 laser points (8 bytes each = 24 bytes)
//   - Enable/disable output via a control command byte
//
// Dynamic HID loading:
//   Uses HIDAPI (hidapi.dll on Windows) or raw WinUSB as fallback.
//   If HIDAPI is not found, driver returns is_open()==false.

#include "idac.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
using LibHandle = HMODULE;
#  define LD_DYN_LOAD(n)    LoadLibraryW(L##n)
#  define LD_DYN_SYM(h,s)   GetProcAddress((HMODULE)(h), s)
#  define LD_DYN_CLOSE(h)   FreeLibrary((HMODULE)(h))
#else
#  include <dlfcn.h>
using LibHandle = void*;
#  ifdef __APPLE__
#    define LD_DYN_LOAD(n)  dlopen("libhidapi.dylib", RTLD_LAZY)
#  else
#    define LD_DYN_LOAD(n)  dlopen("libhidapi-hidraw.so.0", RTLD_LAZY)
#  endif
#  define LD_DYN_SYM(h,s)   dlsym((h), s)
#  define LD_DYN_CLOSE(h)   dlclose(h)
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  LaserDock hardware constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kLdVid              = 0x1FC9;
static constexpr uint16_t kLdPid              = 0x04D8;
static constexpr int      kLdReportSize       = 64;    // HID report bytes
static constexpr int      kLdPointsPerReport  = 2;     // fit 2 × 24-byte points + header
static constexpr int      kLdDefaultPPS       = 30000;
static constexpr int      kLdMaxPPS           = 40000;
static constexpr int      kLdMinPPS           = 5000;

// Control bytes (first byte of each report)
static constexpr uint8_t  kLdCmdSendSamples   = 0x00; // normal point data
static constexpr uint8_t  kLdCmdEnable        = 0x01; // enable output
static constexpr uint8_t  kLdCmdDisable       = 0x02; // disable output

// ─────────────────────────────────────────────────────────────────────────────
//  On-wire point struct (8 bytes, packed)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct LdPoint {
    uint16_t x;     // 0–4095 (12-bit unsigned)
    uint16_t y;     // 0–4095
    uint8_t  r;
    uint8_t  g;
    uint8_t  b;
    uint8_t  flags; // 0x00 = normal, 0x01 = blanked
};
#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
//  LaserDockDac
// ─────────────────────────────────────────────────────────────────────────────
class LaserDockDac final : public IDac {
public:
    explicit LaserDockDac(int device_index = 0);
    ~LaserDockDac() override;

    bool      open()                          override;
    void      close()                         override;
    bool      is_open()              const    override;
    DacStatus status()               const    override;
    bool      set_point_rate(int pps)         override;
    int       max_point_rate()       const    override { return kLdMaxPPS; }
    int       min_point_rate()       const    override { return kLdMinPPS; }
    int       send_points(const PointBuffer&) override;
    const char* type_name()          const    override { return "laserdock"; }

private:
    bool load_hidapi();
    bool send_control_cmd(uint8_t cmd);
    int  send_report(const LdPoint* pts, int count); // returns points sent

    static LdPoint convert_point(const LaserPoint& p);

    int    device_index_ = 0;
    int    point_rate_   = kLdDefaultPPS;
    bool   open_         = false;

    void*  lib_handle_   = nullptr;
    void*  dev_handle_   = nullptr; // hid_device*

    mutable std::mutex  mutex_;
    mutable DacStatus   cached_status_;

    // HIDAPI function pointer table
    struct HidapiFuncs;
    std::unique_ptr<HidapiFuncs> fn_;
};

} // namespace idhmfis
