// common_laser_stream.cpp — CLS1 universal TCP/UDP laser frame streaming sidecar.
//
// TCP server (port 7256) + UDP broadcast (255.255.255.255:7256).
// Every send_points() call serialises the frame to CLS1 binary and delivers it to:
//   - All connected TCP clients (dropped/removed on send failure)
//   - UDP broadcast (best-effort; no flow control)
// A hello thread sends a discovery packet every 1 s so receivers can auto-detect.

#include "common_laser_stream.h"
#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
namespace {
struct WsaInitCls {
    WSADATA w;
    WsaInitCls()  { WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaInitCls() { WSACleanup(); }
};
static WsaInitCls g_wsa_cls;
inline void cls_close_sock(ClsSockFd s) { closesocket(s); }
inline int  cls_send(ClsSockFd s, const char* d, int n)
    { return static_cast<int>(::send(s, d, n, 0)); }
} // anonymous namespace
#else
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
namespace {
inline void cls_close_sock(ClsSockFd s) { ::close(s); }
inline int  cls_send(ClsSockFd s, const char* d, int n)
    { return static_cast<int>(::send(s, d, n, MSG_NOSIGNAL)); }
} // anonymous namespace
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
CommonLaserStream::CommonLaserStream() = default;
CommonLaserStream::~CommonLaserStream() { close(); }

// ─────────────────────────────────────────────────────────────────────────────
//  open
// ─────────────────────────────────────────────────────────────────────────────
bool CommonLaserStream::open() {
    std::lock_guard<std::mutex> lk(open_mutex_);
    if (open_) return true;

    // ── TCP listen socket ────────────────────────────────────────────────────
    listen_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == kClsInvalidSock) {
        log::error("CommonLaserStream: socket(TCP) failed");
        return false;
    }

    {
        int reuse = 1;
        ::setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(port_);
    bind_addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(listen_sock_,
               reinterpret_cast<sockaddr*>(&bind_addr),
               sizeof(bind_addr)) != 0) {
        log::error("CommonLaserStream: bind(TCP :%d) failed", port_);
        cls_close_sock(listen_sock_);
        listen_sock_ = kClsInvalidSock;
        return false;
    }

    if (::listen(listen_sock_, kClsMaxClients) != 0) {
        log::error("CommonLaserStream: listen() failed");
        cls_close_sock(listen_sock_);
        listen_sock_ = kClsInvalidSock;
        return false;
    }

    // ── UDP broadcast socket ─────────────────────────────────────────────────
    udp_sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock_ == kClsInvalidSock) {
        log::warn("CommonLaserStream: socket(UDP) failed — UDP broadcast disabled");
    } else {
        int bcast = 1;
        ::setsockopt(udp_sock_, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&bcast), sizeof(bcast));

        int sndbuf = 256 * 1024;
        ::setsockopt(udp_sock_, SOL_SOCKET, SO_SNDBUF,
                     reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

        std::memset(&bcast_addr_, 0, sizeof(bcast_addr_));
        bcast_addr_.sin_family      = AF_INET;
        bcast_addr_.sin_port        = htons(port_);
        bcast_addr_.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    }

    open_ = true;
    running_.store(true, std::memory_order_release);
    accept_thread_ = std::thread([this]{ accept_thread_fn(); });
    hello_thread_  = std::thread([this]{ hello_thread_fn(); });

    log::info("CommonLaserStream: open — TCP:%d + UDP broadcast", port_);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  close
// ─────────────────────────────────────────────────────────────────────────────
void CommonLaserStream::close() {
    running_.store(false, std::memory_order_release);

    // Unblock accept() by closing the listen socket before joining.
    {
        std::lock_guard<std::mutex> lk(open_mutex_);
        if (listen_sock_ != kClsInvalidSock) {
            cls_close_sock(listen_sock_);
            listen_sock_ = kClsInvalidSock;
        }
    }

    if (accept_thread_.joinable()) accept_thread_.join();
    if (hello_thread_.joinable())  hello_thread_.join();

    std::lock_guard<std::mutex> lk(open_mutex_);
    if (!open_) return;

    {
        std::lock_guard<std::mutex> clk(clients_mutex_);
        for (auto& c : clients_)
            if (c.sock != kClsInvalidSock) cls_close_sock(c.sock);
        clients_.clear();
    }

    if (udp_sock_ != kClsInvalidSock) {
        cls_close_sock(udp_sock_);
        udp_sock_ = kClsInvalidSock;
    }

    open_ = false;
    log::info("CommonLaserStream: closed");
}

