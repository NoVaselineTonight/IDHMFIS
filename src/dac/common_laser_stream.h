#pragma once
// CommonLaserStream — universal TCP/UDP laser frame streaming sidecar (CLS1 protocol).
//
// Runs alongside the primary DAC and mirrors every laser frame to any connected receiver:
//   TCP server port 7256 — reliable, multi-client streaming; any receiver may connect.
//   UDP broadcast 255.255.255.255:7256 — fire-and-forget; no configuration on receiver.
//
// This covers the output method used by the majority of open laser tools:
//   - LaserBoy, OpenLase, LaserShowGen (custom TCP/UDP frame streaming)
//   - Resolume, VJ tools (OSC-style raw frame receivers)
//   - Custom scripted receivers (Python/Node/etc.)
//
// CLS1 wire format (all multi-byte fields little-endian):
//   ClsPacketHeader (12 B): magic "CLS1", type(2), flags(2), body_len(4)
//   Frame body: timestamp_us(4) + point_rate(4) + point_count(4) + ClsPoint[N]
//   ClsPoint (8 B): x(2) y(2) r g b flags  — flags bit0=blanked
//   Hello body: version(4) + name[28]
//
// Default port: 7256.  Up to 8 simultaneous TCP clients.

#include "idac.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
using ClsSockFd = SOCKET;
static constexpr ClsSockFd kClsInvalidSock = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
using ClsSockFd = int;
static constexpr ClsSockFd kClsInvalidSock = -1;
#endif

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace idhmfis {

static constexpr uint16_t kClsDefaultPort = 7256;
static constexpr int      kClsMaxClients  = 8;

// ─────────────────────────────────────────────────────────────────────────────
//  CLS1 on-wire structures (packed, little-endian)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)

struct ClsPacketHeader {
    char     magic[4];   // "CLS1"
    uint16_t type;       // 0x0001=frame, 0x0002=hello
    uint16_t flags;      // 0
    uint32_t body_len;   // bytes of body following this header
};

struct ClsFrameBody {
    uint32_t timestamp_us;   // low 32 bits of steady_clock microseconds
    uint32_t point_rate;     // points per second
    uint32_t point_count;    // ClsPoint records following
};

struct ClsPoint {
    int16_t x;           // ILDA ±32767
    int16_t y;           // ILDA ±32767
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t flags;       // bit0 = blanked (0=lit, 1=blanked)
};

struct ClsHelloBody {
    uint32_t version;    // 1
    char     name[28];   // null-padded ASCII source name
};

#pragma pack(pop)

static constexpr uint16_t kClsTypeFrame = 0x0001;
static constexpr uint16_t kClsTypeHello = 0x0002;

// ─────────────────────────────────────────────────────────────────────────────
//  CommonLaserStream
// ─────────────────────────────────────────────────────────────────────────────
class CommonLaserStream {
public:
    CommonLaserStream();
    ~CommonLaserStream();

    bool open();
    void close();
    bool is_open() const;

    bool set_point_rate(int pps);

    // Mirror pts to all TCP clients and via UDP broadcast.
    // Returns number of points sent (0 if closed or pts empty).
    int send_points(const PointBuffer& pts);

private:
    void accept_thread_fn();
    void hello_thread_fn();

    static std::vector<uint8_t> build_frame_packet(const PointBuffer& pts,
                                                    uint32_t timestamp_us,
                                                    uint32_t point_rate);
    static std::vector<uint8_t> build_hello_packet();

    void send_to_tcp_clients(const std::vector<uint8_t>& data);
    void send_udp(const std::vector<uint8_t>& data);

    mutable std::mutex open_mutex_;
    bool               open_  = false;
    uint16_t           port_  = kClsDefaultPort;
    int                pps_   = 30000;

    ClsSockFd          listen_sock_ = kClsInvalidSock;
    ClsSockFd          udp_sock_    = kClsInvalidSock;
    sockaddr_in        bcast_addr_  = {};

    struct Client { ClsSockFd sock = kClsInvalidSock; };
    mutable std::mutex        clients_mutex_;
    std::vector<Client>       clients_;

    std::atomic<bool>  running_{ false };
    std::thread        accept_thread_;
    std::thread        hello_thread_;
};

} // namespace idhmfis
