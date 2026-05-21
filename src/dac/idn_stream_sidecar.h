#pragma once
// IdnStreamSidecar — IDN-Stream broadcast sidecar for IDHMFIS (ILDA IDN-Stream F01).
//
// Broadcasts laser frame data over IDN-Stream (UDP 255.255.255.255:7255) so any
// IDN-capable receiver (IDN-Toolbox, OpenIDN, hardware projectors) can visualise
// the live laser feed alongside the primary DAC output.
//
// Design:
//   - NOT a primary DAC (does not inherit IDac). Opened unconditionally alongside
//     the active primary DAC.
//   - Sends one UDP broadcast per send_points() call, with service_mode=0x01
//     (Laser Projector Graphic Continuous) so receivers identify this as laser.
//   - Runs a periodic IDN-Hello (VOID ping) announce thread for source discovery.
//   - Thread-safe: all public methods are protected by a single mutex.
//
// Wire format reuses IdnPrimaryHeader, IdnChannelHeader, IdnChannelConfig, IdnPoint
// from idn_sender.h — no duplication.
//
// Reference: ILDA IDN-Stream specification F01 (ilda.com/idn.htm)

#include "idn_sender.h"   // IdnPrimaryHeader, IdnChannelHeader, IdnChannelConfig,
                           // IdnPoint, GTS constants, kIdnCmdVoid, kIdnCmdMessage,
                           // kIdnMaxUdpPayload, kIdnFirstChunkHdr, SockFd, kIdnInvalidSock

#include "idac.h"          // PointBuffer, LaserPoint

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

namespace idhmfis {

static constexpr uint16_t kIdnHelloPort          = 7255u;
static constexpr int      kIdnAnnounceIntervalMs = 1000; // announce every 1 s

// ─────────────────────────────────────────────────────────────────────────────
//  IdnStreamSidecar
// ─────────────────────────────────────────────────────────────────────────────
class IdnStreamSidecar {
public:
    IdnStreamSidecar();
    ~IdnStreamSidecar();

    // Bind UDP broadcast socket and start the announce thread.
    bool open();

    // Stop announce thread and close socket.
    void close();

    // True if the socket is open and ready to broadcast.
    bool is_open() const;

    // Encode pts as IDN-Stream (service_mode=laser) and broadcast to 255.255.255.255:7255.
    // Returns number of points successfully sent (0 if not open or pts empty).
    int send_points(const PointBuffer& pts);

private:
    // Announce thread: sends IDN VOID pings every kIdnAnnounceIntervalMs milliseconds
    // so receivers can auto-discover this source.
    void announce_thread_fn();

    // Assemble and send one IDN-Stream datagram.
    // include_config=true on first chunk (carries CCLF + GTS dictionary).
    bool send_packet(const IdnPoint* pts, int count,
                     uint32_t timestamp_us, bool include_config);

    // Convert a LaserPoint to on-wire IdnPoint.
    static IdnPoint convert_point(const LaserPoint& p);

    SockFd            sock_       = kIdnInvalidSock;
    sockaddr_in       bcast_addr_ = {};
    bool              open_       = false;

    uint16_t          stream_seq_ = 0;
    uint16_t          hello_seq_  = 0;

    std::atomic<bool> running_{ false };
    std::thread       announce_thread_;

    mutable std::mutex mutex_;
};

} // namespace idhmfis
