// artnet_sender.cpp — ArtNet DMX output sender implementation.
//
// Sends ArtDmx (OpCode 0x5000) packets over UDP.
// Supports unicast (specific IP) and broadcast (2.255.255.255 or
// subnet broadcast) modes.
//
// Thread safety: send_dmx() and send_universe() are serialised with
// send_mtx_ so they can be called from the engine thread at 1 kHz.
//
// WSA lifecycle: WSAStartup() reference-counts on Windows; it is safe
// to call it in every ArtNetSender instance as WSACleanup() mirrors it.

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

#include <cstring>
#include <cstdio>
#include <algorithm>

#include "artnet_sender.h"
#include "../core/logger.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Platform helpers (local to this TU)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

#ifdef _WIN32
inline bool sender_socket_valid(SOCKET s) { return s != INVALID_SOCKET; }
inline void sender_socket_close(SOCKET s) { if (s != INVALID_SOCKET) ::closesocket(s); }
#else
inline bool sender_socket_valid(int s)    { return s >= 0; }
inline void sender_socket_close(int s)    { if (s >= 0) ::close(s); }
#endif

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
ArtNetSender::ArtNetSender() {
#ifdef _WIN32
    // WSAStartup is reference-counted; safe to call from multiple instances.
    WSADATA wsd;
    ::WSAStartup(MAKEWORD(2, 2), &wsd);
#endif
}

ArtNetSender::~ArtNetSender() {
    close();
#ifdef _WIN32
    ::WSACleanup();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  Configuration
// ─────────────────────────────────────────────────────────────────────────────
void ArtNetSender::set_target(const std::string& ip, int port) {
    target_ip_   = ip;
    target_port_ = port;
    // Auto-detect broadcast mode from IP
    broadcast_ = (ip == "2.255.255.255" ||
                  ip == "255.255.255.255" ||
                  ip.find(".255") != std::string::npos);
}

void ArtNetSender::set_broadcast(bool enabled) {
    broadcast_ = enabled;
    if (enabled)
        target_ip_ = "2.255.255.255";
}

// ─────────────────────────────────────────────────────────────────────────────
//  open / close / is_open
// ─────────────────────────────────────────────────────────────────────────────
bool ArtNetSender::open() {
    if (is_open()) return true;

#ifdef _WIN32
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        log::error("ArtNetSender: socket() failed, err=%d", WSAGetLastError());
        return false;
    }
#else
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        log::error("ArtNetSender: socket() failed");
        return false;
    }
#endif

    if (broadcast_) {
        int yes = 1;
        if (::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST,
                         reinterpret_cast<const char*>(&yes), sizeof(yes)) != 0) {
            log::warn("ArtNetSender: SO_BROADCAST failed — unicast only");
        }
    }

    log::info("ArtNetSender: opened, target=%s:%d broadcast=%s",
              target_ip_.c_str(), target_port_, broadcast_ ? "yes" : "no");
    return true;
}

void ArtNetSender::close() {
    std::lock_guard<std::mutex> lk(send_mtx_);
    sender_socket_close(sock_);
#ifdef _WIN32
    sock_ = INVALID_SOCKET;
#else
    sock_ = -1;
#endif
}

bool ArtNetSender::is_open() const {
    return sender_socket_valid(sock_);
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_dmx — build and transmit one ArtDmx packet
// ─────────────────────────────────────────────────────────────────────────────
bool ArtNetSender::send_dmx(int universe, const uint8_t* channels, int count) {
    if (!is_open()) return false;
    if (!channels)  return false;

    // Clamp count to [2, 512] and round up to even per Art-Net 4 spec
    if (count < 2)   count = 2;
    if (count > 512) count = 512;
    if (count & 1)   count++;  // must be even

    ArtDmxPacket pkt{};
    // id and most fields are initialised by the default member initialisers
    // in the struct definition.

    pkt.sequence = ++sequence_;  // 1-255, wraps to 1 (0 = disabled)
    if (pkt.sequence == 0) pkt.sequence = 1;

    // universe: 15-bit portAddress, little-endian
    pkt.universe = static_cast<uint16_t>(universe & 0x7FFF);

    // length: channel count, BIG-endian
    pkt.length = htons(static_cast<uint16_t>(count));

    std::memcpy(pkt.data, channels, static_cast<size_t>(count));
    // Zero-fill the rest so we never transmit stale data
    if (count < 512)
        std::memset(pkt.data + count, 0, static_cast<size_t>(512 - count));

    // Build destination address
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(static_cast<uint16_t>(target_port_));
#ifdef _WIN32
    dest.sin_addr.s_addr = inet_addr(target_ip_.c_str());
#else
    if (::inet_pton(AF_INET, target_ip_.c_str(), &dest.sin_addr) <= 0) {
        dest.sin_addr.s_addr = INADDR_BROADCAST;
    }
#endif

    // Packet size: 18-byte header + count data bytes
    const int pkt_size = 18 + count;

    std::lock_guard<std::mutex> lk(send_mtx_);
    int sent = static_cast<int>(::sendto(
        sock_,
        reinterpret_cast<const char*>(&pkt),
        static_cast<size_t>(pkt_size),
        0,
        reinterpret_cast<const sockaddr*>(&dest),
        sizeof(dest)));

    if (sent != pkt_size) {
#ifdef _WIN32
        log::debug("ArtNetSender: sendto() partial/failed, sent=%d expected=%d err=%d",
                   sent, pkt_size, WSAGetLastError());
#else
        log::debug("ArtNetSender: sendto() partial/failed, sent=%d expected=%d",
                   sent, pkt_size);
#endif
        return false;
    }

    pkt_count_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_universe — convenience: send full 512-channel universe
// ─────────────────────────────────────────────────────────────────────────────
bool ArtNetSender::send_universe(int universe, const uint8_t* data) {
    return send_dmx(universe, data, 512);
}

} // namespace idhmfis
