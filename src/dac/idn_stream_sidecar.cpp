// idn_stream_sidecar.cpp — IDN-Stream broadcast sidecar (ILDA IDN-Stream F01).
//
// Broadcasts laser frames and IDN-VOID discovery pings on 255.255.255.255:7255.
// Each laser frame packet carries service_mode=0x01 (Laser Projector Graphic
// Continuous) so IDN-capable receivers identify this source as a laser controller.
//
// Packet layout for first chunk of each frame (include_config=true):
//   [IdnPrimaryHeader 4B]  command=0x40, flags=0, sequence
//   [IdnChannelHeader 8B]  total_size, cnl=0xC1 (CCLF+ch1), chunk_type=0x02, timestamp
//   [IdnChannelConfig 4B]  scwc=6, cfl=0, service_id=0, service_mode=0x01
//   [GTS dictionary  12B]  X(0x4200), Y(0x4210), R(0x527E), G(0x5214), B(0x51CC), I(0x5C10)
//   [IdnPoint × N    8B each]
//
// Continuation chunks (include_config=false) skip config+dictionary: cnl=0x81.
//
// IDN-Hello discovery: sends a 4-byte VOID ping (command=0x00) every 1 s.

#include "idn_stream_sidecar.h"
#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
namespace {
struct WsaInitSidecar {
    WSADATA w;
    WsaInitSidecar()  { WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaInitSidecar() { WSACleanup(); }
};
static WsaInitSidecar g_wsa_sidecar;
inline void sidecar_close_socket(SockFd s) { closesocket(s); }
} // anonymous namespace
#else
#  include <unistd.h>
namespace { inline void sidecar_close_socket(SockFd s) { ::close(s); } }
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
IdnStreamSidecar::IdnStreamSidecar() = default;

IdnStreamSidecar::~IdnStreamSidecar() { close(); }

// ─────────────────────────────────────────────────────────────────────────────
//  open
// ─────────────────────────────────────────────────────────────────────────────
bool IdnStreamSidecar::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (open_) return true;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kIdnInvalidSock) {
        log::error("IdnStreamSidecar: socket() failed");
        return false;
    }

    int bcast = 1;
    if (::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&bcast),
                     sizeof(bcast)) != 0) {
        log::error("IdnStreamSidecar: setsockopt(SO_BROADCAST) failed");
        sidecar_close_socket(sock_);
        sock_ = kIdnInvalidSock;
        return false;
    }

    int sndbuf = 256 * 1024;
    ::setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
                 reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

    int ttl = 4;
    ::setsockopt(sock_, IPPROTO_IP, IP_TTL,
                 reinterpret_cast<const char*>(&ttl), sizeof(ttl));

    std::memset(&bcast_addr_, 0, sizeof(bcast_addr_));
    bcast_addr_.sin_family      = AF_INET;
    bcast_addr_.sin_port        = htons(kIdnHelloPort);
    bcast_addr_.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    open_ = true;
    running_.store(true, std::memory_order_release);
    announce_thread_ = std::thread([this]{ announce_thread_fn(); });

    log::info("IdnStreamSidecar: opened, broadcasting on 255.255.255.255:%d",
              kIdnHelloPort);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  close
// ─────────────────────────────────────────────────────────────────────────────
void IdnStreamSidecar::close() {
    running_.store(false, std::memory_order_release);
    if (announce_thread_.joinable())
        announce_thread_.join();

    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_) return;
    sidecar_close_socket(sock_);
    sock_ = kIdnInvalidSock;
    open_ = false;
    log::info("IdnStreamSidecar: closed");
}

