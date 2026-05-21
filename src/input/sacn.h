#pragma once
// sacn.h — sACN E1.31 (ANSI E1.31-2018) sender.
//
// The receiver (SACNListener) is implemented in artnet.h/artnet.cpp and shares
// the same MpscQueue-based DMX delivery pipeline as Art-Net.
//
// This file adds:
//   SACNSender   — sends E1.31 data packets to unicast or multicast addresses.
//   SACNCid      — 128-bit Component Identifier (UUID) persisted to config.
//
// Wire protocol (ANSI E1.31-2018):
//   Preamble / ACN Root Layer
//   E1.31 Framing Layer
//   ANSI E1.17 DMP Layer
//
// The CID is generated once per application instance and cached in
// cfg_dir/sacn_cid.bin. If the file is missing, a random CID is created.
//
// Source name: "IDHMFIS" (up to 64 UTF-8 bytes, zero-padded)
// Priority:    100 (default, as specified in §B9)
// Universes:   1-4 (internal index 0-3 → sACN universe 1-4)

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
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>

#include "../core/types.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  SACNCid — 16-byte Component Identifier (UUID)
// ─────────────────────────────────────────────────────────────────────────────
struct SACNCid {
    std::array<uint8_t, 16> bytes{};

    // Generate a pseudo-random CID using system entropy sources
    static SACNCid generate();

    // Load from file; generates and saves if not found
    static SACNCid load_or_create(const std::string& path);

    // Save to file
    bool save(const std::string& path) const;

    bool operator==(const SACNCid& o) const { return bytes == o.bytes; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  SACNSender — unicast / multicast E1.31 sender
//
//  Sends DMX data as E1.31 Data Packets to a remote node or multicast group.
//  Call send_universe() from any thread; internally uses a UDP socket.
//  No background thread: packets are sent synchronously in send_universe().
// ─────────────────────────────────────────────────────────────────────────────
class SACNSender {
public:
    explicit SACNSender(SACNCid cid);
    ~SACNSender();

    SACNSender(const SACNSender&)            = delete;
    SACNSender& operator=(const SACNSender&) = delete;

    // Open the socket and configure multicast TTL / unicast binding.
    // dest_ip: "" = send to multicast group for each universe (239.255.0.X)
    //          otherwise unicast to the given IP
    bool start(const std::string& dest_ip = "",
               uint8_t priority = 100,
               uint8_t ttl      = 1);
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    // Send one universe of DMX data.
    // universe: 1-63999 (sACN wire universe number)
    // univ:     DmxUniverse with 512 channels
    bool send_universe(int universe, const DmxUniverse& univ);

    const SACNCid& cid() const { return cid_; }

private:
    // Build a complete E1.31 data packet
    // Returns the packet length
    int build_packet(uint8_t* buf, int buf_size,
                     int universe, const uint8_t* dmx, int chan_count,
                     uint8_t sequence_num) const;

    SACNCid cid_;

#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::string dest_ip_;
    uint8_t     priority_    = 100;
    uint8_t     ttl_         = 1;
    std::atomic<bool> running_{false};

    // Per-universe sequence numbers (1-63999 range, sACN is 1-based)
    // Only track up to 64 universes for sACN sender
    static constexpr int kMaxSACNUniverses = 64;
    uint8_t  seq_[kMaxSACNUniverses] = {};

    // sACN constants
    static constexpr int  kPort         = 5568;
    static constexpr char kSourceName[] = "IDHMFIS";

    // ACN Packet Identifier: "ASC-E1.17\0\0\0"
    static constexpr uint8_t kAcnId[12] = {
        0x41,0x53,0x43,0x2D,0x45,0x31,0x2E,0x31,0x37,0x00,0x00,0x00
    };
};

} // namespace idhmfis
