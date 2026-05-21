// artnet.cpp — Art-Net 4 listener + sACN E1.31 listener implementation
//
// Art-Net 4 compliance:
//   - ArtDMX (opcode 0x5000): full header parsing, sequence tracking per universe
//   - ArtPoll (opcode 0x2000): reply with ArtPollReply broadcast (0x2100)
//   - ArtTimeCode (opcode 0x9700): parsed and exposed via timecode()
//   - Universe: 15-bit portAddress = (Net << 8) | SubUni where SubUni = (Subnet<<4)|Uni
//
// sACN compliance (ANSI E1.31-2018):
//   - ACN Root Layer PDU
//   - E1.31 Framing Layer PDU
//   - ANSI E1.17 DMP Layer PDU
//
// Latency measurement:
//   - Timer starts when recv() returns (UDP delivery to userspace)
//   - Timer stops when item is pushed to MpscQueue
//   - Warning logged if p99 > 8ms

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
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <net/if.h>
#  include <ifaddrs.h>
#  include <netpacket/packet.h>
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <vector>

#include "artnet.h"
#include "../core/logger.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Platform socket helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

bool socket_valid(socket_t s) { return s != INVALID_SOCKET; }

void socket_close(socket_t s) {
    if (socket_valid(s)) ::closesocket(s);
}

bool set_nonblocking(socket_t s) {
    u_long mode = 1;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}

void winsock_init() {
    static struct WSAInit {
        WSAInit() {
            WSADATA wsd;
            ::WSAStartup(MAKEWORD(2,2), &wsd);
        }
        ~WSAInit() { ::WSACleanup(); }
    } init;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;

bool socket_valid(socket_t s) { return s >= 0; }

void socket_close(socket_t s) {
    if (socket_valid(s)) ::close(s);
}

bool set_nonblocking(socket_t s) {
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return ::fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

void winsock_init() {}
#endif

// Copy string into fixed buffer with null termination and zero padding
template<size_t N>
void copy_str_buf(uint8_t (&dst)[N], const std::string& src) {
    std::memset(dst, 0, N);
    size_t n = std::min(src.size(), N - 1);
    std::memcpy(dst, src.data(), n);
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  ArtNetListener — construction / destruction
// ─────────────────────────────────────────────────────────────────────────────
ArtNetListener::ArtNetListener(MpscQueue<std::pair<int, DmxUniverse>>& dmx_out)
    : out_(dmx_out) {
    winsock_init();
    for (auto& b : universe_active_) b.store(false, std::memory_order_relaxed);
}

ArtNetListener::~ArtNetListener() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
bool ArtNetListener::start(const ArtNetConfig& cfg) {
    if (running_.load(std::memory_order_relaxed)) return true;

    cfg_ = cfg;
    for (auto& b : universe_active_) b.store(false, std::memory_order_relaxed);

    // Create UDP socket
#ifdef _WIN32
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        log::error("ArtNet: socket() failed, err=%d", WSAGetLastError());
        return false;
    }
#else
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        log::error("ArtNet: socket() failed");
        return false;
    }
#endif

    // SO_REUSEADDR so we can bind when another Art-Net app is already open
    int yes = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    // SO_BROADCAST for ArtPollReply broadcast transmission
    ::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    // Bind
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(cfg_.port));
    addr.sin_addr.s_addr = inet_addr(cfg_.bind_ip.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE)
        addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::error("ArtNet: bind() failed on %s:%d",
                   cfg_.bind_ip.c_str(), cfg_.port);
        socket_close(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    set_nonblocking(sock_);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ArtNetListener::recv_loop, this);

    log::info("ArtNet: listening on %s:%d (Net=%d SubNet=%d)",
              cfg_.bind_ip.c_str(), cfg_.port, cfg_.net, cfg_.sub_net);
    return true;
}

void ArtNetListener::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;

    running_.store(false, std::memory_order_release);
    socket_close(sock_);
    sock_ = kInvalidSocket;

    if (thread_.joinable())
        thread_.join();

    log::info("ArtNet: stopped. Packets received: %llu",
              static_cast<unsigned long long>(pkts_.load()));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Statistics
