#pragma once
// Art-Net 4 listener with ArtPoll/ArtPollReply support.
// Also contains the sACN E1.31 listener.
//
// Art-Net 4 specification reference: Art-Net 4 Document Revision 1.4dd
// Enttec/Artistic Licence  -  https://art-net.org.uk
//
// All sockets use non-blocking I/O with select() timeouts.
// Universe calculation: 15-bit portAddress = (Net << 8) | SubUni
//   where SubUni = (Subnet << 4) | Universe

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
#  include <net/if.h>
#  include <ifaddrs.h>
#endif

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <functional>

#include "../core/types.h"
#include "../core/spsc_queue.h"
#include "../core/timer.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Art-Net 4 opcodes (little-endian on wire)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint16_t kArtOpPoll        = 0x2000;
static constexpr uint16_t kArtOpPollReply   = 0x2100;
static constexpr uint16_t kArtOpDmx         = 0x5000;
static constexpr uint16_t kArtOpNzs         = 0x5100;
static constexpr uint16_t kArtOpSync        = 0x5200;
static constexpr uint16_t kArtOpTimeCode    = 0x9700;
static constexpr uint16_t kArtOpCommand     = 0x2400;

static constexpr uint8_t  kArtNetProtVerHi  = 0;
static constexpr uint8_t  kArtNetProtVerLo  = 14;

// Art-Net header ID: "Art-Net\0" (8 bytes)
static constexpr uint8_t kArtNetID[8] = {
    'A','r','t','-','N','e','t','\0'
};

// ─────────────────────────────────────────────────────────────────────────────
//  ArtPollReply packet — exact wire layout (239 bytes)
//  Follows Art-Net 4 spec Table 3  (§6 ArtPollReply)
//  All multi-byte integers are little-endian EXCEPT Port(s)Address which is BE.
// ─────────────────────────────────────────────────────────────────────────────
#pragma pack(push, 1)
struct ArtPollReplyPacket {
    // Header
    uint8_t  id[8];           // "Art-Net\0"
    uint16_t op_code;         // 0x2100 LE
    uint8_t  ip_address[4];   // Node's IP in host byte order
    uint16_t port;            // 0x1936 (6454) LE
    uint16_t vers_info;       // Firmware version BE
    uint8_t  net_switch;      // Bits 14-8 of 15-bit port-address
    uint8_t  sub_switch;      // Bits 7-4 of 15-bit port-address
    uint16_t oem;             // OEM code (Artistic Licence)
    uint8_t  ubea_version;    // UBEA version (0 = not present)
    uint8_t  status1;         // General status register
    uint16_t esta_man;        // Manufacturer code LE
    uint8_t  short_name[18];  // Short node name (null-terminated)
    uint8_t  long_name[64];   // Long node name (null-terminated)
    uint8_t  node_report[64]; // Node report (null-terminated)
    uint16_t num_ports;       // Number of input/output ports (max 4) LE
    uint8_t  port_types[4];   // Port type array (see spec Table 10)
    uint8_t  good_input[4];   // Input status of each port
    uint8_t  good_output_a[4];// Output status of each port
    uint8_t  sw_in[4];        // Bits 3-0 of Port-Address for each of the 4 inputs
    uint8_t  sw_out[4];       // Bits 3-0 of Port-Address for each of the 4 outputs
    uint8_t  acn_priority;    // Deprecated; set to 0
    uint8_t  sw_macro;        // Deprecated; set to 0
    uint8_t  sw_remote;       // Deprecated; set to 0
    uint8_t  spare[3];        // Unused, transmit as 0
    uint8_t  style;           // Equipment style (0x00 = StNode)
    uint8_t  mac[6];          // MAC address of node
    uint8_t  bind_ip[4];      // If DHCP, this holds the elected IP
    uint8_t  bind_index;      // Index of parent ArtPollReply (0 = root)
    uint8_t  status2;         // Bit flags (see spec Table 11)
    uint8_t  good_output_b[4];// Output status B (Art-Net 4 extension)
    uint8_t  status3;         // Bit flags (Art-Net 4 extension)
    uint8_t  default_resp_uid[6]; // RDM default responder UID
    uint8_t  user[2];         // User data (0)
    uint16_t refresh_rate;    // Refresh rate (0 = default / 44Hz)
    uint8_t  filler[11];      // Set to zero to allow future expansion (padding to 239)
};
#pragma pack(pop)

static_assert(sizeof(ArtPollReplyPacket) == 239,
    "ArtPollReplyPacket must be exactly 239 bytes (Art-Net 4 spec §6)");

