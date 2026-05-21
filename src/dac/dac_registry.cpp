// dac_registry.cpp
// Background hot-plug scanner for Helios (USB), EtherDream (UDP), and IDN-Stream.

#include "dac_registry.h"
#include "etherdream_dac.h"   // EtherDreamDac::discover(), kEDMaxPPS
#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

// ── Winsock / POSIX socket headers for IDN reachability probe ─────────────
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
namespace {
struct WsaInitReg {
    WSADATA w;
    WsaInitReg()  { WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaInitReg() { WSACleanup(); }
};
static WsaInitReg g_wsa_reg;
using RegSockFd = SOCKET;
static constexpr RegSockFd kRegInvalidSock = INVALID_SOCKET;
inline void reg_close_socket(RegSockFd s) { closesocket(s); }
} // anonymous namespace
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
using RegSockFd = int;
static constexpr RegSockFd kRegInvalidSock = -1;
inline void reg_close_socket(RegSockFd s) { ::close(s); }
#endif

// ── Dynamic libusb types (mirrors the minimal set from helios_dac.cpp) ────
#ifdef _WIN32
#  include <windows.h>
using LibHandle = HMODULE;
#  define DYN_LOAD_USB()    LoadLibraryW(L"libusb-1.0.dll")
#  define DYN_SYM_USB(h, s) GetProcAddress((HMODULE)(h), s)
#  define DYN_CLOSE_USB(h)  FreeLibrary((HMODULE)(h))
#else
#  include <dlfcn.h>
using LibHandle = void*;
#  ifdef __APPLE__
#    define DYN_LOAD_USB()  dlopen("libusb-1.0.dylib", RTLD_LAZY)
#  else
#    define DYN_LOAD_USB()  dlopen("libusb-1.0.so.0", RTLD_LAZY)
#  endif
#  define DYN_SYM_USB(h, s) dlsym((h), s)
#  define DYN_CLOSE_USB(h)  dlclose(h)
#endif

// Known Helios VID/PID
static constexpr uint16_t kHeliosVid = 0x04D8;
static constexpr uint16_t kHeliosPid = 0xFEED;

// Minimal libusb structs (no header needed)
namespace {

struct lusb_ctx;
struct lusb_dev;
struct lusb_dev_hdl;

struct lusb_desc {
    uint8_t  bLength, bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass, bDeviceSubClass, bDeviceProtocol, bMaxPacketSize0;
    uint16_t idVendor, idProduct, bcdDevice;
    uint8_t  iManufacturer, iProduct, iSerialNumber, bNumConfigurations;
};

using pfn_init      = int  (*)(lusb_ctx**);
using pfn_exit      = void (*)(lusb_ctx*);
using pfn_get_list  = int  (*)(lusb_ctx*, lusb_dev***);
using pfn_free_list = void (*)(lusb_dev**, int);
using pfn_get_desc  = int  (*)(lusb_dev*, lusb_desc*);

struct UsbFuncs {
    pfn_init      init      = nullptr;
    pfn_exit      exit_fn   = nullptr;
    pfn_get_list  get_list  = nullptr;
    pfn_free_list free_list = nullptr;
    pfn_get_desc  get_desc  = nullptr;