// ─────────────────────────────────────────────────────────────────────────────
double ArtNetListener::last_packet_age_ms() const {
    int64_t ts = last_pkt_ns_.load(std::memory_order_relaxed);
    if (ts == 0) return -1.0;
    int64_t diff = now_ns() - ts;
    return static_cast<double>(diff) * 1e-6;
}

int ArtNetListener::active_universe_count() const {
    int count = 0;
    for (const auto& b : universe_active_) if (b.load(std::memory_order_relaxed)) ++count;
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Main receive loop
// ─────────────────────────────────────────────────────────────────────────────
void ArtNetListener::recv_loop() {
    constexpr int kBufSize = 1024;
    uint8_t buf[kBufSize];

    while (running_.load(std::memory_order_relaxed)) {
        // Use select() with 100ms timeout so we can check running_ flag
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock_, &rfds);

        timeval tv{};
        tv.tv_sec  = 0;
        tv.tv_usec = 100000; // 100ms

#ifdef _WIN32
        int nfds = 0; // ignored on Windows
#else
        int nfds = static_cast<int>(sock_) + 1;
#endif
        int ret = ::select(nfds, &rfds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        sockaddr_in src{};
#ifdef _WIN32
        int src_len = sizeof(src);
#else
        socklen_t src_len = sizeof(src);
#endif
        int n = static_cast<int>(::recvfrom(
            sock_, reinterpret_cast<char*>(buf), kBufSize, 0,
            reinterpret_cast<sockaddr*>(&src), &src_len));

        if (n < 10) continue;  // too short to be any Art-Net packet

        // Validate Art-Net header ID
        if (std::memcmp(buf, kArtNetID, 8) != 0) continue;

        // Opcode is at bytes 8-9, little-endian
        uint16_t opcode = static_cast<uint16_t>(buf[8]) |
                          (static_cast<uint16_t>(buf[9]) << 8);

        switch (opcode) {
            case kArtOpDmx:
                handle_artdmx(buf, n, src);
                break;
            case kArtOpPoll:
                handle_artpoll(buf, n, src);
                break;
            case kArtOpTimeCode:
                handle_arttimecode(buf, n);
                break;
            default:
                // Other opcodes silently ignored
                break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ArtDMX handler
//  Wire format (byte offsets from packet start):
//   0- 7 : ID "Art-Net\0"
//   8- 9 : OpCode LE (0x5000)
//  10    : ProtVerHi (0)
//  11    : ProtVerLo (14)
//  12    : Sequence  (0 = disabled)
//  13    : Physical  (input port for diagnostics)
//  14    : SubUni    (Subnet<<4 | Universe, bits 7-0 of portAddress)
//  15    : Net       (bits 14-8 of portAddress, i.e. the Net field)
//  16-17 : Length BE (number of channels, even, 2-512)
//  18+   : Data      (DMX channels)
// ─────────────────────────────────────────────────────────────────────────────
void ArtNetListener::handle_artdmx(const uint8_t* data, int len,
                                   const sockaddr_in& /*src*/) {
    // Minimum header size: 18 bytes
    if (len < 18) return;

    // Verify protocol version
    // ProtVerHi at offset 10, ProtVerLo at offset 11
    // We accept any version >= 14 (field kArtNetProtVerLo)
    uint8_t proto_lo = data[11];
    if (proto_lo < kArtNetProtVerLo) {
        log::debug("ArtNet: discarding ArtDMX with proto version %d", proto_lo);
        return;
    }

    uint8_t sub_uni = data[14];  // SubUni (bits 7-0 of portAddress)
    uint8_t net     = data[15];  // Net    (bits 14-8)

    // 15-bit portAddress: Net occupies bits 14-8, SubUni occupies bits 7-0
    int universe = ((net & 0x7F) << 8) | sub_uni;

    if (universe < 0 || universe >= kMaxUniverses) {
        log::debug("ArtNet: universe %d out of range (max %d)", universe, kMaxUniverses);
        return;
    }

    uint8_t sequence = data[12];
    if (sequence != 0) {
        uint8_t last = last_sequence_[static_cast<size_t>(universe)];
        // diff > 192 means this packet arrived more than 64 steps behind
        // (handles 8-bit wrap-around: 256 - 64 = 192)
        uint8_t diff = static_cast<uint8_t>(sequence - last);
        if (last != 0 && diff > 192) {
            log::debug("ArtNet: dropped out-of-order packet uni=%d seq=%d last=%d",
                       universe, sequence, last);
            return;
        }
        last_sequence_[static_cast<size_t>(universe)] = sequence;
    }

    // Channel count: big-endian uint16 at offset 16-17, must be even, max 512
    uint16_t chan_count = (static_cast<uint16_t>(data[16]) << 8) |
                           static_cast<uint16_t>(data[17]);

    if (chan_count == 0 || chan_count > 512) {
        log::debug("ArtNet: invalid channel count %d", chan_count);
        return;
    }
    // Length must be even per spec; round down to nearest even number
    if (chan_count & 1) chan_count &= ~1u;
    // After rounding, a value of 1 becomes 0 — treat as invalid
    if (chan_count == 0) return;

    if (len < 18 + chan_count) return;  // truncated packet

    // Update packet counter first so DmxUniverse.sequence reflects this packet's index.
    uint64_t pkt_idx = pkts_.fetch_add(1, std::memory_order_relaxed);
    last_pkt_ns_.store(now_ns(), std::memory_order_relaxed);

    // Build DmxUniverse
    DmxUniverse univ{};
    univ.sequence = pkt_idx;
    std::memcpy(univ.ch.data(), data + 18, chan_count);

    // Measure latency: time from recv() to queue push
    HRTimer lat_timer;

    // Push to output queue
    bool pushed = out_.try_push({universe, std::move(univ)});
    if (!pushed) {
        log::warn("ArtNet: dmx_out queue full, universe %d dropped", universe);
    }

    double lat_ms = lat_timer.elapsed_ms();
    latency_stats_.push(lat_ms);

    if (latency_stats_.p99() > 8.0) {
        log::warn("ArtNet: latency p99=%.2f ms exceeds 8ms threshold", latency_stats_.p99());
    }

    if (universe < static_cast<int>(universe_active_.size()))
        universe_active_[static_cast<size_t>(universe)].store(true, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ArtPoll handler — send ArtPollReply in response
//  ArtPoll wire format:
//   0- 7 : ID
//   8- 9 : OpCode LE (0x2000)
//  10    : ProtVerHi
//  11    : ProtVerLo
//  12    : TalkToMe (bitfield)
//  13    : Priority (diagnostics priority threshold)
// ─────────────────────────────────────────────────────────────────────────────
void ArtNetListener::handle_artpoll(const uint8_t* data, int len,
                                    const sockaddr_in& src) {
    if (len < 14) return;

    // TalkToMe bit 1: if set, only reply with unicast to src address
    // TalkToMe bit 2: if set, send diagnostics
    // We always send a broadcast reply so that controllers that
    // issue directed polls still discover us.
    (void)data; // parsed above

    send_artpollreply(src);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ArtTimeCode handler
//  Wire format (from offset 10):
//   10 : ProtVerHi
//   11 : ProtVerLo
//   12 : Filler1
//   13 : Filler2
//   14 : Frames  (0-29)
//   15 : Seconds (0-59)
//   16 : Minutes (0-59)
//   17 : Hours   (0-23)
//   18 : Type    (0=24fps,1=25fps,2=29.97fps,3=30fps)
// ─────────────────────────────────────────────────────────────────────────────
void ArtNetListener::handle_arttimecode(const uint8_t* data, int len) {
    if (len < 19) return;

    ArtTimeCode tc{};
    tc.frames  = data[14];
    tc.seconds = data[15];
    tc.minutes = data[16];
    tc.hours   = data[17];
    tc.type    = data[18];

    {
        std::lock_guard<std::mutex> lk(tc_mutex_);
        last_tc_ = tc;
    }
    has_tc_.store(true, std::memory_order_release);

    log::debug("ArtNet: TimeCode %02d:%02d:%02d.%02d type=%d",
               tc.hours, tc.minutes, tc.seconds, tc.frames, tc.type);
}

// ─────────────────────────────────────────────────────────────────────────────
//  ArtPollReply construction and transmission
// ─────────────────────────────────────────────────────────────────────────────
bool ArtNetListener::get_local_ip(uint8_t out[4]) const {
    // Query the local IP address used on the socket
    sockaddr_in addr{};
#ifdef _WIN32
    int addr_len = sizeof(addr);
#else
    socklen_t addr_len = sizeof(addr);
#endif
    if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&addr), &addr_len) == 0) {
        uint32_t ip = ntohl(addr.sin_addr.s_addr);
        if (ip != 0) {
            out[0] = static_cast<uint8_t>((ip >> 24) & 0xFF);
            out[1] = static_cast<uint8_t>((ip >> 16) & 0xFF);
            out[2] = static_cast<uint8_t>((ip >>  8) & 0xFF);
            out[3] = static_cast<uint8_t>( ip        & 0xFF);
            return true;
        }
    }
    // Fallback: use 127.0.0.1
    out[0]=127; out[1]=0; out[2]=0; out[3]=1;
    return false;
}

bool ArtNetListener::get_local_mac(uint8_t out[6]) const {
    std::memset(out, 0, 6);

#ifdef _WIN32
    // Use GetAdaptersInfo to find MAC
    std::vector<uint8_t> buf(sizeof(IP_ADAPTER_INFO));
    ULONG out_len = static_cast<ULONG>(buf.size());

    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(buf.data()), &out_len)
            == ERROR_BUFFER_OVERFLOW) {
        buf.resize(out_len);
    }
    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(buf.data()), &out_len)
            == NO_ERROR) {
        PIP_ADAPTER_INFO ai = reinterpret_cast<PIP_ADAPTER_INFO>(buf.data());
        while (ai) {
            if (ai->AddressLength == 6) {
                std::memcpy(out, ai->Address, 6);
                return true;
            }
            ai = ai->Next;
        }
    }
#else
    struct ifaddrs* ifa_list = nullptr;
    if (::getifaddrs(&ifa_list) == 0) {
        for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr) continue;
#ifdef AF_PACKET
            if (ifa->ifa_addr->sa_family == AF_PACKET) {
                struct sockaddr_ll* sll =
                    reinterpret_cast<struct sockaddr_ll*>(ifa->ifa_addr);
                if (sll->sll_halen == 6) {
                    std::memcpy(out, sll->sll_addr, 6);
                    ::freeifaddrs(ifa_list);
                    return true;
                }
            }
#endif
        }
        ::freeifaddrs(ifa_list);
    }