// ─────────────────────────────────────────────────────────────────────────────
//  ArtTimeCode — parsed SMPTE timecode
// ─────────────────────────────────────────────────────────────────────────────
struct ArtTimeCode {
    uint8_t frames;   // 0-29
    uint8_t seconds;  // 0-59
    uint8_t minutes;  // 0-59
    uint8_t hours;    // 0-23
    uint8_t type;     // 0=24fps 1=25fps 2=29.97fps 3=30fps
};

// ─────────────────────────────────────────────────────────────────────────────
//  ArtNetConfig
// ─────────────────────────────────────────────────────────────────────────────
struct ArtNetConfig {
    std::string bind_ip    = "0.0.0.0";
    int         port       = 6454;
    std::string short_name = "IDHMFIS";
    std::string long_name  = "IDHMFIS Laser Show Software";
    uint16_t    esta_man   = 0x7FF0;   // unregistered manufacturer code
    uint16_t    esta_dev   = 0x0001;
    std::string node_report = "IDHMFIS v0.1.0";
    uint8_t     net        = 0;        // Net (bits 14-8 of port-address)
    uint8_t     sub_net    = 0;        // Sub-net (bits 7-4 of port-address)
};

// ─────────────────────────────────────────────────────────────────────────────
//  ArtNetListener — receives ArtDMX and responds to ArtPoll
// ─────────────────────────────────────────────────────────────────────────────
class ArtNetListener {
public:
    explicit ArtNetListener(MpscQueue<std::pair<int, DmxUniverse>>& dmx_out);
    ~ArtNetListener();

    // Non-copyable, non-movable
    ArtNetListener(const ArtNetListener&)            = delete;
    ArtNetListener& operator=(const ArtNetListener&) = delete;

    bool start(const ArtNetConfig& cfg = {});
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Statistics
    uint64_t packets_received()  const { return pkts_.load(std::memory_order_relaxed); }
    double   last_packet_age_ms() const;   // ms since last ArtDMX packet
    int      active_universe_count() const;

    // Latency stats (engine → artnet processing latency)
    double   latency_p99_ms() const  { return latency_stats_.p99();  }
    double   latency_mean_ms() const { return latency_stats_.mean(); }

    // Last parsed timecode (valid only if has_timecode() returns true)
    bool        has_timecode() const { return has_tc_.load(std::memory_order_acquire); }
    ArtTimeCode timecode()     const {
        std::lock_guard<std::mutex> lk(tc_mutex_);
        return last_tc_;
    }

private:
    void recv_loop();
    void handle_artdmx(const uint8_t* data, int len, const sockaddr_in& src);
    void handle_artpoll(const uint8_t* data, int len, const sockaddr_in& src);
    void handle_arttimecode(const uint8_t* data, int len);
    void send_artpollreply(const sockaddr_in& dest);

    // Build a reply packet from current config + runtime state
    ArtPollReplyPacket build_poll_reply(const sockaddr_in& dest) const;

    // Retrieve local IP bound to the socket
    bool get_local_ip(uint8_t out[4]) const;
    bool get_local_mac(uint8_t out[6]) const;

    MpscQueue<std::pair<int, DmxUniverse>>& out_;
    ArtNetConfig cfg_;

#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<uint64_t>pkts_{0};
    std::atomic<int64_t> last_pkt_ns_{0};  // steady_clock ns

    std::array<std::atomic<bool>, 128> universe_active_{};
    std::array<uint8_t, 128>          last_sequence_{};

    // Latency measurement: time from recv() returning to queue push
    mutable RollingStats<256> latency_stats_;

    // Timecode — written by recv_loop, read by any caller; protected by tc_mutex_.
    mutable std::mutex tc_mutex_;
    std::atomic<bool>  has_tc_{false};
    ArtTimeCode        last_tc_{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  sACN / E1.31 listener (ANSI E1.31-2018)
//  Supports unicast and per-universe multicast join.
// ─────────────────────────────────────────────────────────────────────────────
class SACNListener {
public:
    explicit SACNListener(MpscQueue<std::pair<int, DmxUniverse>>& dmx_out);
    ~SACNListener();

    SACNListener(const SACNListener&)            = delete;
    SACNListener& operator=(const SACNListener&) = delete;

    // bind_ip: "" or "0.0.0.0" for all interfaces
    // universes: list of universes to subscribe to (multicast);
    //            empty = unicast-only on port 5568
    bool start(const std::string& bind_ip = "",
               const std::vector<int>& universes = {});
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    uint64_t packets_received() const { return pkts_.load(std::memory_order_relaxed); }

private:
    void recv_loop();
    void handle_data_packet(const uint8_t* d, int len);

    // Join/leave multicast for the given E1.31 universe (239.255.X.Y)
    bool join_multicast(int universe);

    MpscQueue<std::pair<int, DmxUniverse>>& out_;
    std::string bind_ip_;
    std::vector<int> universes_;

#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<uint64_t>pkts_{0};
};

} // namespace idhmfis
