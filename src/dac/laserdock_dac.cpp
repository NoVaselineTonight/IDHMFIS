// laserdock_dac.cpp
// LaserDock USB HID driver with dynamic HIDAPI loading.

#include "laserdock_dac.h"
#include "core/logger.h"

#include <cstring>
#include <algorithm>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal HIDAPI types (no header required at compile time)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct hid_device_info {
    char*              path;
    unsigned short     vendor_id;
    unsigned short     product_id;
    wchar_t*           serial_number;
    unsigned short     release_number;
    wchar_t*           manufacturer_string;
    wchar_t*           product_string;
    unsigned short     usage_page;
    unsigned short     usage;
    int                interface_number;
    hid_device_info*   next;
};

struct hid_device; // opaque

using fn_hid_init          = int  (*)(void);
using fn_hid_exit          = void (*)(void);
using fn_hid_enumerate     = hid_device_info* (*)(unsigned short, unsigned short);
using fn_hid_free_enumeration=void (*)(hid_device_info*);
using fn_hid_open          = hid_device* (*)(unsigned short, unsigned short,
                                              const wchar_t*);
using fn_hid_open_path     = hid_device* (*)(const char*);
using fn_hid_close         = void (*)(hid_device*);
using fn_hid_write         = int  (*)(hid_device*, const unsigned char*, size_t);
using fn_hid_read          = int  (*)(hid_device*, unsigned char*, size_t);
using fn_hid_error         = const wchar_t* (*)(hid_device*);

} // anonymous namespace

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  HidapiFuncs — dynamic dispatch table
// ─────────────────────────────────────────────────────────────────────────────
struct LaserDockDac::HidapiFuncs {
    fn_hid_init             init              = nullptr;
    fn_hid_exit             exit_fn           = nullptr;
    fn_hid_enumerate        enumerate         = nullptr;
    fn_hid_free_enumeration free_enumeration  = nullptr;
    fn_hid_open             open_fn           = nullptr;
    fn_hid_open_path        open_path         = nullptr;
    fn_hid_close            close_fn          = nullptr;
    fn_hid_write            write_fn          = nullptr;
    fn_hid_read             read_fn           = nullptr;
    fn_hid_error            error_fn          = nullptr;