// ─────────────────────────────────────────────────────────────────────────────
//  is_open / set_point_rate
// ─────────────────────────────────────────────────────────────────────────────
bool CommonLaserStream::is_open() const {
    std::lock_guard<std::mutex> lk(open_mutex_);
    return open_;
}

bool CommonLaserStream::set_point_rate(int pps) {
    if (pps <= 0) return false;
    std::lock_guard<std::mutex> lk(open_mutex_);
    pps_ = pps;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_points
// ─────────────────────────────────────────────────────────────────────────────
int CommonLaserStream::send_points(const PointBuffer& pts) {
    if (pts.empty()) return 0;
    {
        std::lock_guard<std::mutex> lk(open_mutex_);
        if (!open_) return 0;
    }

    using Clock = std::chrono::steady_clock;
    auto ts_us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count() & 0xFFFFFFFFu);

    int rate;
    { std::lock_guard<std::mutex> lk(open_mutex_); rate = pps_; }

    auto pkt = build_frame_packet(pts, ts_us, static_cast<uint32_t>(rate));
    send_to_tcp_clients(pkt);
    send_udp(pkt);

    return static_cast<int>(pts.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_frame_packet
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CommonLaserStream::build_frame_packet(const PointBuffer& pts,
                                                             uint32_t ts_us,
                                                             uint32_t point_rate) {
    const auto n = static_cast<uint32_t>(pts.size());
    const uint32_t body_len = static_cast<uint32_t>(sizeof(ClsFrameBody))
                            + n * static_cast<uint32_t>(sizeof(ClsPoint));

    std::vector<uint8_t> buf(sizeof(ClsPacketHeader) + body_len);
    uint8_t* cur = buf.data();

    auto* hdr = reinterpret_cast<ClsPacketHeader*>(cur);
    hdr->magic[0] = 'C'; hdr->magic[1] = 'L';
    hdr->magic[2] = 'S'; hdr->magic[3] = '1';
    hdr->type     = kClsTypeFrame;
    hdr->flags    = 0;
    hdr->body_len = body_len;
    cur += sizeof(ClsPacketHeader);

    auto* fb = reinterpret_cast<ClsFrameBody*>(cur);
    fb->timestamp_us = ts_us;
    fb->point_rate   = point_rate;
    fb->point_count  = n;
    cur += sizeof(ClsFrameBody);

    for (const auto& p : pts) {
        auto* cp = reinterpret_cast<ClsPoint*>(cur);
        cp->x     = p.x;
        cp->y     = p.y;
        cp->r     = p.r;
        cp->g     = p.g;
        cp->b     = p.b;
        cp->flags = p.blanked ? 0x01u : 0x00u;
        cur += sizeof(ClsPoint);
    }

    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_hello_packet
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint8_t> CommonLaserStream::build_hello_packet() {
    const uint32_t body_len = static_cast<uint32_t>(sizeof(ClsHelloBody));
    std::vector<uint8_t> buf(sizeof(ClsPacketHeader) + body_len, 0);

    auto* hdr = reinterpret_cast<ClsPacketHeader*>(buf.data());
    hdr->magic[0] = 'C'; hdr->magic[1] = 'L';
    hdr->magic[2] = 'S'; hdr->magic[3] = '1';
    hdr->type     = kClsTypeHello;
    hdr->flags    = 0;
    hdr->body_len = body_len;

    auto* hb = reinterpret_cast<ClsHelloBody*>(buf.data() + sizeof(ClsPacketHeader));
    hb->version = 1;
    std::strncpy(hb->name, "IDHMFIS", sizeof(hb->name) - 1);

    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_to_tcp_clients — distribute packet to all connected clients
// ─────────────────────────────────────────────────────────────────────────────
void CommonLaserStream::send_to_tcp_clients(const std::vector<uint8_t>& data) {
    if (data.empty()) return;

    std::lock_guard<std::mutex> lk(clients_mutex_);
    clients_.erase(
        std::remove_if(clients_.begin(), clients_.end(),
            [&](Client& c) -> bool {
                if (c.sock == kClsInvalidSock) return true;
                int total = static_cast<int>(data.size());
                int sent  = 0;
                while (sent < total) {
                    int r = cls_send(c.sock,
                                     reinterpret_cast<const char*>(data.data()) + sent,
                                     total - sent);
                    if (r <= 0) {
                        cls_close_sock(c.sock);
                        c.sock = kClsInvalidSock;
                        return true; // remove
                    }
                    sent += r;
                }
                return false;
            }),
        clients_.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_udp — broadcast packet via UDP
// ─────────────────────────────────────────────────────────────────────────────
void CommonLaserStream::send_udp(const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lk(open_mutex_);
    if (udp_sock_ == kClsInvalidSock || data.empty()) return;

    // Cap at 65507 bytes (max UDP payload)
    const int send_len = std::min(static_cast<int>(data.size()), 65507);
    ::sendto(udp_sock_,
             reinterpret_cast<const char*>(data.data()),
             send_len, 0,
             reinterpret_cast<const sockaddr*>(&bcast_addr_),
             static_cast<int>(sizeof(bcast_addr_)));
}

// ─────────────────────────────────────────────────────────────────────────────
//  accept_thread_fn — wait for incoming TCP connections
// ─────────────────────────────────────────────────────────────────────────────
void CommonLaserStream::accept_thread_fn() {
    while (running_.load(std::memory_order_acquire)) {
        ClsSockFd lsock;
        { std::lock_guard<std::mutex> lk(open_mutex_); lsock = listen_sock_; }
        if (lsock == kClsInvalidSock) break;

        // Use select with 100ms timeout so the thread can exit promptly.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lsock, &rfds);
        timeval tv{ 0, 100000 }; // 100 ms
        int r = ::select(static_cast<int>(lsock) + 1, &rfds, nullptr, nullptr, &tv);
        if (r <= 0) continue;

        sockaddr_in peer_addr{};
#ifdef _WIN32
        int peer_len = sizeof(peer_addr);
#else
        socklen_t peer_len = sizeof(peer_addr);
#endif
        ClsSockFd client = ::accept(lsock,
                                    reinterpret_cast<sockaddr*>(&peer_addr),
                                    &peer_len);
        if (client == kClsInvalidSock) continue;

        // Set TCP_NODELAY for low-latency streaming
        {
            int no_delay = 1;
            ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                         reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        }

        // 5ms send timeout — if the receiver can't drain fast enough, drop it
        // rather than blocking the DAC output thread.
#ifdef _WIN32
        {
            DWORD snd_to = 5;
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                         reinterpret_cast<const char*>(&snd_to), sizeof(snd_to));
        }
#else
        {
            struct timeval snd_to{ 0, 5000 };
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
        }
#endif

        {
            std::lock_guard<std::mutex> clk(clients_mutex_);
            if (static_cast<int>(clients_.size()) >= kClsMaxClients) {
                cls_close_sock(client);
                log::warn("CommonLaserStream: max clients reached, connection refused");
            } else {
                clients_.push_back({ client });
                char peer_ip[INET_ADDRSTRLEN]{};
                ::inet_ntop(AF_INET, &peer_addr.sin_addr, peer_ip, sizeof(peer_ip));
                log::info("CommonLaserStream: client connected from %s:%d",
                          peer_ip, ntohs(peer_addr.sin_port));
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  hello_thread_fn — periodic UDP discovery broadcast
// ─────────────────────────────────────────────────────────────────────────────
void CommonLaserStream::hello_thread_fn() {
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();

    while (running_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        if (now >= next) {
            send_udp(build_hello_packet());
            next = now + std::chrono::seconds(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace idhmfis