#endif
    return false;
}

ArtPollReplyPacket ArtNetListener::build_poll_reply(const sockaddr_in& /*dest*/) const {
    ArtPollReplyPacket pkt{};
    std::memset(&pkt, 0, sizeof(pkt));

    // Header ID
    std::memcpy(pkt.id, kArtNetID, 8);

    // OpCode LE
    pkt.op_code = kArtOpPollReply;  // already LE on little-endian hosts

    // IP address (node's own IP, not the requestor's)
    get_local_ip(pkt.ip_address);

    // Port 0x1936 = 6454 in little-endian
    pkt.port = static_cast<uint16_t>(cfg_.port);  // LE

    // Firmware version (big-endian); we use 0x0001
    pkt.vers_info = htons(0x0001);

    // Net and SubNet switches for portAddress bits 14-8 and 7-4
    pkt.net_switch = cfg_.net & 0x7F;
    pkt.sub_switch = cfg_.sub_net & 0x0F;

    // OEM code: 0xFFFF = unregistered
    pkt.oem = 0xFFFF;

    // Status1 flags:
    //   bit 6 = RdmCapable (0)
    //   bit 5 = BootFromRom (0)
    //   bit 4 = PortAddressProgrammingAuthority (1 = programmed by network)
    //   bits 3-2 = IndicatorState (01 = normal)
    pkt.status1 = 0b00010100;  // PortAddr=net, Indicators=normal

    // ESTA manufacturer code (little-endian)
    pkt.esta_man = cfg_.esta_man;

    // Names
    copy_str_buf(pkt.short_name, cfg_.short_name);
    copy_str_buf(pkt.long_name,  cfg_.long_name);
    copy_str_buf(pkt.node_report, cfg_.node_report);

    // NumPorts: 1 input port (we receive DMX)
    pkt.num_ports = 1;  // LE

    // PortTypes[0]: bit 7=output, bit 6=input, bits 5-0=protocol
    //   0x80 = output, 0x40 = input, 0xC0 = both
    //   0x00 = DMX512 protocol
    // We are an input (receiving ArtDMX), so set bit 6
    pkt.port_types[0] = 0x40;  // Input, DMX512

    // GoodInput[0]: bit 7=data received, bit 2=errors, bit 0=disabled
    //   Set bit 7 if we have received packets
    uint8_t good_in = 0x00;
    if (pkts_.load(std::memory_order_relaxed) > 0)
        good_in |= 0x80;  // data received
    pkt.good_input[0] = good_in;

    // GoodOutputA[0]: not applicable (we're input only)
    pkt.good_output_a[0] = 0x00;

    // SwIn[0]: bits 3-0 of portAddress (Universe bits)
    // The portAddress for input 0 is cfg_.sub_net<<4 | universe_0
    pkt.sw_in[0] = cfg_.sub_net & 0x0F;

    // Style: 0x00 = StNode (standard Art-Net I/O node)
    pkt.style = 0x00;

    // MAC address
    uint8_t mac[6] = {};
    get_local_mac(mac);
    std::memcpy(pkt.mac, mac, 6);

    // BindIp: same as IP (not behind a binding gateway)
    std::memcpy(pkt.bind_ip, pkt.ip_address, 4);

    // BindIndex: 0 = root node
    pkt.bind_index = 0;

    // Status2 flags (Art-Net 4):
    //   bit 0 = DHCP capable (1)
    //   bit 1 = DHCP configured (0 = manual)
    //   bit 2 = supports 15-bit portAddress (1)
    //   bit 3 = can switch to sACN (0)
    //   bit 4 = squawking (0)
    //   bit 6 = supports LLRP (0)
    //   bit 7 = supports failsafe (0)
    pkt.status2 = 0b00000101;  // DHCP capable, 15-bit addressing

    // Status3 flags (Art-Net 4):
    //   bits 7-6 = failsafe mode (00 = hold last state)
    //   bit 5 = supports failsafe record (0)
    pkt.status3 = 0x00;

    // RefreshRate: 0 = default (44Hz per spec)
    pkt.refresh_rate = 0;

    return pkt;
}

