// idn_sender.cpp — IDN-Stream unicast sender (ILDA IDN-Stream F01).
//
// Packet layout per send_packet():
//   First chunk  (include_config=true):
//     [IdnPrimaryHeader 4B][IdnChannelHeader 8B][IdnChannelConfig 4B]
//     [GTS dictionary 12B][IdnPoint × N 8B each]
//   Continuation chunks (include_config=false):
//     [IdnPrimaryHeader 4B][IdnChannelHeader 8B][IdnPoint × N 8B each]

#include "idn_sender.h"
#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
namespace {
struct WsaInitIdn {
    WSADATA w;
    WsaInitIdn()  { WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaInitIdn() { WSACleanup(); }
};
static WsaInitIdn g_wsa_idn;
inline void idn_close_socket(SockFd s) { closesocket(s); }
} // anonymous namespace
#else
#  include <unistd.h>
namespace { inline void idn_close_socket(SockFd s) { ::close(s); } }
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
IdnSender::IdnSender(std::string target_ip, uint16_t port)
    : target_ip_(std::move(target_ip)), port_(port)
{}

IdnSender::~IdnSender() { close(); }

// ─────────────────────────────────────────────────────────────────────────────
//  open / close / is_open
// ─────────────────────────────────────────────────────────────────────────────
bool IdnSender::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (open_) return true;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kIdnInvalidSock) {
        log::error("IDN: socket() failed");
        return false;
    }

    int sndbuf = 256 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

    std::memset(&dest_addr_, 0, sizeof(dest_addr_));
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, target_ip_.c_str(), &dest_addr_.sin_addr) != 1) {
        log::error("IDN: invalid target IP '%s'", target_ip_.c_str());
        idn_close_socket(sock_);
        sock_ = kIdnInvalidSock;
        return false;
    }

    open_ = true;
    log::info("IDN: sender opened -> %s:%d", target_ip_.c_str(), port_);
    return true;
}

void IdnSender::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_) return;
    idn_close_socket(sock_);
    sock_ = kIdnInvalidSock;
    open_ = false;
    log::info("IDN: sender closed");
}

bool IdnSender::is_open() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return open_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  status / set_point_rate
// ─────────────────────────────────────────────────────────────────────────────
DacStatus IdnSender::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    DacStatus s;
    s.connected      = open_;
    s.point_rate     = point_rate_;
    s.buffer_free    = 4096;
    s.device_name    = "IDN-Stream -> " + target_ip_;
    s.driver_version = "idn-f01";
    return s;
}

bool IdnSender::set_point_rate(int pps) {
    if (pps < kIdnMinPPS || pps > kIdnMaxPPS) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    point_rate_ = pps;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  convert_point — LaserPoint -> on-wire IdnPoint
// ─────────────────────────────────────────────────────────────────────────────
IdnPoint IdnSender::convert_point(const LaserPoint& p) {
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
//  send_packet — assemble one IDN-Stream datagram and send it
// ─────────────────────────────────────────────────────────────────────────────
bool IdnSender::send_packet(const IdnPoint* pts, int count,
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
    ph->sequence = htons(sequence_++);
    cur += sizeof(IdnPrimaryHeader);

    // Channel header
    // CNL: bit7=1 always | bit6=CCLF when config present | bits5-0=channel_id(1)
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

    int r = static_cast<int>(::sendto(sock_,
        reinterpret_cast<const char*>(buf.data()),
        total, 0,
        reinterpret_cast<const sockaddr*>(&dest_addr_),
        sizeof(dest_addr_)));

    return r == total;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_points
// ─────────────────────────────────────────────────────────────────────────────
int IdnSender::send_points(const PointBuffer& pts) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_ || pts.empty()) return 0;

    // Use first-chunk capacity (most restrictive: 28-byte header)
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
            log::warn("IDN: sendto failed after %d points", sent);
            break;
        }
    }

    points_sent_ += static_cast<uint64_t>(sent);
    return sent;
}

} // namespace idhmfis
