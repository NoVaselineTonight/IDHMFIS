// citp_caex_dac.cpp
// CITP/CAEX laser source output driver.
//
// Makes IDHMFIS appear as a laser feed source in Capture 2024.
//
// Thread model:
//   announce_thread_    — sends PLoc UDP multicast beacon every 1 s
//   tcp_server_thread_  — accepts one TCP client at a time; handles the CITP
//                         peer-name / feed-list / feed-control handshake
//   Caller thread       — calls send_points() which builds and multicasts
//                         LaserFeedFrame packets
//
// All on-wire integers are little-endian (matches x86 native).

#include "citp_caex_dac.h"
#include "core/logger.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

// ── Winsock initialisation (idiomatic: one static object per TU) ─────────────
#ifdef _WIN32
#  include <windows.h>   // GetModuleFileNameA
#  include <iphlpapi.h>  // GetBestInterface, GetAdaptersInfo
#  include <memory>      // std::make_unique
namespace {
struct CitpWsaInit {
    WSADATA w{};
    CitpWsaInit()  { WSAStartup(MAKEWORD(2, 2), &w); }
    ~CitpWsaInit() { WSACleanup(); }
};
static CitpWsaInit g_citp_wsa;

inline void citp_close_sock(CitpSocket s) { closesocket(s); }
inline int  citp_last_error()             { return WSAGetLastError(); }

// Returns the IPv4 address of the best outbound interface for CITP multicast,
// in network byte order.  Uses the "connect trick": open a throw-away UDP socket,
// connect() to the CITP multicast group (no data is sent), then getsockname()
// to read which local IP the OS bound for that route.  This is reliable on
// Windows even when Hyper-V / VirtualBox virtual adapters are present.
static DWORD get_best_interface_ip() {
    SOCKET probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) return htonl(INADDR_ANY);

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(4809);
    ::inet_pton(AF_INET, "239.224.0.180", &remote.sin_addr);

    int r = ::connect(probe, reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    if (r != 0) { ::closesocket(probe); return htonl(INADDR_ANY); }

    sockaddr_in local{};
    int len = sizeof(local);
    ::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &len);
    ::closesocket(probe);

    // If the OS returned INADDR_ANY, no specific route — fall back gracefully.
    if (local.sin_addr.s_addr == htonl(INADDR_ANY) ||
        local.sin_addr.s_addr == 0)
        return htonl(INADDR_ANY);