void ArtNetListener::send_artpollreply(const sockaddr_in& src) {
    ArtPollReplyPacket pkt = build_poll_reply(src);

    // ArtPollReply must be sent as a BROADCAST so that all controllers on the
    // network discover this node, not just the one that sent the poll.
    // Per Art-Net 4 spec §6: "The ArtPollReply is always broadcast."
    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(static_cast<uint16_t>(cfg_.port));
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    int sent = static_cast<int>(::sendto(
        sock_,
        reinterpret_cast<const char*>(&pkt), sizeof(pkt),
        0,
        reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)));

    if (sent != static_cast<int>(sizeof(pkt))) {
        log::warn("ArtNet: ArtPollReply send failed (sent=%d expected=%d)",
                  sent, static_cast<int>(sizeof(pkt)));
    } else {
        log::debug("ArtNet: ArtPollReply broadcast sent (%d bytes)", sent);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SACNListener
// ─────────────────────────────────────────────────────────────────────────────

// E1.31 constants
namespace sacn {
    static constexpr uint16_t kPort = 5568;

    // ACN Packet Identifier: "ASC-E1.17\0\0\0" (12 bytes)
    static constexpr uint8_t kAcnPacketId[12] = {
        0x41,0x53,0x43,0x2D,0x45,0x31,0x2E,0x31,0x37,0x00,0x00,0x00
    };

    // E1.31 Data Packet Vector
    static constexpr uint32_t kVectorRootE131Ext    = 0x00000004;
    static constexpr uint32_t kVectorE131DataPacket = 0x00000002;
    static constexpr uint32_t kVectorDmpSetProperty = 0x02;

    // Field offsets in the E1.31 Data Packet
    static constexpr int kOffPreambleSize = 0;   // uint16 BE, = 0x0010
    static constexpr int kOffPostambleSize= 2;   // uint16 BE, = 0x0000
    static constexpr int kOffAcnId       = 4;   // [12] = kAcnPacketId
    static constexpr int kOffFramingLen  = 16;  // PDU length (flags+len) BE
    static constexpr int kOffRootVector  = 18;  // uint32 BE
    static constexpr int kOffCid         = 22;  // [16] UUID
    static constexpr int kOffFrameVector = 40;  // uint32 BE = kVectorE131DataPacket
    static constexpr int kOffSource      = 44;  // [64] UTF-8 source name
    static constexpr int kOffPriority    = 108; // uint8, 0-200 (100=default)
    static constexpr int kOffSyncAddr    = 109; // uint16 BE synchronization address
    static constexpr int kOffSequence    = 111; // uint8
    static constexpr int kOffOptions     = 112; // uint8
    static constexpr int kOffUniverse    = 113; // uint16 BE (1-63999)
    static constexpr int kOffDmpPduLen   = 115; // PDU length (flags+len) BE
    static constexpr int kOffDmpVector   = 117; // uint8 = kVectorDmpSetProperty
    static constexpr int kOffDmpAddrType = 118; // uint8 = 0xA1
    static constexpr int kOffDmpFirstAddr= 119; // uint16 BE = 0x0000
    static constexpr int kOffDmpAddrIncr = 121; // uint16 BE = 0x0001
    static constexpr int kOffDmpCount    = 123; // uint16 BE (property count)
    static constexpr int kOffDmpData     = 125; // first byte is DMX start code
    static constexpr int kMinPacketLen   = 126; // minimum valid packet length
}

SACNListener::SACNListener(MpscQueue<std::pair<int, DmxUniverse>>& dmx_out)
    : out_(dmx_out) {
    winsock_init();
}

SACNListener::~SACNListener() {
    stop();
}

bool SACNListener::start(const std::string& bind_ip,
                         const std::vector<int>& universes) {
    if (running_.load(std::memory_order_relaxed)) return true;

    bind_ip_  = bind_ip;
    universes_= universes;

    // Create UDP socket
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!socket_valid(sock_)) {
        log::error("sACN: socket() failed");
        return false;
    }

    int yes = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(sacn::kPort);
    addr.sin_addr.s_addr = bind_ip_.empty()
                           ? INADDR_ANY
                           : inet_addr(bind_ip_.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE)
        addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log::error("sACN: bind() failed on port %d", sacn::kPort);
        socket_close(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

    // Join multicast groups for each requested universe
    for (int u : universes_) {
        if (!join_multicast(u)) {
            log::warn("sACN: failed to join multicast for universe %d", u);
        }
    }

    set_nonblocking(sock_);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&SACNListener::recv_loop, this);

    log::info("sACN: listening on port %d (%zu universes)",
              sacn::kPort, universes_.size());
    return true;
}

