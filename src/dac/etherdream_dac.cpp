// etherdream_dac.cpp
// EtherDream DAC driver.
//
// Protocol reference: https://ether-dream.com/protocol.html
// The EtherDream buffers 1800 points internally.  We use low-water-mark
// flow control: keep at least `low_water_mark_` points ahead in the buffer.

#include "etherdream_dac.h"
#include "core/logger.h"

#include <cstring>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
// Winsock must be initialised before use.  We do it lazily.
namespace {
struct WsaInit {
    WSADATA wsa;
    WsaInit() { WSAStartup(MAKEWORD(2, 2), &wsa); }
    ~WsaInit() { WSACleanup(); }
};
static WsaInit g_wsa;

inline void close_socket(SockFd s) { closesocket(s); }
inline bool would_block() {
    return WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAETIMEDOUT;
}
} // anonymous namespace

#else
#include <fcntl.h>
#include <errno.h>
inline void close_socket(SockFd s) { ::close(s); }
inline bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────
EtherDreamDac::EtherDreamDac(std::string target_ip)
    : target_ip_(std::move(target_ip))
{}

EtherDreamDac::EtherDreamDac(const EtherDreamBroadcast& bcast, const std::string& ip)
    : target_ip_(ip)
    , point_rate_(static_cast<int>(bcast.max_point_rate))
{
    std::memcpy(mac_, bcast.mac, 6);
}