// ─────────────────────────────────────────────────────────────────────────────
//  is_open
// ─────────────────────────────────────────────────────────────────────────────
bool IdnStreamSidecar::is_open() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return open_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  convert_point
// ─────────────────────────────────────────────────────────────────────────────
IdnPoint IdnStreamSidecar::convert_point(const LaserPoint& p) {
    IdnPoint ip{};
    ip.x = static_cast<int16_t>(htons(static_cast<uint16_t>(p.x)));
    ip.y = static_cast<int16_t>(htons(static_cast<uint16_t>(p.y)));
    if (p.blanked) {
        ip.r = ip.g = ip.b = ip.i = 0;
    } else {
        ip.r = p.r;
        ip.g = p.g;
        ip.b = p.b;
        ip.i = 0xFF;
    }
    return ip;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_packet — assemble one IDN-Stream datagram and broadcast it
// ─────────────────────────────────────────────────────────────────────────────
bool IdnStreamSidecar::send_packet(const IdnPoint* pts, int count,
                                    uint32_t timestamp_us, bool include_config) {
    const int config_extra = include_config
        ? static_cast<int>(sizeof(IdnChannelConfig)) + kIdnGtsCount * 2
        : 0;
    const int point_bytes = count * static_cast<int>(sizeof(IdnPoint));
    const int total       = static_cast<int>(sizeof(IdnPrimaryHeader))
                          + static_cast<int>(sizeof(IdnChannelHeader))
                          + config_extra
                          + point_bytes;

    std::vector<uint8_t> buf(static_cast<size_t>(total), 0);
    uint8_t* cur = buf.data();

    // Primary header
    auto* ph    = reinterpret_cast<IdnPrimaryHeader*>(cur);
    ph->command  = kIdnCmdMessage;
    ph->flags    = 0x00;
    ph->sequence = htons(stream_seq_++);
    cur += sizeof(IdnPrimaryHeader);

    // Channel header
    auto* ch      = reinterpret_cast<IdnChannelHeader*>(cur);
    ch->total_size = htons(static_cast<uint16_t>(total));
    ch->cnl        = static_cast<uint8_t>(
        0x80u | (include_config ? 0x40u : 0x00u) | 0x01u);
    ch->chunk_type = kIdnChunkLaserFrame;
    ch->timestamp  = htonl(timestamp_us);
    cur += sizeof(IdnChannelHeader);

    // Channel config + GTS dictionary (first chunk only)
    if (include_config) {
        auto* cc       = reinterpret_cast<IdnChannelConfig*>(cur);
        cc->scwc         = static_cast<uint8_t>(kIdnGtsCount);
        cc->cfl          = 0x00;
        cc->service_id   = 0x00;
        cc->service_mode = kIdnServiceModeLaserContinuous;
        cur += sizeof(IdnChannelConfig);

        static constexpr uint16_t kTags[kIdnGtsCount] = {
            kIdnGtsX, kIdnGtsY, kIdnGtsRed, kIdnGtsGreen, kIdnGtsBlue, kIdnGtsIntensity
        };
        for (int i = 0; i < kIdnGtsCount; ++i) {
            uint16_t be = htons(kTags[i]);
            std::memcpy(cur, &be, 2);
            cur += 2;
        }
    }

    std::memcpy(cur, pts, static_cast<size_t>(point_bytes));

    int r = static_cast<int>(::sendto(
        sock_,
        reinterpret_cast<const char*>(buf.data()),
        total, 0,
        reinterpret_cast<const sockaddr*>(&bcast_addr_),
        static_cast<int>(sizeof(bcast_addr_))));

    return r == total;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_points
// ─────────────────────────────────────────────────────────────────────────────
int IdnStreamSidecar::send_points(const PointBuffer& pts) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_ || pts.empty()) return 0;

    constexpr int kPtsPerPkt = (kIdnMaxUdpPayload - kIdnFirstChunkHdr) /
                               static_cast<int>(sizeof(IdnPoint)); // 171

    using Clock = std::chrono::steady_clock;
    auto ts_us = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count() & 0xFFFFFFFFu);

    std::vector<IdnPoint> converted;
    converted.reserve(pts.size());
    for (const auto& p : pts)
        converted.push_back(convert_point(p));

    int sent = 0;
    int remaining = static_cast<int>(converted.size());
    bool first_chunk = true;

    while (remaining > 0) {
        int chunk = std::min(remaining, kPtsPerPkt);
        if (send_packet(converted.data() + sent, chunk, ts_us, first_chunk)) {
            sent      += chunk;
            remaining -= chunk;
            first_chunk = false;
        } else {
            log::warn("IdnStreamSidecar: sendto failed after %d points", sent);
            break;
        }
    }

    return sent;
}

// ─────────────────────────────────────────────────────────────────────────────
//  announce_thread_fn — periodic IDN VOID ping for source discovery
//
//  A 4-byte VOID packet (command=0x00) is the minimal IDN keep-alive that lets
//  receivers know a source is present on the network.
// ─────────────────────────────────────────────────────────────────────────────
void IdnStreamSidecar::announce_thread_fn() {
    using Clock = std::chrono::steady_clock;
    auto next_announce = Clock::now();

    while (running_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        if (now >= next_announce) {
            IdnPrimaryHeader ping{};
            SockFd sock_copy = kIdnInvalidSock;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (open_) {
                    ping.command  = kIdnCmdVoid;
                    ping.flags    = 0x00;
                    ping.sequence = htons(hello_seq_++);
                    sock_copy     = sock_;
                }
            }
            if (sock_copy != kIdnInvalidSock) {
                ::sendto(sock_copy,
                         reinterpret_cast<const char*>(&ping),
                         static_cast<int>(sizeof(ping)), 0,
                         reinterpret_cast<const sockaddr*>(&bcast_addr_),
                         static_cast<int>(sizeof(bcast_addr_)));
            }
            next_announce = now + std::chrono::milliseconds(kIdnAnnounceIntervalMs);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace idhmfis
