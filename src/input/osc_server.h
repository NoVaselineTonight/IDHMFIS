#pragma once
// OSC 1.0 / 1.1 UDP server.
//
// Parses OSC messages and bundles received on a UDP socket and pushes
// decoded Message structs into an MpscQueue.
//
// OSC wire format (big-endian, 4-byte alignment):
//   Address pattern:  "/path/to/method\0" + padding to 4-byte boundary
//   Type tag string:  ",fff\0"            + padding to 4-byte boundary
//   Arguments:        each 4-byte aligned (float=4, int=4, string=variable)
//
// OSC bundle format:
//   "#bundle\0" (8 bytes)
//   Timetag    (8 bytes, seconds+fractions since 1 Jan 1900)
//   Size+content pairs (uint32 BE size followed by OSC message or nested bundle)
//
// Supported argument types:
//   'i' — int32   (big-endian)
//   'f' — float32 (big-endian IEEE 754)
//   's' — string  (null-terminated, 4-byte padded)
//   'b' — blob    (skipped; not forwarded)
//   'T' — True    (no data; args_i gets 1)
//   'F' — False   (no data; args_i gets 0)
//   'N' — Nil     (no data; ignored)
//   'I' — Impulse (no data; args_i gets 1)

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "../core/spsc_queue.h"

namespace idhmfis {

class OscServer {
public:
    // ─────────────────────────────────────────────────────────────────────
    //  Decoded OSC message
    // ─────────────────────────────────────────────────────────────────────
    struct Message {
        std::string              path;    // OSC address pattern e.g. "/laser/speed"
        std::vector<float>       args_f;  // float32 arguments
        std::vector<int32_t>     args_i;  // int32 + bool arguments
        std::vector<std::string> args_s;  // string arguments
    };

    // ─────────────────────────────────────────────────────────────────────
    //  Construction / lifecycle
    // ─────────────────────────────────────────────────────────────────────
    explicit OscServer(MpscQueue<Message>& out, int port = 8000);
    ~OscServer();

    OscServer(const OscServer&)            = delete;
    OscServer& operator=(const OscServer&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    int  port() const { return port_; }

    // Stats
    uint64_t messages_received() const { return msgs_.load(std::memory_order_relaxed); }
    uint64_t bundles_received()  const { return bundles_.load(std::memory_order_relaxed); }
    uint64_t parse_errors()      const { return errors_.load(std::memory_order_relaxed); }

private:
    void recv_loop();

    // Returns true on success and fills msg
    bool parse_osc(const uint8_t* data, int len, Message& msg);

    // Parse and dispatch a bundle; returns number of messages dispatched
    int  parse_osc_bundle(const uint8_t* data, int len);

    // Dispatch a single parsed message to the queue
    void dispatch(Message&& msg);

    MpscQueue<Message>& out_;
    int                 port_;

#ifdef _WIN32
    SOCKET sock_ = INVALID_SOCKET;
#else
    int sock_ = -1;
#endif

    std::thread          thread_;
    std::atomic<bool>    running_{false};
    std::atomic<uint64_t>msgs_{0};
    std::atomic<uint64_t>bundles_{0};
    std::atomic<uint64_t>errors_{0};
};

} // namespace idhmfis
