#pragma once
// ArtNet DMX output sender.
// Sends ArtDmx packets (OpCode 0x5000) to a configurable IP or broadcast.
// Used for fixture control: moving lights, fog machines, etc.
//
// Art-Net 4 specification reference: Art-Net 4 Document Revision 1.4dd
// § OpDmx — unicast or broadcast to port 6454.

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
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

namespace idhmfis {

// kArtNetPort is already defined in artnet.h as a static constexpr.
// We reuse the same port constant (6454).
static constexpr int kArtNetSenderPort = 6454;

// Wire-layout ArtDmx packet (OpCode 0x5000).
// Header is 18 bytes; data is 2-512 bytes.
// Note: length field is big-endian per the Art-Net 4 spec §ArtDmx.
#pragma pack(push, 1)
struct ArtDmxPacket {
    uint8_t  id[8]        = {'A','r','t','-','N','e','t',0};
    uint16_t opcode       = 0x5000;  // OpDmx, little-endian
    uint8_t  proto_ver_hi = 0;
    uint8_t  proto_ver_lo = 14;
    uint8_t  sequence     = 0;       // 0 = disabled; 1-255 = enabled
    uint8_t  physical     = 0;       // physical output port (informational)
    uint16_t universe     = 0;       // 15-bit portAddress, little-endian
    uint16_t length       = 0;       // channel count, BIG-endian (use htons)
    uint8_t  data[512]{};
};
#pragma pack(pop)

static_assert(sizeof(ArtDmxPacket) == 18 + 512,
    "ArtDmxPacket must be 530 bytes");

// ─────────────────────────────────────────────────────────────────────────────
//  ArtNetSender — sends ArtDmx packets over UDP
// ─────────────────────────────────────────────────────────────────────────────
class ArtNetSender {
public:
    ArtNetSender();
    ~ArtNetSender();

    // Non-copyable, non-movable
    ArtNetSender(const ArtNetSender&)            = delete;
    ArtNetSender& operator=(const ArtNetSender&) = delete;

    // Set target IP and port. Use "2.255.255.255" for ArtNet broadcast.
    void set_target(const std::string& ip, int port = kArtNetSenderPort);

    // If enabled, sends to 2.255.255.255 (ArtNet broadcast subnet).
    // set_target() overrides this when called explicitly.
    void set_broadcast(bool enabled);

    // Open the UDP socket. Returns false on error.
    bool open();

    // Close the socket.
    void close();

    bool is_open() const;

    // Send 512 DMX channels for one universe.
    // universe: 15-bit ArtNet portAddress (0..32767).
    // count: number of channels to transmit (1-512; padded to 512 if < 512).
    // Returns false on socket error.
    bool send_dmx(int universe, const uint8_t* channels, int count = 512);

    // Convenience overload: always sends full 512-channel universe.
    bool send_universe(int universe, const uint8_t* data);

    uint64_t packets_sent() const {
        return pkt_count_.load(std::memory_order_relaxed);
    }

private:
#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::string target_ip_   = "2.255.255.255";
    int         target_port_ = kArtNetSenderPort;
    bool        broadcast_   = true;

    std::atomic<uint64_t> pkt_count_{0};
    std::mutex send_mtx_;
    uint8_t sequence_{0};
};

} // namespace idhmfis
