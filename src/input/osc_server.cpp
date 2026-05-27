// osc_server.cpp — OSC 1.0/1.1 UDP server implementation
//
// Parsing rules:
//   - All OSC data is big-endian
//   - Strings are null-terminated and padded to 4-byte boundaries
//   - Floats and ints are 4 bytes, big-endian IEEE 754
//   - Bundles begin with "#bundle\0" followed by 8-byte timetag
//     then repeated (uint32 size, data) pairs
//   - Type tag strings start with ',' — if missing, we treat all args as nil
//
// The OSC timetag in bundles is parsed but not acted upon (immediate dispatch).
// Nested bundles are fully supported via recursive parsing.

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
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <climits>
#include <algorithm>

#include "osc_server.h"
#include "../core/logger.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Platform helpers (reuse subset from artnet.cpp pattern)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
bool socket_valid(socket_t s) { return s != INVALID_SOCKET; }
void socket_close(socket_t s) { if (socket_valid(s)) ::closesocket(s); }
bool set_nonblocking(socket_t s) {
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}
void osc_winsock_init() {
    static struct W { W(){ WSADATA d; WSAStartup(MAKEWORD(2,2),&d); } ~W(){ WSACleanup(); } } w;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
bool socket_valid(socket_t s) { return s >= 0; }
void socket_close(socket_t s) { if (socket_valid(s)) ::close(s); }
bool set_nonblocking(socket_t s) {
    int f = ::fcntl(s, F_GETFL, 0);
    return f >= 0 && ::fcntl(s, F_SETFL, f | O_NONBLOCK) == 0;
}
void osc_winsock_init() {}
#endif

// Advance cursor to next 4-byte boundary
inline int pad4(int n) { return (n + 3) & ~3; }

// Read big-endian uint32
inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

// Read big-endian float32 via type-punning
inline float read_bef32(const uint8_t* p) {
    uint32_t u = read_be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
OscServer::OscServer(MpscQueue<Message>& out, int port)
    : out_(out), port_(port) {
    osc_winsock_init();
}

OscServer::~OscServer() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
bool OscServer::start() {
    if (running_.load(std::memory_order_relaxed)) return true;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!socket_valid(sock_)) {
        log::error("OSC: socket() failed");
        return false;
    }

    int yes = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::error("OSC: bind() failed on port %d", port_);
        socket_close(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    set_nonblocking(sock_);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&OscServer::recv_loop, this);

    log::info("OSC: listening on UDP port %d", port_);
    return true;
}

void OscServer::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;

    running_.store(false, std::memory_order_release);
    socket_close(sock_);
    sock_ = kInvalidSocket;

    if (thread_.joinable())
        thread_.join();

    log::info("OSC: stopped. Messages=%llu Bundles=%llu Errors=%llu",
              static_cast<unsigned long long>(msgs_.load()),
              static_cast<unsigned long long>(bundles_.load()),
              static_cast<unsigned long long>(errors_.load()));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Receive loop
// ─────────────────────────────────────────────────────────────────────────────
void OscServer::recv_loop() {
    // Max OSC UDP packet = 65535 bytes; practical limit much smaller
    constexpr int kBufSize = 65535;
    std::vector<uint8_t> buf(kBufSize);

    while (running_.load(std::memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_, &rfds);
        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 100000;

#ifdef _WIN32
        int nfds = 0;
#else
        int nfds = static_cast<int>(sock_) + 1;
#endif
        if (::select(nfds, &rfds, nullptr, nullptr, &tv) <= 0) continue;

        sockaddr_in src{};
#ifdef _WIN32
        int src_len = sizeof(src);
#else
        socklen_t src_len = sizeof(src);
#endif
        int n = static_cast<int>(::recvfrom(
            sock_, reinterpret_cast<char*>(buf.data()), kBufSize, 0,
            reinterpret_cast<sockaddr*>(&src), &src_len));

        if (n < 4) continue;

        const uint8_t* data = buf.data();

        // Detect bundle vs message by first byte
        if (n >= 8 && std::memcmp(data, "#bundle\0", 8) == 0) {
            parse_osc_bundle(data, n);
        } else if (data[0] == '/') {
            Message msg;
            if (parse_osc(data, n, msg)) {
                dispatch(std::move(msg));
            } else {
                errors_.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            // Neither bundle nor message — silently discard
            log::debug("OSC: unrecognized packet (first byte=0x%02X)", data[0]);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  OSC message parser
//  Returns true and fills msg on success; returns false on any parse error.
//
//  Layout:
//   [0..?]   Address pattern (null-terminated string, 4-byte padded)
//   [?..?]   Type tag string (null-terminated string starting with ',', 4-byte padded)
//   [?..end] Argument data   (each type is 4-byte aligned except strings)
// ─────────────────────────────────────────────────────────────────────────────
bool OscServer::parse_osc(const uint8_t* data, int len, Message& msg) {
    if (len < 4 || data[0] != '/') return false;

    int cursor = 0;

    // ── Address pattern ──────────────────────────────────────────────────────
    // Find null terminator
    int addr_end = cursor;
    while (addr_end < len && data[addr_end] != '\0') ++addr_end;
    if (addr_end >= len) return false;  // unterminated string

    msg.path = std::string(reinterpret_cast<const char*>(data + cursor),
                           static_cast<size_t>(addr_end - cursor));

    // Advance past null + padding
    cursor = pad4(addr_end + 1);
    if (cursor > len) return false;

    // If no type tag string, treat as message with no arguments (valid in OSC 1.0)
    if (cursor >= len) return true;

    // ── Type tag string ───────────────────────────────────────────────────────
    // Must start with ','
    if (data[cursor] != ',') {
        // Some OSC 1.0 senders omit the type tag; treat args as empty
        return true;
    }

    int tag_start = cursor + 1;  // skip ','
    int tag_end   = tag_start;
    while (tag_end < len && data[tag_end] != '\0') ++tag_end;
    if (tag_end >= len) return false;

    std::string type_tags(reinterpret_cast<const char*>(data + tag_start),
                          static_cast<size_t>(tag_end - tag_start));

    cursor = pad4(tag_end + 1);
    if (cursor > len) return false;

    // ── Arguments ─────────────────────────────────────────────────────────────
    for (char tag : type_tags) {
        if (cursor > len) return false;

        switch (tag) {
            case 'i': {
                if (cursor + 4 > len) return false;
                int32_t v = static_cast<int32_t>(read_be32(data + cursor));
                msg.args_i.push_back(v);
                cursor += 4;
                break;
            }
            case 'f': {
                if (cursor + 4 > len) return false;
                msg.args_f.push_back(read_bef32(data + cursor));
                cursor += 4;
                break;
            }
            case 's': {
                // Null-terminated string, 4-byte padded
                int str_end = cursor;
                while (str_end < len && data[str_end] != '\0') ++str_end;
                if (str_end >= len) return false;
                msg.args_s.emplace_back(
                    reinterpret_cast<const char*>(data + cursor),
                    static_cast<size_t>(str_end - cursor));
                cursor = pad4(str_end + 1);
                break;
            }
            case 'b': {
                // Blob: uint32 size + data (4-byte padded), skip
                if (cursor + 4 > len) return false;
                uint32_t blob_size = read_be32(data + cursor);
                // Guard against integer overflow and truncated blob
                int padded = pad4(static_cast<int>(std::min(blob_size,
                                  static_cast<uint32_t>(INT_MAX / 2))));
                if (cursor + 4 + padded > len) return false;
                cursor += 4 + padded;
                break;
            }
            case 'T':
                // True — no data bytes, push 1 into args_i
                msg.args_i.push_back(1);
                break;
            case 'F':
                // False — no data bytes, push 0 into args_i
                msg.args_i.push_back(0);
                break;
            case 'N':
                // Nil — no data bytes, ignore
                break;
            case 'I':
                // Impulse (bang) — no data bytes, push 1
                msg.args_i.push_back(1);
                break;
            case 'h': {
                // int64 — not in OSC 1.0 but common extension; skip 8 bytes
                if (cursor + 8 > len) return false;
                cursor += 8;
                break;
            }
            case 'd': {
                // float64 — skip 8 bytes
                if (cursor + 8 > len) return false;
                cursor += 8;
                break;
            }
            case 't': {
                // timetag — skip 8 bytes
                if (cursor + 8 > len) return false;
                cursor += 8;
                break;
            }
            default:
                // Unknown type: we can't safely advance cursor, abort argument parsing
                log::debug("OSC: unknown type tag '%c' in %s", tag, msg.path.c_str());
                return true;  // partial success — return what we have so far
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  OSC bundle parser
//  Layout:
//   [0..7]   "#bundle\0"
//   [8..15]  Timetag (uint64 BE: seconds since Jan 1 1900 | sub-second fraction)
//   [16..]   Repeated: (uint32 BE element-size, element-data)
//             element-data is either a bundle or a message
// ─────────────────────────────────────────────────────────────────────────────
int OscServer::parse_osc_bundle(const uint8_t* data, int len) {
    if (len < 16) return 0;
    if (std::memcmp(data, "#bundle\0", 8) != 0) return 0;

    bundles_.fetch_add(1, std::memory_order_relaxed);

    // Timetag at [8..15] — we parse but do not schedule (immediate dispatch)
    // uint64_t timetag = ((uint64_t)read_be32(data+8) << 32) | read_be32(data+12);
    // Timetag 1 = immediate per OSC spec

    int cursor = 16;
    int dispatched = 0;

    while (cursor + 4 <= len) {
        uint32_t raw_element_size = read_be32(data + cursor);
        cursor += 4;

        // BUG #51: validate element_size before use to prevent int overflow
        // and infinite loops. Per OSC spec, element_size must be > 0 and fit
        // within the remaining bundle data.
        if (raw_element_size == 0) {
            log::warn("OSC: zero element_size in bundle, stopping parse");
            break;
        }
        // Guard against values that would overflow int or exceed remaining data
        if (raw_element_size > static_cast<uint32_t>(INT_MAX / 2) ||
            cursor + static_cast<int>(raw_element_size) > len) {
            log::warn("OSC: invalid element_size %u at bundle offset %d, stopping parse",
                      raw_element_size, cursor - 4);
            errors_.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        int elem_len = static_cast<int>(raw_element_size);
        // OSC spec requires element sizes to be 4-byte aligned
        int elem_len_aligned = (elem_len + 3) & ~3;

        const uint8_t* elem = data + cursor;

        if (elem_len >= 8 && std::memcmp(elem, "#bundle\0", 8) == 0) {
            // Nested bundle — recurse
            dispatched += parse_osc_bundle(elem, elem_len);
        } else if (elem_len >= 4 && elem[0] == '/') {
            // OSC message
            Message msg;
            if (parse_osc(elem, elem_len, msg)) {
                dispatch(std::move(msg));
                ++dispatched;
            } else {
                errors_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // Advance by the aligned size; guard against running past end
        if (cursor + elem_len_aligned > len)
            break;
        cursor += elem_len_aligned;
    }

    return dispatched;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Dispatch to queue
// ─────────────────────────────────────────────────────────────────────────────
void OscServer::dispatch(Message&& msg) {
    if (!out_.try_push(std::move(msg))) {
        log::warn("OSC: output queue full, message dropped");
    } else {
        msgs_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace idhmfis
