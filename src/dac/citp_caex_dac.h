#pragma once
// CITP/CAEX laser source output for IDHMFIS.
//
// CITP (Controller Interface Transport Protocol) with the CAEX (Capture Advanced
// Exchange) extension allows IDHMFIS to appear as a laser feed source in Capture
// 2024 and compatible visualizers.
//
// Protocol overview:
//   - UDP multicast PLoc beacons (239.224.0.180:4809 + 224.0.0.180:4809) every ~1 s
//   - TCP server on port 6430 for Capture to connect and request feeds
//   - UDP multicast LaserFeedFrame packets at the rate requested by Capture
//
// Reference: CITP/CAEX specification (internally documented, Capture 2024 compatible)

#include "idac.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
using CitpSocket = SOCKET;
static constexpr CitpSocket kCitpInvalidSocket = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
using CitpSocket = int;
static constexpr CitpSocket kCitpInvalidSocket = -1;
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  CITP/CAEX protocol constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t kCitpCookie      = 0x50544943u; // "CITP"
static constexpr uint32_t kCitpPinfCookie  = 0x464E4950u; // "PINF"
static constexpr uint32_t kCitpCaexCookie  = 0x58454143u; // "CAEX"
static constexpr uint32_t kCitpPLocCode    = 0x636F4C50u; // PLoc sub-type
static constexpr uint32_t kCitpPNamCode    = 0x6D614E50u; // PNam sub-type

// Content codes per CITP/CAEX specification revision F (2020-07-03).
// These are numeric uint32 identifiers, NOT ASCII 4CCs.
// Source: https://www.capture.se/Portals/0/Downloads/CITP%20CAEX%20Specification%20F.pdf
static constexpr uint32_t kCaexGetFeedList   = 0x00030100u;
static constexpr uint32_t kCaexFeedList      = 0x00030101u;
static constexpr uint32_t kCaexFeedControl   = 0x00030108u; // spec rev F alt
static constexpr uint32_t kCaexFeedControl2  = 0x00030102u; // Capture 2024 / spec F
static constexpr uint32_t kCaexFeedFrame     = 0x00030200u; // LaserFeedFrame per spec F

// MSEX (Media Server Extensions) — Capture sends CInf on every TCP connection.
// We reply with SInf declaring zero media libraries so Capture treats us as a
// laser controller rather than a generic media server.
static constexpr uint32_t kCitpMsexCookie   = 0x5845534Du; // "MSEX" LE
static constexpr uint32_t kMsexCInfCode     = 0x666E4943u; // "CInf" LE
static constexpr uint32_t kMsexSInfCode     = 0x666E4953u; // "SInf" LE

static constexpr uint16_t kCitpTcpPort     = 6430u;
static constexpr int      kCitpMcastTTL    = 4;

// Primary and legacy CITP multicast groups
static constexpr const char* kCitpMcast1   = "239.224.0.180";
static constexpr const char* kCitpMcast2   = "224.0.0.180";
static constexpr uint16_t    kCitpMcastPort = 4809u;

static constexpr int kCitpMaxPPS = 60000;

// ─────────────────────────────────────────────────────────────────────────────
//  On-wire structures (packed, little-endian)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)

struct CitpHeader {
    uint32_t Cookie;            // kCitpCookie
    uint8_t  VersionMajor;     // 1
    uint8_t  VersionMinor;     // 0
    uint16_t RequestIndex;     // 0
    uint32_t MessageSize;      // total message bytes
    uint16_t MessagePartCount; // 1
    uint16_t MessagePart;      // 0
    uint32_t ContentType;      // layer cookie
};

struct CitpPinfHeader {
    CitpHeader CITPHeader;     // ContentType = kCitpPinfCookie
    uint32_t   ContentType;    // sub-message code
};

struct CitpCaexHeader {
    CitpHeader CITPHeader;     // ContentType = kCitpCaexCookie
    uint32_t   ContentCode;    // numeric CAEX code
};

struct CaexPoint {
    uint8_t  XLowByte;
    uint8_t  YLowByte;
    uint8_t  XYHighNibbles;    // bits[3:0]=X high, bits[7:4]=Y high
    uint16_t Color;            // R5G6B5 little-endian
};