    return local.sin_addr.s_addr; // already in network byte order
}
} // anonymous namespace
#else
#include <unistd.h>
#include <fcntl.h>
inline void citp_close_sock(CitpSocket s) { ::close(s); }
inline int  citp_last_error()             { return errno; }
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: write a little-endian uint16 into a byte buffer
// ─────────────────────────────────────────────────────────────────────────────
static void write_le16(uint8_t* dst, uint16_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

static void write_le32(uint8_t* dst, uint32_t v) {
    dst[0] = static_cast<uint8_t>(v & 0xFFu);
    dst[1] = static_cast<uint8_t>((v >>  8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
CitpCaexDac::CitpCaexDac(int stream_idx) {
    // Build per-stream identity so Capture sees each Laser output as a separate source.
    // 1-based naming so "Laser 1" in the Outputs panel → "IDHMFIS 1" in Capture
    source_name_ = "IDHMFIS " + std::to_string(stream_idx + 1);
    tcp_port_    = static_cast<uint16_t>(kCitpTcpPort + stream_idx);

    // Generate a random source key, stable for the lifetime of this object
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dist;
    source_key_ = dist(gen);

    log::info("CITP/CAEX[%s]: source key = 0x%08X, TCP port = %u",
              source_name_.c_str(), source_key_, static_cast<unsigned>(tcp_port_));
}

CitpCaexDac::~CitpCaexDac() {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDac: open
// ─────────────────────────────────────────────────────────────────────────────
bool CitpCaexDac::open() {
    {
        std::lock_guard<std::mutex> lk(state_mutex_);
        if (open_.load()) return true;
    }

    // Fix C: Best-effort Windows Firewall rule so Capture can reach us.
    // Runs once; harmless if the rule already exists or on non-Windows builds.
    // Uses CreateProcessW with CREATE_NO_WINDOW so no cmd window is flashed.
#ifdef _WIN32
    {
        // Helper: run a wide command string in a hidden, no-console process.
        // Returns immediately; we do not wait for the child — best-effort.
        auto run_hidden = [](const wchar_t* cmd_line) {
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            // Do NOT set STARTF_USESTDHANDLES with null handles — passing null
            // handle values causes CreateProcessW to fail or the child process
            // (netsh) to exit immediately with an invalid-handle error.
            // CREATE_NO_WINDOW alone is sufficient to suppress any console window.

            PROCESS_INFORMATION pi{};
            // Writable copy required by CreateProcessW
            wchar_t buf[1024]{};
            wcsncpy_s(buf, cmd_line, _TRUNCATE);

            if (CreateProcessW(
                    nullptr,          // no explicit exe path
                    buf,              // command line
                    nullptr,          // process security
                    nullptr,          // thread security
                    FALSE,            // don't inherit handles
                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                    nullptr,          // inherit environment
                    nullptr,          // current directory (inherit)
                    &si,
                    &pi)) {
                // We don't need to wait — just fire and forget.
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        };

        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);

        // Delete stale rule first (ignore errors).
        run_hidden(L"netsh advfirewall firewall delete rule name=\"IDHMFIS CITP\"");

        // Add a fresh allow rule for this executable.
        wchar_t add_cmd[1024]{};
        _snwprintf_s(add_cmd, _countof(add_cmd), _TRUNCATE,
            L"netsh advfirewall firewall add rule"
            L" name=\"IDHMFIS CITP\""
            L" dir=in action=allow protocol=any"
            L" program=\"%s\""
            L" enable=yes profile=any",
            exe_path);
        run_hidden(add_cmd);

        char exe_path_a[MAX_PATH]{};
        GetModuleFileNameA(nullptr, exe_path_a, MAX_PATH);
        log::info("CITP/CAEX: requested firewall rule for '%s'", exe_path_a);
    }
#endif

    if (!open_udp_socket()) {
        log::error("CITP/CAEX: failed to open UDP socket");
        return false;
    }

    if (!open_tcp_server()) {
        log::error("CITP/CAEX: failed to open TCP server");
        close_socket(udp_fd_);
        return false;
    }

    running_.store(true, std::memory_order_release);
    open_.store(true, std::memory_order_release);

    announce_thread_   = std::thread([this]{ announce_thread_fn(); });
    tcp_server_thread_ = std::thread([this]{ tcp_server_thread_fn(); });

    log::info("CITP/CAEX[%s]: started — TCP port %u, multicast %s:%d",
              source_name_.c_str(), static_cast<unsigned>(tcp_port_),
              kCitpMcast1, kCitpMcastPort);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDac: close
// ─────────────────────────────────────────────────────────────────────────────
void CitpCaexDac::close() {
    if (!open_.exchange(false)) return;

    running_.store(false, std::memory_order_release);

    // Force blocking accept() to unblock by closing the listen socket
    {
        std::lock_guard<std::mutex> lk(socket_mutex_);
        if (tcp_listen_fd_ != kCitpInvalidSocket) {
            citp_close_sock(tcp_listen_fd_);
            tcp_listen_fd_ = kCitpInvalidSocket;
        }
        if (tcp_client_fd_ != kCitpInvalidSocket) {
            citp_close_sock(tcp_client_fd_);
            tcp_client_fd_ = kCitpInvalidSocket;
        }
        if (udp_fd_ != kCitpInvalidSocket) {
            citp_close_sock(udp_fd_);
            udp_fd_ = kCitpInvalidSocket;
        }
    }

    if (announce_thread_.joinable())    announce_thread_.join();
    if (tcp_server_thread_.joinable())  tcp_server_thread_.join();

    client_connected_.store(false);
    requested_fps_.store(0);
    frame_seq_.store(0);

    log::info("CITP/CAEX: closed");
}

bool CitpCaexDac::is_open() const {
    return open_.load(std::memory_order_acquire);
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDac: status
// ─────────────────────────────────────────────────────────────────────────────
DacStatus CitpCaexDac::status() const {
    std::lock_guard<std::mutex> lk(state_mutex_);
    DacStatus s;
    s.connected      = open_.load();
    s.point_rate     = point_rate_;
    s.buffer_free    = 4096;
    s.device_name    = "CITP/CAEX (Capture laser feed)";
    s.driver_version = "citp-1.0";
    if (client_connected_.load())
        s.device_name += " [client connected]";
    return s;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDac: set_point_rate
// ─────────────────────────────────────────────────────────────────────────────
bool CitpCaexDac::set_point_rate(int pps) {
    if (pps < min_point_rate() || pps > max_point_rate()) return false;
    std::lock_guard<std::mutex> lk(state_mutex_);
    point_rate_ = pps;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDac: send_points
//
//  Called by the output thread at the render rate.  We only emit a network
//  packet if Capture has requested streaming (requested_fps_ > 0).
// ─────────────────────────────────────────────────────────────────────────────
int CitpCaexDac::send_points(const PointBuffer& pts) {
    if (!open_.load(std::memory_order_acquire)) return 0;
    uint8_t fps = requested_fps_.load();
    if (fps == 0) return 0;

    // Rate-limit to what Capture requested. The engine output thread calls
    // send_points() at its own render rate (up to ~1 kHz). Without this cap
    // we'd flood Capture with hundreds of frames per second, overwhelming its
    // receive buffer and potentially causing it to drop or misclassify the feed.
    {
        using Clock = std::chrono::steady_clock;
        int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count();
        int64_t interval_us = 1000000LL / fps; // e.g. 40000 us for 25fps
        int64_t last = last_frame_us_.load(std::memory_order_relaxed);
        if (now_us - last < interval_us) return 0;
        last_frame_us_.store(now_us, std::memory_order_relaxed);
    }

    // When no show is playing, send a single blanked point so Capture keeps
    // the feed alive without projecting anything visible.
    PointBuffer idle_buf;
    const PointBuffer* effective = &pts;
    if (pts.empty()) {
        LaserPoint p{};
        p.blanked = true;
        idle_buf.push_back(p);
        effective = &idle_buf;
    }

    std::vector<uint8_t> pkt = build_frame(*effective);
    if (pkt.empty()) return 0;

    {
        std::lock_guard<std::mutex> lk(socket_mutex_);
        if (udp_fd_ == kCitpInvalidSocket) return 0;

        int pkt_len = static_cast<int>(pkt.size());
        const char* data = reinterpret_cast<const char*>(pkt.data());

        // 1. Multicast to both CITP groups (primary + legacy)
        ::sendto(udp_fd_, data, pkt_len, 0,
                 reinterpret_cast<const sockaddr*>(&mcast_addr1_),
                 sizeof(mcast_addr1_));
        ::sendto(udp_fd_, data, pkt_len, 0,
                 reinterpret_cast<const sockaddr*>(&mcast_addr2_),
                 sizeof(mcast_addr2_));

        // 2. Unicast to the connected Capture client — guarantees delivery on
        //    same-host setups where Windows multicast loopback is unreliable.
        uint32_t client_ip = client_ip_net_.load(std::memory_order_acquire);
        if (client_ip != 0) {
            sockaddr_in uni{};
            uni.sin_family      = AF_INET;
            uni.sin_port        = htons(kCitpMcastPort);
            uni.sin_addr.s_addr = client_ip;
            ::sendto(udp_fd_, data, pkt_len, 0,
                     reinterpret_cast<const sockaddr*>(&uni), sizeof(uni));
        }

        // 3. Unicast to loopback as additional fallback
        ::sendto(udp_fd_, data, pkt_len, 0,
                 reinterpret_cast<const sockaddr*>(&unicast_addr_),
                 sizeof(unicast_addr_));
    }

    // Log first frame and every 250th thereafter (~10s at 25fps)
    uint32_t seq = frame_seq_.load(std::memory_order_relaxed);
    if (seq == 1 || seq % 250 == 0) {
        log::info("CITP/CAEX: FeedFrame seq=%-6u pts=%-4zu  (%zu bytes)%s",
                  seq, effective->size(), pkt.size(),
                  effective != &pts ? " [idle pattern]" : "");
    }

    return static_cast<int>(pts.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Socket setup: UDP multicast sender
// ─────────────────────────────────────────────────────────────────────────────
bool CitpCaexDac::open_udp_socket() {
    CitpSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kCitpInvalidSocket) {
        log::error("CITP/CAEX: socket(UDP) failed (%d)", citp_last_error());
        return false;
    }

    // Allow multiple sockets on the same port (for receive on same machine)
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Set multicast TTL
    int ttl = kCitpMcastTTL;
    if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL,
                   reinterpret_cast<const char*>(&ttl), sizeof(ttl)) != 0) {
        log::warn("CITP/CAEX: IP_MULTICAST_TTL failed (%d)", citp_last_error());
    }

    // Bind multicast output to the correct physical interface.
    // On Windows with multiple NICs (Hyper-V, VirtualBox, etc.), INADDR_ANY
    // causes multicast to be sent on whichever interface the OS picks first —
    // which is often a virtual adapter, not the real LAN NIC.  We ask the
    // routing table which interface has the best route to the CITP multicast
    // subnet and pin both IP_MULTICAST_IF and IP_ADD_MEMBERSHIP to that IP.
    in_addr iface{};
#ifdef _WIN32
    iface.s_addr = get_best_interface_ip();
    {
        // Log the selected interface for diagnostics
        char iface_str[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &iface, iface_str, sizeof(iface_str));
        log::info("CITP/CAEX: multicast outbound interface = %s", iface_str);
    }
#else
    iface.s_addr = htonl(INADDR_ANY);
#endif

    if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF,
                   reinterpret_cast<const char*>(&iface), sizeof(iface)) != 0) {
        log::warn("CITP/CAEX: IP_MULTICAST_IF failed (%d)", citp_last_error());
    }

    // Enable multicast loopback so Capture on the same machine receives
    // our own multicast transmissions.
#ifdef _WIN32
    DWORD loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loop), sizeof(loop));
#else
    int loop = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP,
               reinterpret_cast<const char*>(&loop), sizeof(loop));
#endif

    // Build destination addresses
    std::memset(&mcast_addr1_, 0, sizeof(mcast_addr1_));
    mcast_addr1_.sin_family = AF_INET;
    mcast_addr1_.sin_port   = htons(kCitpMcastPort);
    ::inet_pton(AF_INET, kCitpMcast1, &mcast_addr1_.sin_addr);

    std::memset(&mcast_addr2_, 0, sizeof(mcast_addr2_));
    mcast_addr2_.sin_family = AF_INET;
    mcast_addr2_.sin_port   = htons(kCitpMcastPort);
    ::inet_pton(AF_INET, kCitpMcast2, &mcast_addr2_.sin_addr);

    // Join both multicast groups on the selected interface so the OS routes
    // multicast traffic correctly (Windows requires the sender to join as well
    // for local delivery, and the interface must match the outbound NIC).
    ip_mreq mreq{};
    mreq.imr_interface = iface; // use same interface as IP_MULTICAST_IF

    ::inet_pton(AF_INET, kCitpMcast1, &mreq.imr_multiaddr);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        log::warn("CITP/CAEX: IP_ADD_MEMBERSHIP(%s) failed (%d)",
                  kCitpMcast1, citp_last_error());
    }

    ::inet_pton(AF_INET, kCitpMcast2, &mreq.imr_multiaddr);
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&mreq), sizeof(mreq)) != 0) {
        log::warn("CITP/CAEX: IP_ADD_MEMBERSHIP(%s) failed (%d)",
                  kCitpMcast2, citp_last_error());
    }

    // Build a unicast destination for the local machine IP so PLoc is also sent
    // directly — guarantees same-host delivery even if multicast loopback fails.
    std::memset(&unicast_addr_, 0, sizeof(unicast_addr_));
    unicast_addr_.sin_family = AF_INET;
    unicast_addr_.sin_port   = htons(kCitpMcastPort);
    if (iface.s_addr != htonl(INADDR_ANY) && iface.s_addr != 0) {
        unicast_addr_.sin_addr = iface;
    } else {
        // Fall back to loopback if we couldn't determine the real interface IP.
        ::inet_pton(AF_INET, "127.0.0.1", &unicast_addr_.sin_addr);
    }

    std::lock_guard<std::mutex> lk(socket_mutex_);
    udp_fd_ = s;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Socket setup: TCP server
