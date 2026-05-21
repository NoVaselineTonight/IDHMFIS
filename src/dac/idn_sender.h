#pragma once
// IDN-Stream unicast sender (ILDA IDN-Stream F01).
//
// Wire format per ILDA IDN-Stream F01 specification:
//   [IdnPrimaryHeader   4B]  — command + flags + sequence (BE)
//   [IdnChannelHeader   8B]  — total_size (BE) + CNL + chunk_type + timestamp (BE)
//   [IdnChannelConfig   4B]  — first chunk only (CCLF=1): SCWC + CFL + svc_id + svc_mode
//   [GTS dictionary    12B]  — first chunk only: 6 × uint16 (BE) tag IDs
//   [IdnPoint × N       8B each] — int16 X/Y (BE), uint8 R,G,B,I
//
// Default destination port: 7255 (ILDA registered).
// Reference: ILDA IDN-Stream specification F01 (ilda.com/idn.htm)

#include "idac.h"

#include <cstdint>
#include <mutex>
#include <string>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SockFd = SOCKET;
   static constexpr SockFd kIdnInvalidSock = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using SockFd = int;
   static constexpr SockFd kIdnInvalidSock = -1;
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  IDN protocol constants (F01)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kIdnDefaultPort   = 7255;
static constexpr int      kIdnDefaultPPS    = 30000;
static constexpr int      kIdnMaxPPS        = 100000;
static constexpr int      kIdnMinPPS        = 5000;
static constexpr int      kIdnMaxUdpPayload = 1400; // stay below Ethernet MTU

// Primary command codes (F01 §4.1)
static constexpr uint8_t kIdnCmdVoid    = 0x00; // keep-alive / discovery ping
static constexpr uint8_t kIdnCmdMessage = 0x40; // realtime channel message

// Chunk type for complete laser frame (F01 §5.3.2)
static constexpr uint8_t kIdnChunkLaserFrame = 0x02;

// Service mode: Laser Projector Graphic Continuous (F01 §5.4)
static constexpr uint8_t kIdnServiceModeLaserContinuous = 0x01;

// GTS (General Tag Set) identifiers — ILDA IDN tag registry
static constexpr uint16_t kIdnGtsX         = 0x4200u; // X coordinate (int16 signed)
static constexpr uint16_t kIdnGtsY         = 0x4210u; // Y coordinate (int16 signed)
static constexpr uint16_t kIdnGtsRed       = 0x527Eu; // Red   (uint8, 638 nm)
static constexpr uint16_t kIdnGtsGreen     = 0x5214u; // Green (uint8, 532 nm)
static constexpr uint16_t kIdnGtsBlue      = 0x51CCu; // Blue  (uint8, 460 nm)
static constexpr uint16_t kIdnGtsIntensity = 0x5C10u; // Intensity (uint8)
static constexpr int      kIdnGtsCount     = 6;        // entries above

// ─────────────────────────────────────────────────────────────────────────────
//  On-wire structures (packed; all multi-byte fields are big-endian)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)

// Primary packet header — 4 bytes, starts every IDN datagram
struct IdnPrimaryHeader {
    uint8_t  command;  // kIdnCmdVoid or kIdnCmdMessage
    uint8_t  flags;    // 0x00
    uint16_t sequence; // monotonic counter, big-endian
};

// Channel message header — 8 bytes, follows primary header for kIdnCmdMessage
// CNL: bit7=1 always, bit6=CCLF (channel config present), bits5-0=channel_id
struct IdnChannelHeader {
    uint16_t total_size; // total datagram bytes, big-endian
    uint8_t  cnl;        // 0xC1 on first chunk (bit7|CCLF|ch_id=1), 0x81 on continuation
    uint8_t  chunk_type; // kIdnChunkLaserFrame (0x02)
    uint32_t timestamp;  // microseconds, big-endian
};

// Channel config — 4 bytes, follows channel header when CCLF bit is set
struct IdnChannelConfig {
    uint8_t scwc;         // scan word count = kIdnGtsCount (6)
    uint8_t cfl;          // 0x00
    uint8_t service_id;   // 0x00
    uint8_t service_mode; // kIdnServiceModeLaserContinuous (0x01)
};

// On-wire laser point — 8 bytes in GTS dictionary order (X, Y, R, G, B, I)
struct IdnPoint {
    int16_t x; // ILDA ±32767, stored big-endian
    int16_t y;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t i; // intensity; 0xFF=full, 0x00=blanked
};

#pragma pack(pop)

// Header overhead for the first chunk of a frame (with config + GTS dictionary)
static constexpr int kIdnFirstChunkHdr =
    static_cast<int>(sizeof(IdnPrimaryHeader)) +   // 4
    static_cast<int>(sizeof(IdnChannelHeader)) +   // 8
    static_cast<int>(sizeof(IdnChannelConfig)) +   // 4
    kIdnGtsCount * 2;                              // 12  → total 28

// ─────────────────────────────────────────────────────────────────────────────
//  IdnSender — unicast IDN-Stream output DAC
// ─────────────────────────────────────────────────────────────────────────────
class IdnSender final : public IDac {
public:
    IdnSender(std::string target_ip, uint16_t port = kIdnDefaultPort);
    ~IdnSender() override;

    bool        open()                          override;
    void        close()                         override;
    bool        is_open()              const    override;
    DacStatus   status()               const    override;
    bool        set_point_rate(int pps)         override;
    int         max_point_rate()       const    override { return kIdnMaxPPS; }
    int         min_point_rate()       const    override { return kIdnMinPPS; }
    int         send_points(const PointBuffer&) override;
    const char* type_name()            const    override { return "idn"; }

private:
    static IdnPoint convert_point(const LaserPoint& p);
    bool send_packet(const IdnPoint* pts, int count,
                     uint32_t timestamp_us, bool include_config);

    std::string  target_ip_;
    uint16_t     port_        = kIdnDefaultPort;
    int          point_rate_  = kIdnDefaultPPS;

    SockFd       sock_        = kIdnInvalidSock;
    sockaddr_in  dest_addr_   = {};
    bool         open_        = false;

    uint16_t     sequence_    = 0;
    uint64_t     points_sent_ = 0;

    mutable std::mutex mutex_;
};

} // namespace idhmfis
