#pragma once
// artnet_poll.h — Art-Net 4 ArtPoll sender (periodic peer-to-peer discovery)
// and standalone ArtPollReply broadcaster.
//
// The existing ArtNetListener already handles *incoming* ArtPoll requests and
// replies with ArtPollReply. This module adds the *outgoing* side:
//   - Broadcasts ArtPoll every 2.5–3 seconds so IDHMFIS appears on networks
//     where no controller issues an active poll (peer-to-peer discovery, §4).
//   - Sends an immediate ArtPollReply broadcast on start (announce presence).
//
// Both are sent to 255.255.255.255:6454 via SO_BROADCAST.
//
// Thread model: one dedicated thread, blocked in select() with 100ms timeout.
// No shared mutable state except the running_ atomic flag.

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <net/if.h>
#  include <ifaddrs.h>
#endif

#include <atomic>
#include <thread>
#include <cstdint>
#include <string>
#include <array>
#include <cstring>

#include "artnet.h"   // ArtPollReplyPacket, opcodes, kArtNetID

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  ArtPollSender
//
//  Periodically broadcasts ArtPoll + ArtPollReply to force discovery on
//  networks where no active poll is issued.  This ensures IDHMFIS appears
//  in grandMA3, Hog 4, MagicQ, and ETC EOS fixture maps immediately.
//
//  Usage:
//      ArtPollSender sender;
//      sender.start(cfg);  // non-blocking; spawns background thread
//      // ...
//      sender.stop();      // joins thread
// ─────────────────────────────────────────────────────────────────────────────
class ArtPollSender {
public:
    ArtPollSender() = default;
    ~ArtPollSender() { stop(); }

    ArtPollSender(const ArtPollSender&)            = delete;
    ArtPollSender& operator=(const ArtPollSender&) = delete;

    // Start the periodic ArtPoll broadcast thread.
    // Uses a separate socket from ArtNetListener so both can coexist.
    // If bind fails, logs a warning and returns false (non-fatal).
    bool start(const ArtNetConfig& cfg = {});

    void stop();

    bool is_running() const {
        return running_.load(std::memory_order_relaxed);
    }

private:
    void poll_loop();

    // Build and send a single ArtPoll broadcast
    void send_artpoll();

    // Build and send a single ArtPollReply broadcast
    void send_artpollreply();

    // Build the ArtPollReply packet
    ArtPollReplyPacket build_poll_reply() const;

    // Retrieve local IP (for binding)
    bool get_local_ip(uint8_t out[4]) const;
    bool get_local_mac(uint8_t out[6]) const;

    ArtNetConfig cfg_;

#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::thread          thread_;
    std::atomic<bool>    running_{false};

    // Interval: 2500–3000 ms between ArtPoll broadcasts (spec §4)
    static constexpr int kPollIntervalMs = 2750;
};

} // namespace idhmfis