// ─────────────────────────────────────────────────────────────────────────────
bool CitpCaexDac::open_tcp_server() {
    CitpSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kCitpInvalidSocket) {
        log::error("CITP/CAEX: socket(TCP) failed (%d)", citp_last_error());
        return false;
    }

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(tcp_port_);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::error("CITP/CAEX[%s]: bind(TCP:%u) failed (%d)",
                   source_name_.c_str(), static_cast<unsigned>(tcp_port_),
                   citp_last_error());
        citp_close_sock(s);
        return false;
    }

    if (::listen(s, 4) != 0) {
        log::error("CITP/CAEX: listen() failed (%d)", citp_last_error());
        citp_close_sock(s);
        return false;
    }

    std::lock_guard<std::mutex> lk(socket_mutex_);
    tcp_listen_fd_ = s;
    return true;
}

void CitpCaexDac::close_socket(CitpSocket& s) {
    if (s != kCitpInvalidSocket) {
        citp_close_sock(s);
        s = kCitpInvalidSocket;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  announce_thread_fn — send PLoc beacon every 1 second
// ─────────────────────────────────────────────────────────────────────────────
void CitpCaexDac::announce_thread_fn() {
    log::info("CITP/CAEX: announce thread started");

    while (running_.load(std::memory_order_acquire)) {
        std::vector<uint8_t> pkt = build_ploc();

        {
            std::lock_guard<std::mutex> lk(socket_mutex_);
            if (udp_fd_ != kCitpInvalidSocket) {
                int pkt_len = static_cast<int>(pkt.size());
                const char* data = reinterpret_cast<const char*>(pkt.data());

                // Standard CITP multicast beacons (239.224.0.180 = current,
                // 224.0.0.180 = legacy pre-2014)
                ::sendto(udp_fd_, data, pkt_len, 0,
                         reinterpret_cast<const sockaddr*>(&mcast_addr1_),
                         sizeof(mcast_addr1_));
                ::sendto(udp_fd_, data, pkt_len, 0,
                         reinterpret_cast<const sockaddr*>(&mcast_addr2_),
                         sizeof(mcast_addr2_));

                // Unicast to local machine IP — guarantees delivery when both
                // IDHMFIS and Capture run on the same Windows PC (multicast
                // loopback is unreliable across processes on Windows).
                ::sendto(udp_fd_, data, pkt_len, 0,
                         reinterpret_cast<const sockaddr*>(&unicast_addr_),
                         sizeof(unicast_addr_));
            }
        }

        // Sleep ~1 s in 100 ms slices for clean shutdown
        for (int i = 0; i < 10 && running_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    log::info("CITP/CAEX: announce thread exiting");
}

// ─────────────────────────────────────────────────────────────────────────────
//  tcp_server_thread_fn — accept one client at a time
// ─────────────────────────────────────────────────────────────────────────────
void CitpCaexDac::tcp_server_thread_fn() {
    log::info("CITP/CAEX[%s]: TCP server thread started on port %u",
              source_name_.c_str(), static_cast<unsigned>(tcp_port_));

    while (running_.load(std::memory_order_acquire)) {
        CitpSocket listen_fd;
        {
            std::lock_guard<std::mutex> lk(socket_mutex_);
            listen_fd = tcp_listen_fd_;
        }
        if (listen_fd == kCitpInvalidSocket) break;

        // Use select() with a short timeout so we can notice running_ becoming false
#ifdef _WIN32
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listen_fd, &rset);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200 * 1000; // 200 ms
        int sel = ::select(0, &rset, nullptr, nullptr, &tv);
#else
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listen_fd, &rset);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 200 * 1000;
        int sel = ::select(static_cast<int>(listen_fd) + 1,
                           &rset, nullptr, nullptr, &tv);
#endif
        if (sel <= 0) continue; // timeout or error — loop to check running_

        sockaddr_in peer{};
#ifdef _WIN32
        int peerlen = sizeof(peer);
#else
        socklen_t peerlen = sizeof(peer);
#endif
        CitpSocket client = ::accept(listen_fd,
                                     reinterpret_cast<sockaddr*>(&peer),
                                     &peerlen);

        if (client == kCitpInvalidSocket) {
            if (!running_.load(std::memory_order_acquire)) break;
            continue;
        }

        char peer_ip[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        log::info("CITP/CAEX: Capture connected from %s", peer_ip);
        client_ip_net_.store(peer.sin_addr.s_addr, std::memory_order_release);

        // Set a 2-second receive timeout on the client socket so the inner
        // header/body recv loops cannot deadlock if the stream goes silent.
#ifdef _WIN32
        DWORD rcv_timeout_ms = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcv_timeout_ms),
                   sizeof(rcv_timeout_ms));
#else
        struct timeval rcv_tv{ 2, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&rcv_tv), sizeof(rcv_tv));
#endif

        {
            std::lock_guard<std::mutex> lk(socket_mutex_);
            tcp_client_fd_ = client;
        }
        client_connected_.store(true);

        handle_client(client);

        {
            std::lock_guard<std::mutex> lk(socket_mutex_);
            if (tcp_client_fd_ == client) {
                citp_close_sock(tcp_client_fd_);
                tcp_client_fd_ = kCitpInvalidSocket;
            }
        }
        client_connected_.store(false);
        requested_fps_.store(0);
        client_ip_net_.store(0, std::memory_order_release);

        log::info("CITP/CAEX: client %s disconnected", peer_ip);
    }

    log::info("CITP/CAEX: TCP server thread exiting");
}