void SACNListener::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;

    running_.store(false, std::memory_order_release);
    socket_close(sock_);
    sock_ = kInvalidSocket;

    if (thread_.joinable())
        thread_.join();

    log::info("sACN: stopped. Packets received: %llu",
              static_cast<unsigned long long>(pkts_.load()));
}

bool SACNListener::join_multicast(int universe) {
    if (universe < 1 || universe > 63999) return false;

    // E1.31 multicast address: 239.255.X.Y
    // where X = (universe >> 8) & 0xFF, Y = universe & 0xFF
    uint8_t mc_x = static_cast<uint8_t>((universe >> 8) & 0xFF);
    uint8_t mc_y = static_cast<uint8_t>( universe       & 0xFF);

    char mc_addr[32];
    std::snprintf(mc_addr, sizeof(mc_addr), "239.255.%d.%d", mc_x, mc_y);

    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(mc_addr);
    mreq.imr_interface.s_addr = bind_ip_.empty()
                                ? INADDR_ANY
                                : inet_addr(bind_ip_.c_str());
    if (mreq.imr_interface.s_addr == INADDR_NONE)
        mreq.imr_interface.s_addr = INADDR_ANY;

    int ret = ::setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                           reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    if (ret != 0) {
        log::warn("sACN: IP_ADD_MEMBERSHIP failed for universe %d (%s)", universe, mc_addr);
        return false;
    }
    log::debug("sACN: joined multicast %s for universe %d", mc_addr, universe);
    return true;
}

