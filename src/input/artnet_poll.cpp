// artnet_poll.cpp — Art-Net 4 periodic ArtPoll broadcaster (peer-to-peer discovery).
//
// Art-Net 4 spec §4:
//   "Nodes that need to communicate on a peer-to-peer basis must transmit
//    ArtPoll every 2.5 to 3 seconds and must respond with ArtPollReply
//    to any received ArtPoll."
//
// This module handles the *outgoing* ArtPoll broadcast so that IDHMFIS
// appears in network discovery dialogs of grandMA3, Hog 4, MagicQ, ETC EOS,
// and similar consoles even if none of them issues an active poll first.
//
// The ArtPollReply fields are set per Art-Net 4 spec §B9 requirements:
//   - OpCode 0x2100, ProtVer 14
//   - NodeReport: "#0001 [0000] IDHMFIS Laser Show"
//   - NumPorts: 4, PortTypes 0x80 (output DMX512)
//   - GoodOutputA: 0x80 (data transmitted)
//   - SwOut 0..3: universes 0-3
//   - ShortName: "IDHMFIS", LongName: "IDHMFIS Laser Show Controller"
//   - Style: 0x00 (StNode)
//   - OemCode: 0xFFFF, Esta: 0x0000

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
#  include <fcntl.h>
#  include <netpacket/packet.h>
#endif

#include <cstring>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <vector>

#include "artnet_poll.h"
#include "../core/logger.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Socket helpers (local to this TU)
// ─────────────────────────────────────────────────────────────────────────────
namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSock = INVALID_SOCKET;
inline bool sock_valid(socket_t s) { return s != INVALID_SOCKET; }
inline void sock_close(socket_t s) { if (sock_valid(s)) ::closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalidSock = -1;
inline bool sock_valid(socket_t s) { return s >= 0; }
inline void sock_close(socket_t s) { if (sock_valid(s)) ::close(s); }
#endif

void ensure_wsa() {
#ifdef _WIN32
    static struct Init { Init() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
                         ~Init(){ WSACleanup(); } } g;
#endif
}

// Copy string into fixed zero-padded buffer
template<size_t N>
void fill_str(uint8_t (&dst)[N], const char* src) {
    std::memset(dst, 0, N);
    size_t n = std::strlen(src);
    if (n >= N) n = N - 1;
    std::memcpy(dst, src, n);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  ArtPollSender::start / stop
// ─────────────────────────────────────────────────────────────────────────────
bool ArtPollSender::start(const ArtNetConfig& cfg) {
    if (running_.load(std::memory_order_relaxed)) return true;

    ensure_wsa();
    cfg_ = cfg;

    // Create a separate UDP socket for outgoing polls
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sock_valid(sock_)) {
        log::warn("ArtPollSender: socket() failed — peer discovery disabled");
        return false;
    }

    // SO_REUSEADDR — allow binding even if ArtNetListener already holds port 6454
    int yes = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    // SO_BROADCAST — required to send to 255.255.255.255
    ::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    // Bind to 0.0.0.0:6454 (same port, REUSEADDR allows it)
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(cfg_.port));
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        // Non-fatal: we can still send without binding to 6454
        log::warn("ArtPollSender: bind() failed on port %d — "
                  "peer ArtPoll will use ephemeral port", cfg_.port);
        // Close and reopen without binding to a specific port
        sock_close(sock_);
        sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (!sock_valid(sock_)) {
            log::warn("ArtPollSender: socket() retry failed — peer discovery disabled");
            return false;
        }
        ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
        ::setsockopt(sock_, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&ArtPollSender::poll_loop, this);

    log::info("ArtPollSender: started (broadcasting every %d ms)", kPollIntervalMs);
    return true;
}