// ─────────────────────────────────────────────────────────────────────────────
//  handle_client — runs on tcp_server_thread_ for the duration of a connection
//
//  Protocol:
//    1. Send PNam (our name)
//    2. Receive PNam from Capture (log and ignore)
//    3. Receive GetLaserFeedList → send LaserFeedList
//    4. Receive LaserFeedControl → update requested_fps_
//    5. Repeat from 3 until disconnected
// ─────────────────────────────────────────────────────────────────────────────
void CitpCaexDac::handle_client(CitpSocket client) {
    // 1. Send PNam
    {
        std::vector<uint8_t> pnam = build_pnam();
        int r = static_cast<int>(
            ::send(client,
                   reinterpret_cast<const char*>(pnam.data()),
                   static_cast<int>(pnam.size()), 0));
        if (r <= 0) {
            log::warn("CITP/CAEX: send PNam failed (%d)", citp_last_error());
            return;
        }
    }

    // 2. Proactively announce our feed list immediately after PNam.
    //    Per spec, the source SHOULD push FeedList on connect so Capture can
    //    present the feed to the user without waiting for GetLaserFeedList.
    {
        std::vector<uint8_t> feed_list = build_feed_list();
        int r = static_cast<int>(
            ::send(client,
                   reinterpret_cast<const char*>(feed_list.data()),
                   static_cast<int>(feed_list.size()), 0));
        if (r <= 0)
            log::warn("CITP/CAEX: proactive FeedList send failed (%d)", citp_last_error());
        else
            log::info("CITP/CAEX: sent proactive LaserFeedList (%zu bytes)", feed_list.size());
    }

    // Receive loop — read a CITP header (20 bytes), then dispatch.
    while (running_.load(std::memory_order_acquire)) {
        // Use select() with 500 ms timeout for clean shutdown
#ifdef _WIN32
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(client, &rset);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 500 * 1000;
        int sel = ::select(0, &rset, nullptr, nullptr, &tv);
#else
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(client, &rset);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 500 * 1000;
        int sel = ::select(static_cast<int>(client) + 1,
                           &rset, nullptr, nullptr, &tv);
#endif
        if (sel == 0) continue; // timeout
        if (sel < 0)  break;    // error

        // Read the CITP header (20 bytes)
        CitpHeader hdr{};
        int recvd = 0;
        while (recvd < static_cast<int>(sizeof(hdr))) {
            int r = static_cast<int>(
                ::recv(client,
                       reinterpret_cast<char*>(&hdr) + recvd,
                       static_cast<int>(sizeof(hdr)) - recvd, 0));
            if (r <= 0) goto client_done; // connection closed or error
            recvd += r;
        }

        if (hdr.Cookie != kCitpCookie) {
            log::warn("CITP/CAEX: bad cookie 0x%08X", hdr.Cookie);
            break;
        }

        // Read the rest of the message (MessageSize - sizeof(CitpHeader) bytes)
        uint32_t total_size = hdr.MessageSize;
        if (total_size < sizeof(CitpHeader)) {
            log::warn("CITP/CAEX: MessageSize %u too small", total_size);
            break;
        }
        // BUG #68: Reject packets whose declared size would cause an enormous
        // allocation (e.g. a malformed/malicious peer sending 4 GB).
        if (total_size > 1024u * 1024u) { // 1 MB cap
            log::warn("CITP/CAEX: packet too large (%u bytes), dropping", total_size);
            break;
        }

        uint32_t body_size = total_size - static_cast<uint32_t>(sizeof(CitpHeader));
        std::vector<uint8_t> body(body_size);
        uint32_t body_recvd = 0;
        while (body_recvd < body_size) {
            int r = static_cast<int>(
                ::recv(client,
                       reinterpret_cast<char*>(body.data()) + body_recvd,
                       static_cast<int>(body_size - body_recvd), 0));
            if (r <= 0) goto client_done;
            body_recvd += static_cast<uint32_t>(r);
        }

        // Dispatch on ContentType
        if (hdr.ContentType == kCitpPinfCookie) {
            // Should be PNam from Capture — read the sub content type (4 bytes)
            if (body_size >= 4) {
                uint32_t sub = 0;
                std::memcpy(&sub, body.data(), 4);
                if (sub == kCitpPNamCode) {
                    // Read Capture's name — it follows the sub content type.
                    // BUG #67: body is received from the network and may not be
                    // null-terminated.  Append a null byte before treating as C-string.
                    if (body_size > 4) {
                        body.push_back(0); // ensure null termination
                        const char* cname = reinterpret_cast<const char*>(body.data() + 4);
                        log::info("CITP/CAEX: peer name = '%s'", cname);
                    }
                }
            }
        } else if (hdr.ContentType == kCitpCaexCookie) {
            // Need at least 4 bytes for ContentCode
            if (body_size < 4) {
                log::warn("CITP/CAEX: CAEX body too short (%u)", body_size);
                break;
            }
            uint32_t code = 0;
            std::memcpy(&code, body.data(), 4);

            if (code == kCaexGetFeedList) {
                // Reply with LaserFeedList
                std::vector<uint8_t> feed_list = build_feed_list();
                int r = static_cast<int>(
                    ::send(client,
                           reinterpret_cast<const char*>(feed_list.data()),
                           static_cast<int>(feed_list.size()), 0));
                if (r <= 0) {
                    log::warn("CITP/CAEX: send LaserFeedList failed (%d)",
                              citp_last_error());
                    goto client_done;
                }
                log::info("CITP/CAEX: sent LaserFeedList (triggered by GetLaserFeedList)");
            } else if (code == kCaexFeedControl || code == kCaexFeedControl2) {
                // LaserFeedControl.  Capture 2024 uses code 0x00030102, the CITP
                // spec rev F defines 0x00030108; we accept both.
                //
                // Two wire formats observed:
                //   Compact (Capture 2024): ContentCode(4) + FeedIndex(1) + FPS(1) — body 6 bytes
                //   Extended (spec rev F):  ContentCode(4) + SourceKey(4) + FeedIndex(1) + FPS(1) — body 10 bytes
                uint8_t feed_idx   = 0;
                uint8_t frame_rate = 0;
                if (body_size >= 10) {
                    uint32_t src_key = 0;
                    std::memcpy(&src_key, body.data() + 4, 4);
                    feed_idx   = body[8];
                    frame_rate = body[9];
                    log::info("CITP/CAEX: FeedControl(0x%08X) src=0x%08X feed=%u fps=%u",
                              code, src_key, feed_idx, frame_rate);
                } else if (body_size >= 6) {
                    feed_idx   = body[4];
                    frame_rate = body[5];
                    log::info("CITP/CAEX: FeedControl(0x%08X) feed=%u fps=%u",
                              code, feed_idx, frame_rate);
                } else {
                    log::warn("CITP/CAEX: FeedControl body too short (%u bytes)", body_size);
                }
                if (feed_idx == 0) {
                    requested_fps_.store(frame_rate);
                    if (frame_rate > 0)
                        log::info("CITP/CAEX: streaming started at %u fps", frame_rate);
                    else
                        log::info("CITP/CAEX: streaming stopped (fps=0)");
                }
            } else {
                log::info("CITP/CAEX: unknown CAEX code 0x%08X (body %u bytes)",
                          code, body_size);
            }
        } else if (hdr.ContentType == kCitpMsexCookie) {
            // MSEX message layout: uint8 VersionMajor + uint8 VersionMinor + uint32 ContentCode
            // ContentCode is at byte offset 2 (after the two version bytes).
            if (body_size >= 6) {
                uint8_t  msex_ver_major = body[0];
                uint8_t  msex_ver_minor = body[1];
                uint32_t sub = 0;
                std::memcpy(&sub, body.data() + 2, 4);
                if (sub == kMsexCInfCode) {
                    // Capture sends CInf on every connection. We respond with SInf
                    // declaring zero media libraries so Capture marks our feeds as
                    // laser inputs rather than generic media server outputs.
                    std::vector<uint8_t> sinf = build_sinf(msex_ver_major, msex_ver_minor);
                    int r = static_cast<int>(
                        ::send(client,
                               reinterpret_cast<const char*>(sinf.data()),
                               static_cast<int>(sinf.size()), 0));
                    if (r <= 0)
                        log::warn("CITP/MSEX: SInf send failed (%d)", citp_last_error());
                    else
                        log::info("CITP/MSEX: sent SInf 1.1 (CInf was v%u.%u, %zu bytes sent)",
                                  msex_ver_major, msex_ver_minor, sinf.size());
                } else {
                    log::info("CITP/MSEX: ignoring sub code 0x%08X v%u.%u (body %u bytes)",
                              sub, msex_ver_major, msex_ver_minor, body_size);
                }
            }
        } else {
            log::info("CITP/CAEX: unknown ContentType 0x%08X (size %u)",
                      hdr.ContentType, hdr.MessageSize);
        }
    }

    client_done:;
}