    bool ok() const { return init && exit_fn && get_list && free_list && get_desc; }
};

static void* g_usb_lib   = nullptr;
static bool  g_usb_tried = false;

static UsbFuncs* usb_funcs() {
    static UsbFuncs fn;
    if (g_usb_tried) return fn.ok() ? &fn : nullptr;
    g_usb_tried = true;

    g_usb_lib = DYN_LOAD_USB();
    if (!g_usb_lib) {
        idhmfis::log::warn("DacRegistry: libusb not found — Helios scan disabled");
        return nullptr;
    }

#define LSYM(f, n) fn.f = reinterpret_cast<decltype(fn.f)>(DYN_SYM_USB(g_usb_lib, n))
    LSYM(init,     "libusb_init");
    LSYM(exit_fn,  "libusb_exit");
    LSYM(get_list, "libusb_get_device_list");
    LSYM(free_list,"libusb_free_device_list");
    LSYM(get_desc, "libusb_get_device_descriptor");
#undef LSYM

    if (!fn.ok()) {
        idhmfis::log::warn("DacRegistry: libusb symbol load incomplete — Helios scan disabled");
        DYN_CLOSE_USB(g_usb_lib);
        g_usb_lib = nullptr;
        return nullptr;
    }
    idhmfis::log::info("DacRegistry: libusb loaded for hot-plug scan");
    return &fn;
}

} // anonymous namespace

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::start_scan() {
    if (scanning_.exchange(true)) return; // already running

    scan_thread_ = std::thread([this]{ scan_loop(); });
    log::info("DacRegistry: scan started");
}

void DacRegistry::stop_scan() {
    if (!scanning_.exchange(false)) return; // not running

    if (scan_thread_.joinable())
        scan_thread_.join();

    log::info("DacRegistry: scan stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Device list
// ─────────────────────────────────────────────────────────────────────────────
std::vector<DacDescriptor> DacRegistry::list() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return devices_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Selection
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::select(const std::string& id) {
    std::lock_guard<std::mutex> lk(mutex_);
    selected_id_ = id;
}

std::string DacRegistry::selected_id() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return selected_id_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDN target management
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::add_idn_target(const std::string& host, uint16_t port) {
    std::lock_guard<std::mutex> lk(mutex_);
    // Avoid duplicates
    for (const auto& t : idn_targets_) {
        if (t.host == host && t.port == port) return;
    }
    idn_targets_.push_back({ host, port });
}

// ─────────────────────────────────────────────────────────────────────────────
//  on_found / on_lost (called from scanner helpers below)
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::on_found(DacDescriptor desc) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // Check for existing entry with the same id
        auto it = std::find_if(devices_.begin(), devices_.end(),
                               [&](const DacDescriptor& d){ return d.id == desc.id; });
        if (it == devices_.end()) {
            desc.connected = true;
            devices_.push_back(desc);
            changed = true;
            log::info("DacRegistry: found %s (%s)",
                      desc.name.c_str(), desc.id.c_str());
        } else {
            if (!it->connected) {
                it->connected = true;
                changed = true;
                log::info("DacRegistry: reconnected %s (%s)",
                          it->name.c_str(), it->id.c_str());
            }
        }
    }
    if (changed && on_changed) {
        on_changed(list());
    }
}

void DacRegistry::on_lost(const std::string& id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(devices_.begin(), devices_.end(),
                               [&](const DacDescriptor& d){ return d.id == id; });
        if (it != devices_.end() && it->connected) {
            it->connected = false;
            it->active    = false;
            changed = true;
            log::info("DacRegistry: lost %s (%s)", it->name.c_str(), id.c_str());
        }
    }
    if (changed && on_changed) {
        on_changed(list());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  probe_helios — enumerate USB devices looking for Helios VID/PID
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::probe_helios() {
    UsbFuncs* fn = usb_funcs();
    if (!fn) return; // libusb unavailable

    lusb_ctx* ctx = nullptr;
    if (fn->init(&ctx) != 0) {
        log::warn("DacRegistry: libusb_init failed in probe_helios");
        return;
    }

    lusb_dev** dev_list = nullptr;
    int count = fn->get_list(ctx, &dev_list);
    if (count < 0) {
        fn->exit_fn(ctx);
        return;
    }

    // Build the set of currently visible Helios IDs
    std::vector<std::string> visible_ids;
    int helios_idx = 0;
    for (int i = 0; i < count; ++i) {
        lusb_desc desc{};
        if (fn->get_desc(dev_list[i], &desc) != 0) continue;
        if (desc.idVendor == kHeliosVid && desc.idProduct == kHeliosPid) {
            std::ostringstream oss;
            oss << "helios:" << helios_idx;
            std::string id = oss.str();
            visible_ids.push_back(id);

            DacDescriptor d;
            d.type    = DacType::Helios;
            d.id      = id;
            d.name    = "Helios DAC #" + std::to_string(helios_idx);
            d.max_pps = 65000;
            on_found(d);
            ++helios_idx;
        }
    }

    fn->free_list(dev_list, 1);
    fn->exit_fn(ctx);

    // Check for devices that were previously seen but are now gone
    std::vector<DacDescriptor> current;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        current = devices_;
    }
    for (const auto& d : current) {
        if (d.type != DacType::Helios || !d.connected) continue;
        bool still_present = std::find(visible_ids.begin(), visible_ids.end(), d.id)
                             != visible_ids.end();
        if (!still_present) {
            on_lost(d.id);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  probe_etherdream — UDP broadcast discover, 200 ms window
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::probe_etherdream() {
    auto devices = EtherDreamDac::discover(200);

    std::vector<std::string> visible_ids;
    for (auto& [ip, bcast] : devices) {
        // Build id as "ip:7765"
        std::string id = ip + ":7765";
        visible_ids.push_back(id);

        DacDescriptor d;
        d.type    = DacType::EtherDream;
        d.id      = id;
        d.name    = "EtherDream @ " + ip;
        d.max_pps = static_cast<int>(bcast.max_point_rate);
        if (d.max_pps <= 0) d.max_pps = 100000;
        on_found(d);
    }

    // Mark as lost any EtherDream that did not respond this round
    std::vector<DacDescriptor> current;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        current = devices_;
    }
    for (const auto& d : current) {
        if (d.type != DacType::EtherDream || !d.connected) continue;
        bool still_present = std::find(visible_ids.begin(), visible_ids.end(), d.id)
                             != visible_ids.end();
        if (!still_present) {
            on_lost(d.id);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  probe_idn — attempt a UDP socket sendto to each registered IDN target
//  "Reachable" is defined as the OS accepting the send without ENETUNREACH.
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::probe_idn() {
    std::vector<IdnTarget> targets;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        targets = idn_targets_;
    }

    for (const auto& tgt : targets) {
        std::ostringstream oss;
        oss << tgt.host << ":" << tgt.port;
        std::string id = oss.str();

        // Try to resolve and create a temporary UDP socket
        RegSockFd sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == kRegInvalidSock) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(tgt.port);

        bool resolved = false;
        // Try numeric first, then hostname
        if (::inet_pton(AF_INET, tgt.host.c_str(), &addr.sin_addr) == 1) {
            resolved = true;
        } else {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            addrinfo* res = nullptr;
            if (getaddrinfo(tgt.host.c_str(), nullptr, &hints, &res) == 0 && res) {
                std::memcpy(&addr.sin_addr,
                    &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                    sizeof(addr.sin_addr));
                freeaddrinfo(res);
                resolved = true;
            }
        }

        if (resolved) {
            // Send a zero-byte probe; if it doesn't ENETUNREACH we treat as reachable
            int r = static_cast<int>(::sendto(sock, "", 0, 0,
                reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
            bool reachable = (r >= 0);

            if (reachable) {
                DacDescriptor d;
                d.type    = DacType::IdnStream;
                d.id      = id;
                d.name    = "IDN-Stream @ " + tgt.host;
                d.max_pps = 100000;
                on_found(d);
            } else {
                on_lost(id);
            }
        } else {
            // Cannot resolve → mark lost
            on_lost(id);
        }

        reg_close_socket(sock);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  scan_loop — runs on scan_thread_; probes every 2 seconds
// ─────────────────────────────────────────────────────────────────────────────
void DacRegistry::scan_loop() {
    log::info("DacRegistry: scan loop started");

    while (scanning_.load(std::memory_order_acquire)) {
        // Run all three probes
        probe_helios();
        probe_etherdream();
        probe_idn();

        // Sleep 2 seconds in 100 ms slices so we can exit promptly
        for (int i = 0; i < 20 && scanning_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    log::info("DacRegistry: scan loop exiting");
}

} // namespace idhmfis
