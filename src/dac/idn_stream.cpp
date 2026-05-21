// idn_stream.cpp — IDN-Stream UDP laser output (ILDA IDN-Stream F01).
//
// Each send_points() call produces one or more UDP datagrams.
// First datagram includes channel config + GTS dictionary (CCLF=1).
// Subsequent datagrams in the same frame are continuation chunks (CCLF=0).

#include "idn_stream.h"
#include "core/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <vector>

#ifdef _WIN32
namespace {
struct WsaInitIdnStream {
    WSADATA w;
    WsaInitIdnStream()  { WSAStartup(MAKEWORD(2, 2), &w); }
    ~WsaInitIdnStream() { WSACleanup(); }
};
static WsaInitIdnStream g_wsa_idns;
inline void idns_close_socket(SockFd s) { closesocket(s); }
} // anonymous namespace
#else
#  include <netdb.h>
#  include <unistd.h>
namespace { inline void idns_close_socket(SockFd s) { ::close(s); } }
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
IdnStreamOutput::IdnStreamOutput(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port)
{}

IdnStreamOutput::~IdnStreamOutput() { disconnect(); }

// ─────────────────────────────────────────────────────────────────────────────
//  connect / disconnect / is_connected
// ─────────────────────────────────────────────────────────────────────────────
bool IdnStreamOutput::connect() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (connected_) return true;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kIdnInvalidSock) {
        log::error("IdnStream: socket() failed for %s:%d", host_.c_str(), port_);
        return false;
    }

    int sndbuf = 512 * 1024;
    setsockopt(sock_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

    std::memset(&dest_addr_, 0, sizeof(dest_addr_));
    dest_addr_.sin_family = AF_INET;
    dest_addr_.sin_port   = htons(port_);

    bool resolved = false;
    if (::inet_pton(AF_INET, host_.c_str(), &dest_addr_.sin_addr) == 1) {
        resolved = true;
    } else {
#ifdef _WIN32
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host_.c_str(), nullptr, &hints, &res) == 0 && res) {
            std::memcpy(&dest_addr_.sin_addr,
                &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                sizeof(dest_addr_.sin_addr));
            freeaddrinfo(res);
            resolved = true;
        }
#else
        addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host_.c_str(), nullptr, &hints, &res) == 0 && res) {
            std::memcpy(&dest_addr_.sin_addr,
                &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                sizeof(dest_addr_.sin_addr));
            ::freeaddrinfo(res);
            resolved = true;
        }
#endif
    }

    if (!resolved) {
        log::error("IdnStream: cannot resolve host '%s'", host_.c_str());
        idns_close_socket(sock_);
        sock_ = kIdnInvalidSock;
        return false;
    }

    connected_  = true;
    negotiated_ = point_rate_;
    log::info("IdnStream: connected to %s:%d", host_.c_str(), port_);
    return true;
}

void IdnStreamOutput::disconnect() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!connected_) return;
    idns_close_socket(sock_);
    sock_      = kIdnInvalidSock;
    connected_ = false;
    log::info("IdnStream: disconnected from %s:%d", host_.c_str(), port_);
}

bool IdnStreamOutput::is_connected() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return connected_;
}

int IdnStreamOutput::negotiated_pps() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return negotiated_;
}

std::string IdnStreamOutput::id() const {
    std::ostringstream oss;
    oss << host_ << ":" << port_;
    return oss.str();
}

std::string IdnStreamOutput::name() const {
    return "IDN-Stream @ " + host_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  status / set_point_rate
// ─────────────────────────────────────────────────────────────────────────────
DacStatus IdnStreamOutput::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    DacStatus s;
    s.connected      = connected_;
    s.point_rate     = point_rate_;
    s.buffer_free    = 4096;
    s.device_name    = "IDN-Stream @ " + host_;
    s.driver_version = "idn-stream-f01";
    return s;
}

bool IdnStreamOutput::set_point_rate(int pps) {
    if (pps < kIdnMinPPS || pps > kIdnMaxPPS) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    point_rate_ = pps;
    negotiated_ = pps;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_frame (IDacOutput)
// ─────────────────────────────────────────────────────────────────────────────
bool IdnStreamOutput::send_frame(const PointBuffer& pts, int target_pps) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        int clamped = std::clamp(target_pps, kIdnMinPPS, kIdnMaxPPS);
        negotiated_ = clamped;
        point_rate_ = clamped;
    }
    int sent = send_points(pts);
    return sent == static_cast<int>(pts.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  convert_point
// ─────────────────────────────────────────────────────────────────────────────
IdnPoint IdnStreamOutput::convert_point(const LaserPoint& p) {
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
//  send_packet — assemble and send one IDN-Stream datagram
// ─────────────────────────────────────────────────────────────────────────────
bool IdnStreamOutput::send_packet(const IdnPoint* pts, int count,
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
//  send_points (IDac)
// ─────────────────────────────────────────────────────────────────────────────
int IdnStreamOutput::send_points(const PointBuffer& pts) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!connected_ || pts.empty()) return 0;

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
        if (!send_packet(converted.data() + sent, chunk, ts_us, first_chunk)) {
            log::warn("IdnStream: sendto failed after %d / %d points",
                      sent, static_cast<int>(pts.size()));
            break;
        }
        sent      += chunk;
        remaining -= chunk;
        first_chunk = false;
    }

    return sent;
}

} // namespace idhmfis