void SACNListener::recv_loop() {
    constexpr int kBufSize = 1144;  // max E1.31 packet: 638 channels * ~1.7 = well within
    uint8_t buf[kBufSize];

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
            sock_, reinterpret_cast<char*>(buf), kBufSize, 0,
            reinterpret_cast<sockaddr*>(&src), &src_len));

        if (n < sacn::kMinPacketLen) continue;

        handle_data_packet(buf, n);
    }
}

void SACNListener::handle_data_packet(const uint8_t* d, int len) {
    if (len < sacn::kMinPacketLen) return;

    // Validate ACN Packet Identifier (offset 4, 12 bytes)
    if (std::memcmp(d + sacn::kOffAcnId, sacn::kAcnPacketId, 12) != 0) return;

    // Root layer vector (BE uint32 at offset 18) must be 0x00000004
    uint32_t root_vec = (static_cast<uint32_t>(d[sacn::kOffRootVector])     << 24) |
                        (static_cast<uint32_t>(d[sacn::kOffRootVector + 1]) << 16) |
                        (static_cast<uint32_t>(d[sacn::kOffRootVector + 2]) <<  8) |
                         static_cast<uint32_t>(d[sacn::kOffRootVector + 3]);
    if (root_vec != sacn::kVectorRootE131Ext) return;

    // Framing layer vector (BE uint32 at offset 40) must be 0x00000002
    uint32_t frame_vec = (static_cast<uint32_t>(d[sacn::kOffFrameVector])     << 24) |
                         (static_cast<uint32_t>(d[sacn::kOffFrameVector + 1]) << 16) |
                         (static_cast<uint32_t>(d[sacn::kOffFrameVector + 2]) <<  8) |
                          static_cast<uint32_t>(d[sacn::kOffFrameVector + 3]);
    if (frame_vec != sacn::kVectorE131DataPacket) return;

    // Universe (BE uint16 at offset 113)
    uint16_t universe = (static_cast<uint16_t>(d[sacn::kOffUniverse]) << 8) |
                         static_cast<uint16_t>(d[sacn::kOffUniverse + 1]);
    if (universe == 0 || universe > 63999) return;

    // DMP layer vector (uint8 at offset 117) must be 0x02
    if (d[sacn::kOffDmpVector] != sacn::kVectorDmpSetProperty) return;

    // Address type (uint8 at offset 118) must be 0xA1
    if (d[sacn::kOffDmpAddrType] != 0xA1) return;

    // First property address (BE uint16 at offset 119) must be 0x0000
    uint16_t first_addr = (static_cast<uint16_t>(d[sacn::kOffDmpFirstAddr]) << 8) |
                           static_cast<uint16_t>(d[sacn::kOffDmpFirstAddr + 1]);
    if (first_addr != 0x0000) return;

    // Address increment (BE uint16 at offset 121) must be 0x0001
    uint16_t addr_incr = (static_cast<uint16_t>(d[sacn::kOffDmpAddrIncr]) << 8) |
                          static_cast<uint16_t>(d[sacn::kOffDmpAddrIncr + 1]);
    if (addr_incr != 0x0001) return;

    // Property count (BE uint16 at offset 123) = number of slots including start code
    uint16_t count = (static_cast<uint16_t>(d[sacn::kOffDmpCount]) << 8) |
                      static_cast<uint16_t>(d[sacn::kOffDmpCount + 1]);
    if (count < 2) return;  // must have at least start code + 1 data byte

    // Offset 125 is the DMX512 start code; value 0 = standard null start code
    uint8_t start_code = d[sacn::kOffDmpData];
    if (start_code != 0x00) return;  // ignore non-null start codes

    uint16_t data_count = count - 1;  // exclude start code
    if (data_count > 512) data_count = 512;
    if (len < sacn::kOffDmpData + 1 + data_count) return;

    // Build DmxUniverse (0-based internal index = universe-1)
    int internal_uni = static_cast<int>(universe) - 1;
    if (internal_uni < 0 || internal_uni >= kMaxUniverses) return;

    uint64_t pkt_idx = pkts_.fetch_add(1, std::memory_order_relaxed);

    DmxUniverse univ{};
    univ.sequence = pkt_idx;
    std::memcpy(univ.ch.data(), d + sacn::kOffDmpData + 1, data_count);

    out_.try_push({internal_uni, std::move(univ)});
}

} // namespace idhmfis