// ─────────────────────────────────────────────────────────────────────────────
//  fill_citp_header
// ─────────────────────────────────────────────────────────────────────────────
void CitpCaexDac::fill_citp_header(CitpHeader& h, uint32_t content_type,
                                    uint32_t total_size) {
    h.Cookie            = kCitpCookie;
    h.VersionMajor      = 1;
    h.VersionMinor      = 0;
    h.RequestIndex      = 0;
    h.MessageSize       = total_size;
    h.MessagePartCount  = 1;
    h.MessagePart       = 0;
    h.ContentType       = content_type;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_ploc — PINF/PLoc beacon
//
//  Layout (after the PINF header):
//    uint16_t ListeningTCPPort
//    char[]   Type   "MediaServer\0"  (null-terminated ASCII/UTF-8)
//    char[]   Name   "IDHMFIS\0"      (null-terminated ASCII/UTF-8)
//    char[]   State  "Running\0"      (null-terminated ASCII/UTF-8)
//
//  Valid Type values per CITP base spec: "LightingConsole", "MediaServer",
//  "Visualiser".  There is NO "LaserController" type — laser sources must
//  advertise as "MediaServer" for Capture to accept the beacon.
//  Strings are null-terminated UTF-8 (single byte per char), NOT UTF-16 LE.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CitpCaexDac::build_ploc() const {
    static constexpr char kType[]  = "MediaServer";
    static constexpr char kState[] = "Running";

    // +1 for null terminator on each string
    static constexpr uint32_t kTypeLen  = sizeof(kType);
    static constexpr uint32_t kStateLen = sizeof(kState);

    // source_name_ is unique per stream index — Capture uses it to distinguish sources.
    uint32_t kNameLen = static_cast<uint32_t>(source_name_.size()) + 1u; // +1 for null

    uint32_t msg_size = static_cast<uint32_t>(sizeof(CitpPinfHeader)) +
                        sizeof(uint16_t) +
                        kTypeLen + kNameLen + kStateLen;

    std::vector<uint8_t> buf(msg_size, 0);
    uint8_t* p = buf.data();

    // CITP base header
    auto* ch = reinterpret_cast<CitpHeader*>(p);
    fill_citp_header(*ch, kCitpPinfCookie, msg_size);
    p += sizeof(CitpHeader);

    // PINF sub content type = PLoc
    write_le32(p, kCitpPLocCode);
    p += 4;

    // Listening TCP port — unique per stream index
    write_le16(p, tcp_port_);
    p += 2;

    // Type (ASCII, null-terminated)
    std::memcpy(p, kType, kTypeLen);
    p += kTypeLen;

    // Name (ASCII, null-terminated) — unique per stream index
    std::memcpy(p, source_name_.c_str(), kNameLen);
    p += kNameLen;

    // State (ASCII, null-terminated)
    std::memcpy(p, kState, kStateLen);
    p += kStateLen;

    assert(p == buf.data() + msg_size);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_pnam — PINF/PNam peer name message
//
//  Layout (after the PINF header):
//    char[] Name "IDHMFIS\0"  (null-terminated ASCII/UTF-8)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CitpCaexDac::build_pnam() const {
    uint32_t kNameLen = static_cast<uint32_t>(source_name_.size()) + 1u; // +1 for null

    uint32_t msg_size = static_cast<uint32_t>(sizeof(CitpPinfHeader)) + kNameLen;

    std::vector<uint8_t> buf(msg_size, 0);
    uint8_t* p = buf.data();

    auto* ch = reinterpret_cast<CitpHeader*>(p);
    fill_citp_header(*ch, kCitpPinfCookie, msg_size);
    p += sizeof(CitpHeader);

    write_le32(p, kCitpPNamCode);
    p += 4;

    std::memcpy(p, source_name_.c_str(), kNameLen);
    p += kNameLen;

    assert(p == buf.data() + msg_size);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_sinf — MSEX/SInf server information response
//
//  Tells Capture we support MSEX 2.0 but have zero media libraries.
//  This causes Capture to classify our CAEX feeds as laser inputs rather
//  than treating us as a generic media server.
//
//  Layout (after CitpHeader):
//    uint32_t ContentCode          "SInf"
//    uint8_t  MsexVersionMajor     2
//    uint8_t  MsexVersionMinor     0
//    uint8_t  SupportedVersionsCount 1
//    uint8_t  Version[0].Major     2
//    uint8_t  Version[0].Minor     0
//    uint16_t SupportedLibraryTypes  0  (no media libraries)
//    uint8_t  ThumbnailFormatsCount  0
//    uint8_t  StreamFormatsCount     0
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CitpCaexDac::build_sinf(uint8_t ver_major, uint8_t ver_minor) const {
    // Always respond at MSEX 1.1 level regardless of what Capture sent.
    // MSEX 1.1 has SupportedLibraryTypes (key for laser-vs-media classification)
    // but does NOT have the MSEX 1.2 LayerCount field, so the size is well-defined.
    //
    // Layout:
    //   uint8  MsexVersionMajor       1
    //   uint8  MsexVersionMinor       1
    //   uint32 ContentCode            "SInf"
    //   uint8  SupportedVersionsCount 1
    //   uint8  Version[0].Major       1
    //   uint8  Version[0].Minor       1
    //   uint16 SupportedLibraryTypes  0  (no media — laser-only)
    //   uint8  ThumbnailFormatsCount  0
    //   uint8  StreamFormatsCount     0
    // Total payload = 2+4+1+2+2+1+1 = 13 bytes
    (void)ver_major; (void)ver_minor; // respond at 1.1 regardless

    constexpr uint32_t kPayload = 13u;
    uint32_t msg_size = static_cast<uint32_t>(sizeof(CitpHeader)) + kPayload;

    std::vector<uint8_t> buf(msg_size, 0);
    uint8_t* p = buf.data();

    auto* ch = reinterpret_cast<CitpHeader*>(p);
    fill_citp_header(*ch, kCitpMsexCookie, msg_size);
    p += sizeof(CitpHeader);

    *p++ = 1;                      // MsexVersionMajor = 1
    *p++ = 1;                      // MsexVersionMinor = 1  (MSEX 1.1)
    write_le32(p, kMsexSInfCode);  p += 4;  // ContentCode "SInf"
    *p++ = 1;                      // SupportedVersionsCount = 1
    *p++ = 1;                      // Version[0].Major
    *p++ = 1;                      // Version[0].Minor
    write_le16(p, 0); p += 2;     // SupportedLibraryTypes = 0 (no media, laser-only)
    *p++ = 0;                      // ThumbnailFormatsCount
    *p++ = 0;                      // StreamFormatsCount

    assert(p == buf.data() + msg_size);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_feed_list — CAEX/LaserFeedList response
//
//  Layout (after the 24-byte CAEX header):
//    uint32_t SourceKey
//    uint8_t  FeedCount (1)
//    // 1 × null-terminated UTF-16LE name "IDHMFIS\0"
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CitpCaexDac::build_feed_list() const {
    // Encode source_name_ as UTF-16LE with a null terminator.
    // source_name_ is always pure ASCII so each char maps to a 2-byte LE codepoint.
    std::vector<uint8_t> feed_name_utf16;
    feed_name_utf16.reserve((source_name_.size() + 1u) * 2u);
    for (unsigned char c : source_name_) {
        feed_name_utf16.push_back(c);
        feed_name_utf16.push_back(0x00u);
    }
    // Null terminator
    feed_name_utf16.push_back(0x00u);
    feed_name_utf16.push_back(0x00u);

    uint32_t kFeedNameLen = static_cast<uint32_t>(feed_name_utf16.size());

    uint32_t body_size = sizeof(uint32_t) +  // SourceKey
                         sizeof(uint8_t)  +  // FeedCount
                         kFeedNameLen;

    uint32_t msg_size = static_cast<uint32_t>(sizeof(CitpCaexHeader)) + body_size;

    std::vector<uint8_t> buf(msg_size, 0);
    uint8_t* p = buf.data();

    // CITP base header
    auto* ch = reinterpret_cast<CitpHeader*>(p);
    fill_citp_header(*ch, kCitpCaexCookie, msg_size);
    p += sizeof(CitpHeader);

    // CAEX content code
    write_le32(p, kCaexFeedList);
    p += 4;

    // SourceKey
    write_le32(p, source_key_);
    p += 4;

    // FeedCount
    *p++ = 1u;

    // Feed name (UTF-16LE, unique per stream index)
    std::memcpy(p, feed_name_utf16.data(), kFeedNameLen);
    p += kFeedNameLen;

    assert(p == buf.data() + msg_size);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Coordinate and colour conversions
// ─────────────────────────────────────────────────────────────────────────────
uint16_t CitpCaexDac::ilda_to_caex(int16_t ilda) {
    // Map [-32768..32767] → [0..4095] (full 12-bit CAEX coordinate range)
    int32_t shifted = static_cast<int32_t>(ilda) + 32768;
    return static_cast<uint16_t>(shifted * 4095 / 65535);
}

uint16_t CitpCaexDac::rgb888_to_r5g6b5(uint8_t r, uint8_t g, uint8_t b) {
    // CAEX FeedFrame color is BGR565: B in bits[15:11], G in bits[10:5], R in bits[4:0]
    // (packed as R | (G<<5) | (B<<11) per CAEX spec rev F)
    uint16_t r5 = static_cast<uint16_t>(r >> 3);
    uint16_t g6 = static_cast<uint16_t>(g >> 2);
    uint16_t b5 = static_cast<uint16_t>(b >> 3);
    return static_cast<uint16_t>(b5 << 11) | static_cast<uint16_t>(g6 << 5) | r5;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_frame — CAEX/LaserFeedFrame
//
//  Layout (after the 24-byte CAEX header):
//    uint32_t SourceKey
//    uint8_t  FeedIndex (0)
//    uint32_t FrameSequenceNo
//    uint16_t PointCount
//    PointCount × CaexPoint (5 bytes each)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CitpCaexDac::build_frame(const PointBuffer& pts) {
    // BUG #32: Verify that CitpCaexHeader is exactly CitpHeader + 4 bytes
    // (the CAEX content-type code field) so pointer arithmetic in this function
    // is correct.  Mismatched struct packing would silently corrupt the wire format.
    static_assert(sizeof(CitpCaexHeader) == sizeof(CitpHeader) + sizeof(uint32_t),
        "CitpCaexHeader size mismatch — check struct packing");

    uint16_t count = static_cast<uint16_t>(
        std::min<size_t>(pts.size(), 0xFFFFu));

    uint32_t body_size = sizeof(uint32_t) +  // SourceKey
                         sizeof(uint8_t)  +  // FeedIndex
                         sizeof(uint32_t) +  // FrameSequenceNo
                         sizeof(uint16_t) +  // PointCount
                         static_cast<uint32_t>(count) * 5u;

    uint32_t msg_size = static_cast<uint32_t>(sizeof(CitpCaexHeader)) + body_size;

    std::vector<uint8_t> buf(msg_size, 0);
    uint8_t* p = buf.data();

    // CITP base header
    auto* ch = reinterpret_cast<CitpHeader*>(p);
    fill_citp_header(*ch, kCitpCaexCookie, msg_size);
    p += sizeof(CitpHeader);

    // CAEX content code
    write_le32(p, kCaexFeedFrame);
    p += 4;

    // SourceKey
    write_le32(p, source_key_);
    p += 4;

    // FeedIndex
    *p++ = 0u;

    // FrameSequenceNo (monotonic)
    uint32_t seq = frame_seq_.fetch_add(1, std::memory_order_relaxed);
    write_le32(p, seq);
    p += 4;

    // PointCount
    write_le16(p, count);
    p += 2;

    // Points
    for (uint16_t i = 0; i < count; ++i) {
        const LaserPoint& lp = pts[i];

        uint16_t cx = ilda_to_caex(lp.x);
        uint16_t cy = ilda_to_caex(lp.y);

        uint8_t x_low  = static_cast<uint8_t>(cx & 0xFFu);
        uint8_t y_low  = static_cast<uint8_t>(cy & 0xFFu);
        uint8_t xy_high = static_cast<uint8_t>((cx >> 8) & 0x0Fu) |
                          static_cast<uint8_t>((cy >> 4) & 0xF0u);

        uint16_t color = lp.blanked
            ? 0x0000u
            : rgb888_to_r5g6b5(lp.r, lp.g, lp.b);

        *p++ = x_low;
        *p++ = y_low;
        *p++ = xy_high;
        write_le16(p, color);
        p += 2;
    }

    assert(p == buf.data() + msg_size);
    return buf;
}

} // namespace idhmfis