#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
//  CitpCaexDac
// ─────────────────────────────────────────────────────────────────────────────
class CitpCaexDac final : public IDac {
public:
    // stream_idx == 0  →  name "IDHMFIS",   TCP port 6430
    // stream_idx == N  →  name "IDHMFIS-N", TCP port 6430+N
    explicit CitpCaexDac(int stream_idx = 0);
    ~CitpCaexDac() override;

    // IDac interface
    [[nodiscard]] bool     open()                      override;
    void                   close()                     override;
    [[nodiscard]] bool     is_open()         const     override;
    DacStatus              status()          const     override;
    [[nodiscard]] bool     set_point_rate(int pps)     override;
    int                    max_point_rate()  const     override { return kCitpMaxPPS; }
    int                    min_point_rate()  const     override { return 1000; }
    int                    send_points(const PointBuffer& pts) override;
    const char*            type_name()       const     override { return "citp_caex"; }

    // Per-stream identity accessors (used by DacManager for UI reporting)
    const std::string&     source_name()     const     { return source_name_; }
    uint16_t               tcp_port()        const     { return tcp_port_; }

private:
    // ── Worker threads ────────────────────────────────────────────────────────
    void announce_thread_fn();
    void tcp_server_thread_fn();
    void handle_client(CitpSocket client);

    // ── Packet builders ───────────────────────────────────────────────────────
    std::vector<uint8_t> build_ploc() const;
    std::vector<uint8_t> build_pnam() const;
    std::vector<uint8_t> build_sinf(uint8_t ver_major, uint8_t ver_minor) const;
    std::vector<uint8_t> build_feed_list() const;
    std::vector<uint8_t> build_frame(const PointBuffer& pts);

    // ── Socket helpers ────────────────────────────────────────────────────────
    [[nodiscard]] bool open_udp_socket();
    [[nodiscard]] bool open_tcp_server();
    void               close_socket(CitpSocket& s);

    // Convert ILDA int16 coord to 12-bit CAEX coord
    static uint16_t ilda_to_caex(int16_t ilda);
    static uint16_t rgb888_to_r5g6b5(uint8_t r, uint8_t g, uint8_t b);

    // Fill a CitpHeader in-place
    static void fill_citp_header(CitpHeader& h, uint32_t content_type,
                                 uint32_t total_size);

    // ── State ─────────────────────────────────────────────────────────────────
    // Per-instance identity — unique when multiple DacManagers run in parallel.
    std::string           source_name_; // e.g. "IDHMFIS" or "IDHMFIS-2"
    uint16_t              tcp_port_{ kCitpTcpPort }; // base + stream_idx

    std::atomic<bool>     open_{ false };
    std::atomic<bool>     running_{ false };
    std::atomic<uint8_t>  requested_fps_{ 0 };
    std::atomic<bool>     client_connected_{ false };
    std::atomic<uint32_t> frame_seq_{ 0 };
    std::atomic<int64_t>  last_frame_us_{ 0 }; // last FeedFrame send time (microseconds, steady_clock)
    // Sockets
    CitpSocket            udp_fd_         = kCitpInvalidSocket;
    CitpSocket            tcp_listen_fd_  = kCitpInvalidSocket;
    CitpSocket            tcp_client_fd_  = kCitpInvalidSocket;
    mutable std::mutex    socket_mutex_;

    // Multicast destinations
    sockaddr_in           mcast_addr1_{}; // 239.224.0.180:4809
    sockaddr_in           mcast_addr2_{}; // 224.0.0.180:4809
    sockaddr_in           unicast_addr_{}; // local machine IP:4809 (same-host fallback)
    // Connected client's unicast address — populated by tcp_server_thread_fn.
    // FeedFrame is also sent unicast here so same-machine delivery is guaranteed
    // regardless of Windows multicast loopback reliability.
    std::atomic<uint32_t> client_ip_net_{ 0 }; // in network byte order; 0 = none

    // Threads
    std::thread           announce_thread_;
    std::thread           tcp_server_thread_;

    // Source key — random uint32, generated once at construction.
    // Included in LaserFeedList and LaserFeedFrame per CAEX spec §5.3/5.4.
    uint32_t              source_key_;

    // Current point rate
    int                   point_rate_ = 30000;

    mutable std::mutex    state_mutex_;
};

} // namespace idhmfis
