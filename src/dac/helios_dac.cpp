// helios_dac.cpp
// Helios DAC driver implementation with dynamic libusb loading.
//
// Protocol reference: https://github.com/Gitle-m/helios_dac
// All libusb interaction goes through a function-pointer table populated at
// runtime so that the application starts cleanly even without the DLL.

#include "helios_dac.h"
#include "core/logger.h"

#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
using LibHandle = HMODULE;
#  define DYN_LOAD(name)    LoadLibraryW(L##name)
#  define DYN_SYM(h, s)     GetProcAddress((HMODULE)(h), s)
#  define DYN_CLOSE(h)      FreeLibrary((HMODULE)(h))
#else
#  include <dlfcn.h>
using LibHandle = void*;
#  ifdef __APPLE__
#    define DYN_LOAD(name)   dlopen("libusb-1.0.dylib", RTLD_LAZY)
#  else
#    define DYN_LOAD(name)   dlopen("libusb-1.0.so.0", RTLD_LAZY)
#  endif
#  define DYN_SYM(h, s)     dlsym((h), s)
#  define DYN_CLOSE(h)      dlclose(h)
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Minimal libusb-1.0 type definitions (no external header required)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// libusb transfer type constants
static constexpr int LUSB_SUCCESS               = 0;
static constexpr int LUSB_TRANSFER_TYPE_BULK    = 2;
static constexpr int LUSB_ENDPOINT_IN           = 0x80;
static constexpr int LUSB_REQUEST_TYPE_VENDOR   = (0x02 << 5);
static constexpr int LUSB_RECIPIENT_DEVICE      = 0x00;
static constexpr int LUSB_ENDPOINT_OUT          = 0x00;

// Opaque libusb types — we never dereference these directly
struct lusb_context;
struct lusb_device;
struct lusb_device_handle;

struct lusb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
};

// Function pointer types
using fn_init           = int  (*)(lusb_context**);
using fn_exit           = void (*)(lusb_context*);
using fn_get_device_list= int  (*)(lusb_context*, lusb_device***);
using fn_free_device_list=void (*)(lusb_device**, int);
using fn_get_descriptor = int  (*)(lusb_device*, lusb_device_descriptor*);
using fn_open           = int  (*)(lusb_device*, lusb_device_handle**);
using fn_close          = void (*)(lusb_device_handle*);
using fn_claim_interface= int  (*)(lusb_device_handle*, int);
using fn_release_interface=int (*)(lusb_device_handle*, int);
using fn_bulk_transfer  = int  (*)(lusb_device_handle*, uint8_t,
                                    uint8_t*, int, int*, unsigned int);
using fn_control_transfer=int  (*)(lusb_device_handle*, uint8_t, uint8_t,
                                    uint16_t, uint16_t,
                                    uint8_t*, uint16_t, unsigned int);
using fn_set_configuration=int (*)(lusb_device_handle*, int);
using fn_error_name     = const char* (*)(int);

} // anonymous namespace

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  LibusbFuncs — dynamic dispatch table
// ─────────────────────────────────────────────────────────────────────────────
struct HeliosDac::LibusbFuncs {
    fn_init             init              = nullptr;
    fn_exit             exit_fn           = nullptr;
    fn_get_device_list  get_device_list   = nullptr;
    fn_free_device_list free_device_list  = nullptr;
    fn_get_descriptor   get_descriptor    = nullptr;
    fn_open             open_fn           = nullptr;
    fn_close            close_fn          = nullptr;
    fn_claim_interface  claim_interface   = nullptr;
    fn_release_interface release_interface= nullptr;
    fn_bulk_transfer    bulk_transfer     = nullptr;
    fn_control_transfer control_transfer  = nullptr;
    fn_set_configuration set_configuration= nullptr;
    fn_error_name       error_name        = nullptr;