EtherDreamDac::~EtherDreamDac() {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  UDP discovery
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::pair<std::string, EtherDreamBroadcast>>
EtherDreamDac::discover(int timeout_ms) {
    std::vector<std::pair<std::string, EtherDreamBroadcast>> results;

    SockFd sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidSock) return results;

    // Enable address reuse
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in local{};
    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port        = htons(static_cast<uint16_t>(kEDDiscoveryPort));

    if (::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
        close_socket(sock);
        return results;
    }

    // Set receive timeout
#ifdef _WIN32
    DWORD tv_ms = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
#else
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    while (Clock::now() < deadline) {
        EtherDreamBroadcast pkt{};
        sockaddr_in sender{};
#ifdef _WIN32
        int sender_len = sizeof(sender);
#else
        socklen_t sender_len = sizeof(sender);
#endif
        int r = static_cast<int>(::recvfrom(sock,
            reinterpret_cast<char*>(&pkt), sizeof(pkt),
            0,
            reinterpret_cast<sockaddr*>(&sender), &sender_len));

        if (r < static_cast<int>(sizeof(EtherDreamBroadcast))) continue;
        if (std::memcmp(pkt.magic, kEDMagic, kEDMagicLen) != 0) continue;

        char ip_str[INET_ADDRSTRLEN]{};
        ::inet_ntop(AF_INET, &sender.sin_addr, ip_str, sizeof(ip_str));
        results.push_back({ std::string(ip_str), pkt });
    }

    close_socket(sock);
    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TCP connect / disconnect
// ─────────────────────────────────────────────────────────────────────────────
bool EtherDreamDac::tcp_connect() {
    if (target_ip_.empty()) {
        log::warn("EtherDream: no target IP — run discover() first");
        return false;
    }

    tcp_sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_sock_ == kInvalidSock) return false;

    // Set receive timeout (100 ms)
#ifdef _WIN32
    DWORD tv_ms = 100;
    setsockopt(tcp_sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
    setsockopt(tcp_sock_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
#else
    timeval tv{ 0, 100000 };
    setsockopt(tcp_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(tcp_sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // Disable Nagle for lower latency
    int no_delay = 1;
    setsockopt(tcp_sock_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(kEDDataPort));
    ::inet_pton(AF_INET, target_ip_.c_str(), &addr.sin_addr);

    if (::connect(tcp_sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        log::warn("EtherDream: TCP connect to %s failed", target_ip_.c_str());
        close_socket(tcp_sock_);
        tcp_sock_ = kInvalidSock;
        return false;
    }

    // On connect the EtherDream immediately sends a DacStatus response
    EtherDreamResponse resp{};
    if (!recv_response(resp, 500)) {
        log::warn("EtherDream: no initial status response");
        close_socket(tcp_sock_);
        tcp_sock_ = kInvalidSock;
        return false;
    }

    buffer_free_ = kEDBufferSize - resp.status.buffer_fullness;
    // MAC is known only if populated from a discovery broadcast; leave as-is.
    log::info("EtherDream: connected to %s (buffer_free=%d)",
              target_ip_.c_str(), buffer_free_);
    return true;
}

void EtherDreamDac::tcp_disconnect() {
    if (tcp_sock_ != kInvalidSock) {
        close_socket(tcp_sock_);
        tcp_sock_ = kInvalidSock;
    }
    streaming_ = false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Raw send / receive helpers
// ─────────────────────────────────────────────────────────────────────────────
bool EtherDreamDac::send_raw(const void* data, int len) {
    const char* ptr = reinterpret_cast<const char*>(data);
    int remaining = len;
    while (remaining > 0) {
        int sent = static_cast<int>(::send(tcp_sock_, ptr, remaining, 0));
        if (sent <= 0) return false;
        ptr       += sent;
        remaining -= sent;
    }
    return true;
}

bool EtherDreamDac::recv_response(EtherDreamResponse& out, int timeout_ms) {
    (void)timeout_ms; // handled by socket option
    int r = static_cast<int>(::recv(tcp_sock_,
        reinterpret_cast<char*>(&out), sizeof(out), 0));
    return r == static_cast<int>(sizeof(out));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Protocol command helpers
// ─────────────────────────────────────────────────────────────────────────────
bool EtherDreamDac::send_cmd_prepare() {
    uint8_t cmd = kEDCmdPrepare;
    if (!send_raw(&cmd, 1)) return false;
    EtherDreamResponse resp{};
    if (!recv_response(resp)) return false;
    return resp.response == kEDRespAck;
}

bool EtherDreamDac::send_cmd_begin() {
    EtherDreamBeginCmd cmd{};
    cmd.command        = kEDCmdBegin;
    cmd.low_water_mark = static_cast<uint16_t>(low_water_);
    cmd.point_rate     = static_cast<uint32_t>(point_rate_);
    if (!send_raw(&cmd, sizeof(cmd))) return false;
    EtherDreamResponse resp{};
    if (!recv_response(resp)) return false;
    return resp.response == kEDRespAck;
}

bool EtherDreamDac::send_cmd_stop() {
    uint8_t cmd = kEDCmdStop;
    if (!send_raw(&cmd, 1)) return false;
    EtherDreamResponse resp{};
    recv_response(resp, 50); // best-effort
    return true;
}

bool EtherDreamDac::send_cmd_ping() {
    uint8_t cmd = kEDCmdPing;
    if (!send_raw(&cmd, 1)) return false;
    EtherDreamResponse resp{};
    if (!recv_response(resp, 50)) return false;
    // Update buffer_free_ from the ping response so the send loop can make progress.
    buffer_free_ = kEDBufferSize - static_cast<int>(resp.status.buffer_fullness);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_data_chunk — DATA command + raw point array
// ─────────────────────────────────────────────────────────────────────────────
bool EtherDreamDac::send_data_chunk(const EtherDreamPoint* pts, int count) {
    EtherDreamDataCmd hdr{};
    hdr.command    = kEDCmdData;
    hdr.num_points = static_cast<uint16_t>(count);

    // Send header then data in two writes (avoids one big copy)
    if (!send_raw(&hdr, sizeof(hdr))) return false;
    if (!send_raw(pts, count * static_cast<int>(sizeof(EtherDreamPoint))))
        return false;

    EtherDreamResponse resp{};
    if (!recv_response(resp)) return false;
    if (resp.response != kEDRespAck) return false;

    buffer_free_ = kEDBufferSize - static_cast<int>(resp.status.buffer_fullness);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  convert_point
// ─────────────────────────────────────────────────────────────────────────────
EtherDreamPoint EtherDreamDac::convert_point(const LaserPoint& p) {
    EtherDreamPoint ep{};
    ep.x = p.x;
    ep.y = p.y;
    if (p.blanked) {
        ep.r = ep.g = ep.b = ep.i = 0;
    } else {
        // Scale 0–255 to 0–65535
        ep.r = static_cast<uint16_t>(p.r * 257u);
        ep.g = static_cast<uint16_t>(p.g * 257u);
        ep.b = static_cast<uint16_t>(p.b * 257u);
        ep.i = 0xFFFF;
    }
    ep.u1 = 0;
    ep.u2 = 0;
    return ep;
}

// ─────────────────────────────────────────────────────────────────────────────
//  open / close
// ─────────────────────────────────────────────────────────────────────────────
bool EtherDreamDac::open() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (open_) return true;

    if (target_ip_.empty()) {
        // Try auto-discovery
        log::info("EtherDream: scanning for devices...");
        auto devices = EtherDreamDac::discover(2000);
        if (devices.empty()) {
            cached_status_.error = "no EtherDream found";
            log::warn("EtherDream: no device found on network");
            return false;
        }
        target_ip_ = devices[0].first;
        std::memcpy(mac_, devices[0].second.mac, 6);
        log::info("EtherDream: auto-discovered %s", target_ip_.c_str());
    }

    if (!tcp_connect()) {
        cached_status_.error = "TCP connect failed";
        return false;
    }

    // Protocol handshake: prepare → begin
    if (!send_cmd_prepare()) {
        log::warn("EtherDream: PREPARE rejected");
        tcp_disconnect();
        cached_status_.error = "PREPARE rejected";
        return false;
    }

    if (!send_cmd_begin()) {
        log::warn("EtherDream: BEGIN rejected");
        tcp_disconnect();
        cached_status_.error = "BEGIN rejected";
        return false;
    }

    streaming_       = true;
    open_            = true;
    negotiated_pps_  = point_rate_; // rate accepted by BEGIN response
    cached_status_.connected     = true;
    cached_status_.device_name   = "EtherDream @ " + target_ip_;
    cached_status_.driver_version = "1.0";
    cached_status_.point_rate    = point_rate_;
    cached_status_.buffer_free   = buffer_free_;
    cached_status_.error         = "";
    log::info("EtherDream: streaming started at %d pps (negotiated)", point_rate_);
    return true;
}

void EtherDreamDac::close() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!open_) return;

    if (streaming_) send_cmd_stop();
    tcp_disconnect();

    open_      = false;
    streaming_ = false;
    cached_status_ = DacStatus{};
    log::info("EtherDream: closed");
}

bool EtherDreamDac::is_open() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return open_;
}

DacStatus EtherDreamDac::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    cached_status_.buffer_free = buffer_free_;
    return cached_status_;
}

bool EtherDreamDac::set_point_rate(int pps) {
    if (pps < kEDMinPPS || pps > kEDMaxPPS) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    if (pps == point_rate_) return true; // no-op: rate unchanged, avoids redundant BEGIN
    point_rate_ = pps;
    cached_status_.point_rate = pps;
    // Re-issue BEGIN with new rate if already streaming
    if (streaming_) {
        send_cmd_stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (send_cmd_prepare()) {
            if (send_cmd_begin()) {
                negotiated_pps_ = pps; // hardware accepted the new rate
            }
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_points
// ─────────────────────────────────────────────────────────────────────────────
int EtherDreamDac::send_points(const PointBuffer& pts) {
    // H-21: use unique_lock so we can release the mutex around blocking I/O
    // (send_cmd_ping) and sleep_for, which previously held mutex_ across an
    // up-to-10 ms sleep, stalling any concurrent accessor (e.g. status reads
    // from the UI thread).
    std::unique_lock<std::mutex> lk(mutex_);
    if (!open_ || pts.empty()) return 0;

    // Convert
    std::vector<EtherDreamPoint> converted;
    converted.reserve(pts.size());
    for (const auto& p : pts)
        converted.push_back(convert_point(p));

    int sent      = 0;
    int remaining = static_cast<int>(converted.size());

    int ping_retries = 0;
    static constexpr int kMaxPingRetries = 20; // ~10 ms of 500 µs sleeps before giving up
    while (remaining > 0) {
        // Respect low-water-mark: don't flood the buffer
        if (buffer_free_ <= 0) {
            // Release lock during sleep so other threads aren't blocked for up
            // to 10 ms.  Re-check open_ after reacquiring.
            lk.unlock();
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            lk.lock();

            if (!open_) break; // Close() called while we were sleeping

            // Ping to refresh buffer_free_
            if (!send_cmd_ping()) {
                log::warn("EtherDream: ping failed, assuming disconnect");
                open_ = false;
                streaming_ = false;
                cached_status_.connected = false;
                cached_status_.error = "ping timeout";
                // BUG #36: close the TCP socket immediately so the fd is not
                // leaked until the next explicit close() call.
                tcp_disconnect();
                break;
            }
        }

        int space  = std::max(0, buffer_free_);
        int chunk  = std::min({ remaining, space, kEDMaxPoints });
        if (chunk <= 0) {
            if (++ping_retries > kMaxPingRetries) {
                log::warn("EtherDream: buffer full after %d pings, dropping %d points",
                          kMaxPingRetries, remaining);
                break;
            }
            continue;
        }
        ping_retries = 0;

        if (!send_data_chunk(converted.data() + sent, chunk)) {
            log::warn("EtherDream: DATA failed, assuming disconnect");
            open_ = false;
            streaming_ = false;
            cached_status_.connected = false;
            cached_status_.error = "send_data failed";
            break;
        }

        sent      += chunk;
        remaining -= chunk;
    }

    cached_status_.buffer_free = buffer_free_;
    return sent;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IDacOutput implementation
// ─────────────────────────────────────────────────────────────────────────────
int EtherDreamDac::negotiated_pps() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return negotiated_pps_;
}

std::string EtherDreamDac::id() const {
    // Use "IP:7765" as the unique identifier
    return target_ip_ + ":7765";
}

std::string EtherDreamDac::name() const {
    return "EtherDream @ " + target_ip_;
}

bool EtherDreamDac::send_frame(const PointBuffer& pts, int target_pps) {
    // Negotiate rate: clamp to EtherDream hardware limits.
    // Always call set_point_rate — it now early-returns cheaply when the rate
    // is already correct, eliminating the prior TOCTOU between the check-under-lock
    // and the re-acquisition inside set_point_rate.
    int clamped = std::clamp(target_pps, kEDMinPPS, kEDMaxPPS);
    set_point_rate(clamped);
    int sent = send_points(pts);
    return sent == static_cast<int>(pts.size());
}

// ─────────────────────────────────────────────────────────────────────────────
//  mac_string
// ─────────────────────────────────────────────────────────────────────────────
std::string EtherDreamDac::mac_string() const {
    std::ostringstream oss;
    for (int i = 0; i < 6; ++i) {
        if (i) oss << ':';
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(mac_[i]);
    }
    return oss.str();
}

} // namespace idhmfis