void ArtPollSender::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    running_.store(false, std::memory_order_release);
    sock_close(sock_);
    sock_ = kInvalidSock;
    if (thread_.joinable()) thread_.join();
    log::info("ArtPollSender: stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  poll_loop — runs on background thread
// ─────────────────────────────────────────────────────────────────────────────
void ArtPollSender::poll_loop() {
    using Clock = std::chrono::steady_clock;
    using ms    = std::chrono::milliseconds;

    // Send immediate presence announcement on start
    send_artpollreply();
    send_artpoll();

    auto next_poll = Clock::now() + ms(kPollIntervalMs);

    while (running_.load(std::memory_order_relaxed)) {
        auto now = Clock::now();
        if (now >= next_poll) {
            send_artpoll();
            send_artpollreply();
            next_poll = Clock::now() + ms(kPollIntervalMs);
        }
        // Sleep in 100 ms increments to check running_ flag
        std::this_thread::sleep_for(ms(100));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ArtPoll packet — broadcast to discover other nodes
//  Wire format:
//   [0-7]  "Art-Net\0"
//   [8-9]  OpCode 0x2000 LE
//   [10]   ProtVerHi = 0
//   [11]   ProtVerLo = 14
//   [12]   TalkToMe  = 0x06 (reply on change, diagnostics unicast)
//   [13]   Priority  = 0x10 (DP_Low = 16, accept all diagnostics)
// ─────────────────────────────────────────────────────────────────────────────
void ArtPollSender::send_artpoll() {
    if (!sock_valid(sock_)) return;

    uint8_t pkt[14] = {};
    std::memcpy(pkt, kArtNetID, 8);
    pkt[8]  = static_cast<uint8_t>(kArtOpPoll & 0xFF);
    pkt[9]  = static_cast<uint8_t>((kArtOpPoll >> 8) & 0xFF);
    pkt[10] = kArtNetProtVerHi;
    pkt[11] = kArtNetProtVerLo;
    pkt[12] = 0x06;  // TalkToMe: reply-on-change, unicast diag
    pkt[13] = 0x10;  // Priority: DP_Low

    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(static_cast<uint16_t>(cfg_.port));
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    int sent = static_cast<int>(::sendto(
        sock_, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
        reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)));

    if (sent < 0) {
        log::debug("ArtPollSender: ArtPoll sendto failed");
    } else {
        log::debug("ArtPollSender: ArtPoll broadcast sent");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_artpollreply — broadcast our node information
// ─────────────────────────────────────────────────────────────────────────────
void ArtPollSender::send_artpollreply() {
    if (!sock_valid(sock_)) return;

    ArtPollReplyPacket pkt = build_poll_reply();

    sockaddr_in dest{};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(static_cast<uint16_t>(cfg_.port));
    dest.sin_addr.s_addr = INADDR_BROADCAST;

    int sent = static_cast<int>(::sendto(
        sock_,
        reinterpret_cast<const char*>(&pkt), sizeof(pkt), 0,
        reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)));

    if (sent != static_cast<int>(sizeof(pkt))) {
        log::warn("ArtPollSender: ArtPollReply broadcast failed (sent=%d, expected=%d)",
                  sent, static_cast<int>(sizeof(pkt)));
    } else {
        log::debug("ArtPollSender: ArtPollReply broadcast sent (%d bytes)", sent);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_poll_reply — constructs the 239-byte ArtPollReplyPacket
//  Fields set per §B9 requirements
// ─────────────────────────────────────────────────────────────────────────────
ArtPollReplyPacket ArtPollSender::build_poll_reply() const {
    ArtPollReplyPacket pkt{};
    std::memset(&pkt, 0, sizeof(pkt));

    // ── Header ────────────────────────────────────────────────────────────
    std::memcpy(pkt.id, kArtNetID, 8);
    pkt.op_code = kArtOpPollReply;             // 0x2100 LE

    // ── IP / port ─────────────────────────────────────────────────────────
    get_local_ip(pkt.ip_address);
    pkt.port     = static_cast<uint16_t>(cfg_.port);  // 6454 LE
    pkt.vers_info= htons(0x0001);                       // firmware v1, BE

    // ── Port-address: Net=0, SubNet=0 ─────────────────────────────────────
    pkt.net_switch = cfg_.net & 0x7F;
    pkt.sub_switch = cfg_.sub_net & 0x0F;

    // ── OEM / ESTA ────────────────────────────────────────────────────────
    pkt.oem      = 0xFFFF;     // unregistered / generic
    pkt.esta_man = 0x0000;     // ESTA = 0x0000 as specified

    // ── Names and report ──────────────────────────────────────────────────
    fill_str(pkt.short_name,  "IDHMFIS");
    fill_str(pkt.long_name,   "IDHMFIS Laser Show Controller");
    fill_str(pkt.node_report, "#0001 [0000] IDHMFIS Laser Show");

    // ── Ports: 4 DMX output ports ─────────────────────────────────────────
    pkt.num_ports = 4;  // LE

    // PortTypes[0..3]: 0x80 = output, DMX512
    for (int i = 0; i < 4; ++i) {
        pkt.port_types[i]    = 0x80;   // output, DMX512
        pkt.good_output_a[i] = 0x80;   // data transmitted
        pkt.sw_out[i]        = static_cast<uint8_t>(i);  // universes 0-3
    }

    // ── Style: StNode ─────────────────────────────────────────────────────
    pkt.style = 0x00;  // StNode

    // ── MAC address ───────────────────────────────────────────────────────
    get_local_mac(pkt.mac);

    // ── BindIp = local IP ─────────────────────────────────────────────────
    std::memcpy(pkt.bind_ip, pkt.ip_address, 4);
    pkt.bind_index = 0;

    // ── Status1 ───────────────────────────────────────────────────────────
    // bit 6: RDM capable = 0
    // bit 4: PortAddressProgrammingAuthority = 1 (network programmed)
    // bits 3-2: IndicatorState = 01 (normal)
    pkt.status1 = 0b00010100;

    // ── Status2 (Art-Net 4) ───────────────────────────────────────────────
    // bit 0: DHCP capable = 1
    // bit 2: supports 15-bit portAddress = 1
    pkt.status2 = 0b00000101;

    // ── Status3 = 0 ───────────────────────────────────────────────────────
    pkt.status3 = 0x00;

    // ── RefreshRate = 0 (default 44 Hz) ───────────────────────────────────
    pkt.refresh_rate = 0;

    return pkt;
}

// ─────────────────────────────────────────────────────────────────────────────
//  get_local_ip / get_local_mac
// ─────────────────────────────────────────────────────────────────────────────
bool ArtPollSender::get_local_ip(uint8_t out[4]) const {
    // Connect a UDP socket to an external address to determine local IP
    // without actually sending any traffic.
#ifdef _WIN32
    SOCKET probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == INVALID_SOCKET) {
        out[0]=127; out[1]=0; out[2]=0; out[3]=1; return false;
    }
    sockaddr_in remote{};
    remote.sin_family      = AF_INET;
    remote.sin_port        = htons(80);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (::connect(probe, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
        sockaddr_in local{};
        int len = sizeof(local);
        if (::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            uint32_t ip = ntohl(local.sin_addr.s_addr);
            if (ip != 0) {
                out[0] = static_cast<uint8_t>((ip >> 24) & 0xFF);
                out[1] = static_cast<uint8_t>((ip >> 16) & 0xFF);
                out[2] = static_cast<uint8_t>((ip >>  8) & 0xFF);
                out[3] = static_cast<uint8_t>( ip        & 0xFF);
                ::closesocket(probe);
                return true;
            }
        }
    }
    ::closesocket(probe);
#else
    int probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe >= 0) {
        sockaddr_in remote{};
        remote.sin_family      = AF_INET;
        remote.sin_port        = htons(80);
        remote.sin_addr.s_addr = inet_addr("8.8.8.8");
        if (::connect(probe, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
            sockaddr_in local{};
            socklen_t len = sizeof(local);
            if (::getsockname(probe, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                uint32_t ip = ntohl(local.sin_addr.s_addr);
                if (ip != 0) {
                    out[0] = static_cast<uint8_t>((ip >> 24) & 0xFF);
                    out[1] = static_cast<uint8_t>((ip >> 16) & 0xFF);
                    out[2] = static_cast<uint8_t>((ip >>  8) & 0xFF);
                    out[3] = static_cast<uint8_t>( ip        & 0xFF);
                    ::close(probe);
                    return true;
                }
            }
        }
        ::close(probe);
    }
#endif
    out[0]=2; out[1]=0; out[2]=0; out[3]=1;  // Art-Net default range
    return false;
}

bool ArtPollSender::get_local_mac(uint8_t out[6]) const {
    std::memset(out, 0, 6);
#ifdef _WIN32
    std::vector<uint8_t> buf(sizeof(IP_ADAPTER_INFO));
    ULONG sz = static_cast<ULONG>(buf.size());
    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(buf.data()), &sz)
            == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz);
    }
    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(buf.data()), &sz) == NO_ERROR) {
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

} // namespace idhmfis