    bool all_loaded() const {
        return init && exit_fn && enumerate && free_enumeration &&
               open_fn && close_fn && write_fn && read_fn;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────
LaserDockDac::LaserDockDac(int device_index)
    : device_index_(device_index)
    , fn_(std::make_unique<HidapiFuncs>())
{}

LaserDockDac::~LaserDockDac() {
    close();
    if (lib_handle_) {
        LD_DYN_CLOSE(lib_handle_);
        lib_handle_ = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Dynamic library loading
// ─────────────────────────────────────────────────────────────────────────────
bool LaserDockDac::load_hidapi() {
    if (lib_handle_) return true;

#ifdef _WIN32
    lib_handle_ = LD_DYN_LOAD("hidapi.dll");
    if (!lib_handle_)
        lib_handle_ = LD_DYN_LOAD("hidapi-libusb.dll");
#else
    lib_handle_ = LD_DYN_LOAD("");  // macro handles platform path
#endif

    if (!lib_handle_) {
        log::warn("LaserDock: hidapi not found — driver disabled");
        return false;
    }

#define LD_SYM(f, name) fn_->f = reinterpret_cast<decltype(fn_->f)>(LD_DYN_SYM(lib_handle_, name))
    LD_SYM(init,             "hid_init");
    LD_SYM(exit_fn,          "hid_exit");
    LD_SYM(enumerate,        "hid_enumerate");
    LD_SYM(free_enumeration, "hid_free_enumeration");
    LD_SYM(open_fn,          "hid_open");
    LD_SYM(open_path,        "hid_open_path");
    LD_SYM(close_fn,         "hid_close");
    LD_SYM(write_fn,         "hid_write");
    LD_SYM(read_fn,          "hid_read");
    LD_SYM(error_fn,         "hid_error");
#undef LD_SYM

    if (!fn_->all_loaded()) {
        log::warn("LaserDock: hidapi symbol resolution incomplete");
        LD_DYN_CLOSE(lib_handle_);
        lib_handle_ = nullptr;
        return false;
    }

    fn_->init();
    log::info("LaserDock: hidapi loaded");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  open
// ─────────────────────────────────────────────────────────────────────────────
bool LaserDockDac::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (open_) return true;

    if (!load_hidapi()) {
        cached_status_.error = "hidapi not available";
        return false;
    }

    // Enumerate HID devices to find the right index
    hid_device_info* info = fn_->enumerate(kLdVid, kLdPid);
    if (!info) {
        log::warn("LaserDock: no device found (VID=%04X PID=%04X)", kLdVid, kLdPid);
        cached_status_.error = "device not found";
        return false;
    }

    hid_device_info* cur    = info;
    int              found  = 0;
    const char*      path   = nullptr;

    while (cur) {
        if (found == device_index_) {
            path = cur->path;
            break;
        }
        ++found;
        cur = cur->next;
    }

    if (!path) {
        fn_->free_enumeration(info);
        log::warn("LaserDock: device index %d not found", device_index_);
        cached_status_.error = "device index not found";
        return false;
    }

    dev_handle_ = fn_->open_path(path);
    fn_->free_enumeration(info);

    if (!dev_handle_) {
        log::error("LaserDock: hid_open_path failed");
        cached_status_.error = "hid_open failed";
        return false;
    }

    // Enable output
    if (!send_control_cmd(kLdCmdEnable)) {
        log::warn("LaserDock: enable command failed (non-fatal)");
    }

    open_ = true;
    cached_status_.connected     = true;
    cached_status_.device_name   = "LaserDock";
    cached_status_.driver_version = "hidapi/1.0";
    cached_status_.point_rate    = point_rate_;
    cached_status_.buffer_free   = 64;
    cached_status_.error         = "";
    log::info("LaserDock: opened (idx %d)", device_index_);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  close
// ─────────────────────────────────────────────────────────────────────────────
void LaserDockDac::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_) return;

    if (dev_handle_) {
        send_control_cmd(kLdCmdDisable);
        fn_->close_fn(reinterpret_cast<hid_device*>(dev_handle_));
        dev_handle_ = nullptr;
    }

    open_ = false;
    cached_status_ = DacStatus{};
    log::info("LaserDock: closed");
}

bool LaserDockDac::is_open() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return open_;
}

DacStatus LaserDockDac::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return cached_status_;
}

bool LaserDockDac::set_point_rate(int pps) {
    if (pps < kLdMinPPS || pps > kLdMaxPPS) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    point_rate_ = pps;
    cached_status_.point_rate = pps;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_control_cmd — 64-byte HID report with single command byte
// ─────────────────────────────────────────────────────────────────────────────
bool LaserDockDac::send_control_cmd(uint8_t cmd) {
    if (!dev_handle_) return false;

    uint8_t report[kLdReportSize + 1]{};  // +1 for report ID prefix
    report[0] = 0x00; // report ID
    report[1] = cmd;

    int r = fn_->write_fn(reinterpret_cast<hid_device*>(dev_handle_),
                          report, sizeof(report));
    return r >= 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  convert_point
//
//  LaserPoint: x/y ±32767, RGB 0–255
//  LdPoint:    x/y 0–4095 (12-bit unsigned), RGB 0–255
// ─────────────────────────────────────────────────────────────────────────────
LdPoint LaserDockDac::convert_point(const LaserPoint& p) {
    LdPoint lp{};
    uint32_t ux = static_cast<uint32_t>(static_cast<int32_t>(p.x) + 32767);
    uint32_t uy = static_cast<uint32_t>(static_cast<int32_t>(p.y) + 32767);
    lp.x = static_cast<uint16_t>((ux * 4095u) / 65534u);
    lp.y = static_cast<uint16_t>((uy * 4095u) / 65534u);
    if (p.blanked) {
        lp.r = lp.g = lp.b = 0;
        lp.flags = 0x01;
    } else {
        lp.r     = p.r;
        lp.g     = p.g;
        lp.b     = p.b;
        lp.flags = 0x00;
    }
    return lp;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_report — pack up to kLdPointsPerReport points into a 64-byte HID report
//
//  Report layout:
//    Byte 0:    report ID (0x00)
//    Byte 1:    kLdCmdSendSamples (0x00) + num_points in this report
//    Bytes 2–N: packed LdPoint structs
// ─────────────────────────────────────────────────────────────────────────────
int LaserDockDac::send_report(const LdPoint* pts, int count) {
    // BUG #33: Verify at compile time that the fixed report buffer is large
    // enough to hold the maximum number of points.
    static_assert(3 + kLdPointsPerReport * sizeof(LdPoint) <= kLdReportSize + 1,
        "LaserDock report buffer too small for kLdPointsPerReport points");

    int actual = std::min(count, kLdPointsPerReport);
    // BUG #33 (runtime clamp): guard against future constant changes where
    // kLdPointsPerReport > (kLdReportSize - 2) / sizeof(LdPoint).
    actual = std::min(actual, static_cast<int>((kLdReportSize - 2) / sizeof(LdPoint)));

    uint8_t report[kLdReportSize + 1]{};
    report[0] = 0x00;                          // HID report ID
    report[1] = kLdCmdSendSamples;
    report[2] = static_cast<uint8_t>(actual);  // num points in this report

    for (int i = 0; i < actual; ++i) {
        std::memcpy(report + 3 + i * static_cast<int>(sizeof(LdPoint)),
                    &pts[i], sizeof(LdPoint));
    }

    int r = fn_->write_fn(reinterpret_cast<hid_device*>(dev_handle_),
                          report, sizeof(report));
    return (r >= 0) ? actual : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_points
// ─────────────────────────────────────────────────────────────────────────────
int LaserDockDac::send_points(const PointBuffer& pts) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_ || pts.empty()) return 0;

    std::vector<LdPoint> converted;
    converted.reserve(pts.size());
    for (const auto& p : pts)
        converted.push_back(convert_point(p));

    int sent      = 0;
    int remaining = static_cast<int>(converted.size());

    while (remaining > 0) {
        int n = send_report(converted.data() + sent,
                            std::min(remaining, kLdPointsPerReport));
        if (n <= 0) break;
        sent      += n;
        remaining -= n;
    }

    return sent;
}

} // namespace idhmfis