    bool all_loaded() const {
        return init && exit_fn && get_device_list && free_device_list &&
               get_descriptor && open_fn && close_fn &&
               claim_interface && release_interface &&
               bulk_transfer && control_transfer && set_configuration;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  HeliosDac — construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
HeliosDac::HeliosDac(int device_index)
    : device_index_(device_index)
    , fn_(std::make_unique<LibusbFuncs>())
{}

HeliosDac::~HeliosDac() {
    close();
    if (lib_handle_) {
        DYN_CLOSE(lib_handle_);
        lib_handle_ = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Dynamic library loading
// ─────────────────────────────────────────────────────────────────────────────
bool HeliosDac::load_libusb() {
    if (lib_handle_) return true; // already loaded

#ifdef _WIN32
    lib_handle_ = DYN_LOAD("libusb-1.0.dll");
#else
    lib_handle_ = DYN_LOAD("");  // macro fills in the platform path
#endif

    if (!lib_handle_) {
        log::warn("Helios: libusb not found — driver disabled");
        return false;
    }

#define LOAD_SYM(f, name) fn_->f = reinterpret_cast<decltype(fn_->f)>(DYN_SYM(lib_handle_, name))
    LOAD_SYM(init,               "libusb_init");
    LOAD_SYM(exit_fn,            "libusb_exit");
    LOAD_SYM(get_device_list,    "libusb_get_device_list");
    LOAD_SYM(free_device_list,   "libusb_free_device_list");
    LOAD_SYM(get_descriptor,     "libusb_get_device_descriptor");
    LOAD_SYM(open_fn,            "libusb_open");
    LOAD_SYM(close_fn,           "libusb_close");
    LOAD_SYM(claim_interface,    "libusb_claim_interface");
    LOAD_SYM(release_interface,  "libusb_release_interface");
    LOAD_SYM(bulk_transfer,      "libusb_bulk_transfer");
    LOAD_SYM(control_transfer,   "libusb_control_transfer");
    LOAD_SYM(set_configuration,  "libusb_set_configuration");
    LOAD_SYM(error_name,         "libusb_error_name");
#undef LOAD_SYM

    if (!fn_->all_loaded()) {
        log::warn("Helios: libusb symbol resolution incomplete — driver disabled");
        DYN_CLOSE(lib_handle_);
        lib_handle_ = nullptr;
        return false;
    }

    log::info("Helios: libusb loaded successfully");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  open()
// ─────────────────────────────────────────────────────────────────────────────
bool HeliosDac::open() {
    std::lock_guard<std::mutex> lk(mutex_);

    if (open_) return true;

    if (!load_libusb()) {
        cached_status_.error = "libusb not available";
        return false;
    }

    // Initialise libusb context
    auto* ctx = reinterpret_cast<lusb_context**>(&ctx_);
    int r = fn_->init(ctx);
    if (r != LUSB_SUCCESS) {
        std::string err = fn_->error_name ? fn_->error_name(r) : "unknown";
        log::error("Helios: libusb_init failed: %s", err.c_str());
        cached_status_.error = "libusb_init: " + err;
        return false;
    }

    // Enumerate devices and find device_index_-th Helios
    lusb_device** dev_list = nullptr;
    int count = fn_->get_device_list(reinterpret_cast<lusb_context*>(ctx_), &dev_list);
    if (count < 0) {
        cached_status_.error = "get_device_list failed";
        return false;
    }

    int found = 0;
    lusb_device* target = nullptr;
    for (int i = 0; i < count; ++i) {
        lusb_device_descriptor desc{};
        if (fn_->get_descriptor(dev_list[i], &desc) != LUSB_SUCCESS) continue;
        if (desc.idVendor == kHeliosVid && desc.idProduct == kHeliosPid) {
            if (found == device_index_) {
                target = dev_list[i];
                break;
            }
            ++found;
        }
    }

    if (!target) {
        fn_->free_device_list(dev_list, 1);
        log::warn("Helios: device index %d not found", device_index_);
        cached_status_.error = "device not found";
        return false;
    }

    // Open device handle
    auto** hdl = reinterpret_cast<lusb_device_handle**>(&dev_handle_);
    r = fn_->open_fn(target, hdl);
    fn_->free_device_list(dev_list, 1);

    if (r != LUSB_SUCCESS) {
        std::string err = fn_->error_name ? fn_->error_name(r) : "open failed";
        log::error("Helios: open failed: %s", err.c_str());
        cached_status_.error = err;
        return false;
    }

    fn_->set_configuration(reinterpret_cast<lusb_device_handle*>(dev_handle_), 1);
    r = fn_->claim_interface(reinterpret_cast<lusb_device_handle*>(dev_handle_), 0);
    if (r != LUSB_SUCCESS) {
        std::string err = fn_->error_name ? fn_->error_name(r) : "claim failed";
        log::error("Helios: claim_interface failed: %s", err.c_str());
        fn_->close_fn(reinterpret_cast<lusb_device_handle*>(dev_handle_));
        dev_handle_ = nullptr;
        cached_status_.error = err;
        return false;
    }

    // Query firmware version via control transfer
    uint8_t fw_buf[32]{};
    int transferred = fn_->control_transfer(
        reinterpret_cast<lusb_device_handle*>(dev_handle_),
        static_cast<uint8_t>(LUSB_REQUEST_TYPE_VENDOR | LUSB_ENDPOINT_IN),
        kHeliosCtrlFwVersion,
        0x0000, 0x0000,
        fw_buf, static_cast<uint16_t>(sizeof(fw_buf)),
        kHeliosCtrlTimeout);

    std::string fw_ver = "unknown";
    if (transferred >= 2)
        fw_ver = std::to_string(fw_buf[0]) + "." + std::to_string(fw_buf[1]);

    open_ = true;
    cached_status_.connected     = true;
    cached_status_.device_name   = "Helios DAC";
    cached_status_.driver_version = "libusb/" + fw_ver;
    cached_status_.point_rate    = point_rate_;
    cached_status_.buffer_free   = kHeliosMaxPoints;
    cached_status_.error         = "";

    log::info("Helios: opened (fw %s, idx %d)", fw_ver.c_str(), device_index_);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  close()
// ─────────────────────────────────────────────────────────────────────────────
void HeliosDac::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_) return;

    if (dev_handle_ && fn_ && fn_->release_interface) {
        fn_->release_interface(reinterpret_cast<lusb_device_handle*>(dev_handle_), 0);
        fn_->close_fn(reinterpret_cast<lusb_device_handle*>(dev_handle_));
        dev_handle_ = nullptr;
    }

    if (ctx_ && fn_ && fn_->exit_fn) {
        fn_->exit_fn(reinterpret_cast<lusb_context*>(ctx_));
        ctx_ = nullptr;
    }

    open_ = false;
    cached_status_ = DacStatus{};
    log::info("Helios: closed");
}

// ─────────────────────────────────────────────────────────────────────────────
//  is_open / status
// ─────────────────────────────────────────────────────────────────────────────
bool HeliosDac::is_open() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return open_;
}

DacStatus HeliosDac::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return cached_status_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  set_point_rate
// ─────────────────────────────────────────────────────────────────────────────
bool HeliosDac::set_point_rate(int pps) {
    if (pps < kHeliosMinPPS || pps > kHeliosMaxPPS) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    point_rate_ = pps;
    cached_status_.point_rate = pps;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  poll_ready — waits for the DAC FIFO to become free
// ─────────────────────────────────────────────────────────────────────────────
bool HeliosDac::poll_ready(int timeout_ms) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    auto* hdl = reinterpret_cast<lusb_device_handle*>(dev_handle_);

    while (Clock::now() < deadline) {
        uint8_t status_buf[2]{};
        int transferred = 0;
        int r = fn_->bulk_transfer(hdl,
            static_cast<uint8_t>(kHeliosEndpointIn),
            status_buf, 2,
            &transferred,
            static_cast<unsigned int>(kHeliosBulkTimeout));

        if (r == LUSB_SUCCESS && transferred >= 1) {
            if (status_buf[0] == kHeliosStatusReady) return true;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  convert_point — LaserPoint → HeliosPoint
//
//  LaserPoint uses ±32767 (ILDA) for x/y, 0–255 for RGB.
//  HeliosPoint uses 0–0xFFF (12-bit) for x/y, 0–255 for RGB/intensity.
//  Map: x_helios = (x_ilda + 32767) * 4095 / 65534
// ─────────────────────────────────────────────────────────────────────────────
HeliosPoint HeliosDac::convert_point(const LaserPoint& p) {
    HeliosPoint hp{};
    // Shift from signed ±32767 to unsigned 0..65534, then scale to 12-bit
    uint32_t ux = static_cast<uint32_t>(static_cast<int32_t>(p.x) + 32767);
    uint32_t uy = static_cast<uint32_t>(static_cast<int32_t>(p.y) + 32767);
    hp.x = static_cast<uint16_t>((ux * 4095u) / 65534u);
    hp.y = static_cast<uint16_t>((uy * 4095u) / 65534u);
    if (p.blanked) {
        hp.r = hp.g = hp.b = hp.i = 0;
    } else {
        hp.r = p.r;
        hp.g = p.g;
        hp.b = p.b;
        hp.i = 0xFF; // full intensity
    }
    return hp;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_frame — transmit one sub-frame over bulk EP
//
//  Frame format (from Helios protocol spec):
//    Byte 0: 0x80 | (count >> 8)
//    Byte 1: count & 0xFF
//    Byte 2: 0x00
//    Byte 3: flags (0x00 = repeat mode enabled)
//    Bytes 4..: packed HeliosPoint array
//
//  Total transfer size = 4 + count * sizeof(HeliosPoint)
//  Maximum 4096 points per frame.
// ─────────────────────────────────────────────────────────────────────────────
void HeliosDac::send_frame(const HeliosPoint* pts, int count) {
    constexpr int kHeaderSize = 4;

    // Build frame buffer on the stack for small frames, heap for large
    std::vector<uint8_t> buf;
    buf.resize(static_cast<size_t>(kHeaderSize + count * static_cast<int>(sizeof(HeliosPoint))));

    buf[0] = static_cast<uint8_t>(0x80 | ((count >> 8) & 0x0F));
    buf[1] = static_cast<uint8_t>(count & 0xFF);
    buf[2] = 0x00;
    buf[3] = 0x01; // flags: 0x01 = auto-repeat last frame on underrun

    std::memcpy(buf.data() + kHeaderSize, pts,
                static_cast<size_t>(count) * sizeof(HeliosPoint));

    auto* hdl = reinterpret_cast<lusb_device_handle*>(dev_handle_);
    int transferred = 0;
    fn_->bulk_transfer(hdl,
        static_cast<uint8_t>(kHeliosEndpointOut),
        buf.data(),
        static_cast<int>(buf.size()),
        &transferred,
        static_cast<unsigned int>(kHeliosBulkTimeout));
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_points
// ─────────────────────────────────────────────────────────────────────────────
int HeliosDac::send_points(const PointBuffer& pts) {
    std::unique_lock<std::mutex> lk(mutex_);
    if (!open_ || pts.empty()) return 0;

    // Convert all points first
    std::vector<HeliosPoint> converted;
    converted.reserve(pts.size());
    for (const auto& p : pts)
        converted.push_back(convert_point(p));

    int sent      = 0;
    int remaining = static_cast<int>(converted.size());

    while (remaining > 0) {
        // BUG #37: Release the mutex around the blocking poll_ready() call so
        // that close() is not stalled for the full 8 ms per iteration.
        // Re-check open_ after reacquiring in case close() ran during the wait.
        lk.unlock();
        bool ready = poll_ready(8); // blocking call, no lock held
        lk.lock();
        if (!open_) break; // close() called while waiting — abort the send

        if (!ready) {
            log::warn("Helios: DAC not ready after 8ms, dropping %d points", remaining);
            break;
        }

        int chunk = std::min(remaining, kHeliosMaxPoints);
        send_frame(converted.data() + sent, chunk);

        sent      += chunk;
        remaining -= chunk;
    }

    // buffer_free: 0 on full success; remaining unsent count on partial send.
    cached_status_.buffer_free = remaining;
    return sent;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDacOutput implementation
// ─────────────────────────────────────────────────────────────────────────────
int HeliosDac::negotiated_pps() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return negotiated_pps_;
}

std::string HeliosDac::id() const {
    return "helios:" + std::to_string(device_index_);
}

bool HeliosDac::send_frame(const PointBuffer& pts, int target_pps) {
    // Clamp rate to hardware limits and store as negotiated
    int clamped = std::clamp(target_pps, kHeliosMinPPS, kHeliosMaxPPS);
    // Update rate fields (set_point_rate acquires the mutex internally)
    set_point_rate(clamped);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        negotiated_pps_ = clamped;
    }
    int sent = send_points(pts);
    return sent == static_cast<int>(pts.size());
}

} // namespace idhmfis
