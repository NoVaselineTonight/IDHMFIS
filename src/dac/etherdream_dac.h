#pragma once
// EtherDream DAC driver.
//
// Protocol specification: https://ether-dream.com/protocol.html
//   UDP discovery on port 7654 (device broadcasts ~every 1 s)
//   TCP control/data on port 7765
//
// Flow control: low-water-mark scheme.
//   The DAC reports free buffer space in every ACK response.
//   When free space drops below low_water_mark the driver re-sends data.
//   Default low_water_mark = 1600 points (≈ 53 ms at 30 kpps).

#include "idac.h"
#include "dac_interface.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SockFd = SOCKET;
   static constexpr SockFd kInvalidSock = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using SockFd = int;
   static constexpr SockFd kInvalidSock = -1;
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  EtherDream protocol constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int      kEDDiscoveryPort     = 7654;
static constexpr int      kEDDataPort          = 7765;
static constexpr int      kEDMagicLen          = 4;
static constexpr int      kEDDefaultLowWater   = 1600;
static constexpr int      kEDDefaultPPS        = 30000;
static constexpr int      kEDMaxPPS            = 100000;
static constexpr int      kEDMinPPS            = 7500;
static constexpr int      kEDMaxPoints         = 1799;  // per DATA packet
static constexpr int      kEDBufferSize        = 1800;  // hardware FIFO in points

// Command bytes
static constexpr uint8_t  kEDCmdPrepare        = 0x70; // 'p'
static constexpr uint8_t  kEDCmdBegin          = 0x62; // 'b'
static constexpr uint8_t  kEDCmdData           = 0x64; // 'd'
static constexpr uint8_t  kEDCmdStop           = 0x73; // 's'
static constexpr uint8_t  kEDCmdEmergStop      = 0x00;
static constexpr uint8_t  kEDCmdPing           = 0x3F; // '?'
static constexpr uint8_t  kEDCmdClearStop      = 0x63; // 'c'
static constexpr uint8_t  kEDRespAck           = 0x61; // 'a'
static constexpr uint8_t  kEDRespNak           = 0x6E; // 'n'

// Playback state flags (from ACK response)
static constexpr uint8_t  kEDStatePreparing    = 0;
static constexpr uint8_t  kEDStatePrepared     = 1;
static constexpr uint8_t  kEDStatePlaying      = 2;
static constexpr uint8_t  kEDStateHWStopped    = 3;
static constexpr uint8_t  kEDStateSWPause      = 4;

// Discovery packet magic
static constexpr char     kEDMagic[4]          = { 0x4A, 0x4C, 0x61, 0x73 }; // "JLas"

// ─────────────────────────────────────────────────────────────────────────────
//  On-wire structures (packed, little-endian)
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)

// 8-channel DAC point as sent over TCP
struct EtherDreamPoint {
    int16_t  x;   // ±32767 ILDA
    int16_t  y;
    uint16_t r;   // 0–65535
    uint16_t g;
    uint16_t b;
    uint16_t i;   // intensity
    uint16_t u1;  // user data
    uint16_t u2;
};

// Full DAC status response (sent on connect + with every ACK)
struct EtherDreamDacStatus {
    uint8_t  protocol;          // 0
    uint8_t  light_engine_state;
    uint8_t  playback_state;
    uint8_t  source;
    uint16_t light_engine_flags;
    uint16_t playback_flags;
    uint16_t source_flags;
    uint16_t buffer_fullness;   // points currently in buffer
    uint32_t point_rate;
    uint32_t point_count;
};

// Broadcast discovery packet from EtherDream firmware
struct EtherDreamBroadcast {
    char     magic[4];          // kEDMagic
    uint8_t  mac[6];
    uint8_t  hw_revision;
    uint8_t  sw_revision;
    uint16_t buffer_capacity;
    uint32_t max_point_rate;
    EtherDreamDacStatus status;
};

// ACK/response header
struct EtherDreamResponse {
    uint8_t  response;          // kEDRespAck or kEDRespNak
    uint8_t  command;           // which command this acks
    EtherDreamDacStatus status;
};

// Command: begin playback
struct EtherDreamBeginCmd {
    uint8_t  command = kEDCmdBegin;
    uint16_t low_water_mark;
    uint32_t point_rate;
};

// Command: send data
struct EtherDreamDataCmd {
    uint8_t  command = kEDCmdData;
    uint16_t num_points;
    // Followed immediately by num_points * sizeof(EtherDreamPoint)
};

#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
//  EtherDreamDac
// ─────────────────────────────────────────────────────────────────────────────
class EtherDreamDac final : public IDac, public IDacOutput {
public:
    // Construct for a specific IP (no discovery needed, direct connect).
    explicit EtherDreamDac(std::string target_ip = "");

    // Construct from a discovered broadcast packet.
    explicit EtherDreamDac(const EtherDreamBroadcast& bcast, const std::string& ip);

    ~EtherDreamDac() override;

    // ── IDac interface ───────────────────────────────────────────────────────
    bool      open()                          override;
    void      close()                         override;
    bool      is_open()              const    override;
    DacStatus status()               const    override;
    bool      set_point_rate(int pps)         override;
    int       max_point_rate()       const    override { return kEDMaxPPS; }
    int       min_point_rate()       const    override { return kEDMinPPS; }
    int       send_points(const PointBuffer&) override;
    const char* type_name()          const    override { return "etherdream"; }
    std::string target_address()     const    override { return target_ip_; }

    // ── IDacOutput interface ─────────────────────────────────────────────────
    bool        connect()                     override { return open(); }
    void        disconnect()                  override { close(); }
    bool        is_connected()       const    override { return is_open(); }
    int         negotiated_pps()     const    override;
    int         max_pps()            const    override { return kEDMaxPPS; }
    bool        send_frame(const PointBuffer& pts, int target_pps) override;
    std::string id()                 const    override;
    std::string name()               const    override;

    // UDP discovery: returns a list of discovered EtherDream IPs.
    // Blocks for up to timeout_ms.
    static std::vector<std::pair<std::string, EtherDreamBroadcast>>
        discover(int timeout_ms = 1500);

    // MAC address as hex string, valid after successful open
    std::string mac_string() const;

private:
    bool         tcp_connect();
    void         tcp_disconnect();
    bool         send_raw(const void* data, int len);
    bool         recv_response(EtherDreamResponse& out, int timeout_ms = 100);
    bool         send_cmd_prepare();
    bool         send_cmd_begin();
    bool         send_cmd_stop();
    bool         send_cmd_ping();
    bool         send_data_chunk(const EtherDreamPoint* pts, int count);
    int          flush_acks(int timeout_ms = 5);

    static EtherDreamPoint convert_point(const LaserPoint& p);

    std::string  target_ip_;
    int          point_rate_       = kEDDefaultPPS;
    int          negotiated_pps_   = kEDDefaultPPS;
    int          low_water_        = kEDDefaultLowWater;

    SockFd       tcp_sock_      = kInvalidSock;
    bool         open_          = false;
    bool         streaming_     = false;
    int          buffer_free_   = kEDBufferSize;

    uint8_t      mac_[6]        = {};

    mutable std::mutex  mutex_;
    mutable DacStatus   cached_status_;
};

} // namespace idhmfis
