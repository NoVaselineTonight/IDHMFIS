// sacn.cpp — sACN E1.31 sender implementation.
//
// E1.31 Data Packet wire layout (all multi-byte fields big-endian):
//
// Offset  Field                         Size
// ──────  ─────────────────────────────  ────
//  0      Preamble Size (0x0010)         2
//  2      Postamble Size (0x0000)        2
//  4      ACN Packet Identifier          12
// 16      PDU Flags+Length (root)        2
// 18      Root Vector (0x00000004)       4
// 22      CID                            16
// 38      PDU Flags+Length (framing)     2   ← offset in packet = 38 - (38-38) = 38
// 40      Framing Vector (0x00000002)    4
// 44      Source Name (64 bytes)         64
// 108     Priority (0-200)               1
// 109     Synchronization Address        2
// 111     Sequence Number                1
// 112     Options                        1
// 113     Universe (1-63999)             2
// 115     PDU Flags+Length (DMP)         2
// 117     DMP Vector (0x02)              1
// 118     Address Type (0xA1)            1
// 119     First Property Address         2
// 121     Address Increment              2
// 123     Property Count (ch+1)          2
// 125     Start Code (0x00)              1
// 126+    DMX Data (up to 512 bytes)     var
// Total with 512 channels:              638 bytes

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
#  include <fcntl.h>
#endif

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <fstream>

#include "sacn.h"
#include "../core/logger.h"

namespace idhmfis {

// Inline constexpr member definition (ODR)
constexpr char SACNSender::kSourceName[];
constexpr uint8_t SACNSender::kAcnId[12];

// ─────────────────────────────────────────────────────────────────────────────
//  Socket helpers
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
    static struct Init { Init(){WSADATA d; WSAStartup(MAKEWORD(2,2),&d);}
                         ~Init(){WSACleanup();}} g;
#endif
}

// Write big-endian uint16
inline void be16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[1] = static_cast<uint8_t>( v       & 0xFF);
}

// Write big-endian uint32
inline void be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >>  8) & 0xFF);
    p[3] = static_cast<uint8_t>( v        & 0xFF);
}

// PDU flags+length field: high 4 bits = 0x7 (flags), low 12 bits = length
inline void pdu_len(uint8_t* p, uint16_t len) {
    uint16_t fl = static_cast<uint16_t>(0x7000u | (len & 0x0FFFu));
    be16(p, fl);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  SACNCid
// ─────────────────────────────────────────────────────────────────────────────
SACNCid SACNCid::generate() {
    SACNCid cid;
    // Use steady_clock + system_clock as entropy
    uint64_t t1 = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    uint64_t t2 = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    // Mix into 16 bytes using simple xorshift
    uint64_t a = t1 ^ 0x123456789ABCDEF0ULL;
    uint64_t b = t2 ^ 0xFEDCBA9876543210ULL;
    for (int i = 0; i < 3; ++i) {
        a ^= a << 13; a ^= a >> 7; a ^= a << 17;
        b ^= b << 17; b ^= b >> 5; b ^= b << 12;
    }
    std::memcpy(cid.bytes.data(),     &a, 8);
    std::memcpy(cid.bytes.data() + 8, &b, 8);

    // Set UUID version 4 and variant bits
    cid.bytes[6] = static_cast<uint8_t>((cid.bytes[6] & 0x0F) | 0x40); // version 4
    cid.bytes[8] = static_cast<uint8_t>((cid.bytes[8] & 0x3F) | 0x80); // variant 1

    return cid;
}

SACNCid SACNCid::load_or_create(const std::string& path) {
    // Try to load existing CID
    {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            SACNCid cid;
            f.read(reinterpret_cast<char*>(cid.bytes.data()), 16);
            if (f.gcount() == 16) {
                log::info("sACN: loaded CID from %s", path.c_str());
                return cid;
            }
        }
    }

    // Generate and save
    SACNCid cid = generate();
    cid.save(path);
    log::info("sACN: generated new CID, saved to %s", path.c_str());
    return cid;
}

bool SACNCid::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), 16);
    return f.good();
}

// ─────────────────────────────────────────────────────────────────────────────
//  SACNSender constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
SACNSender::SACNSender(SACNCid cid) : cid_(cid) {
    ensure_wsa();
    std::memset(seq_, 0, sizeof(seq_));
}

SACNSender::~SACNSender() {
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
bool SACNSender::start(const std::string& dest_ip, uint8_t priority, uint8_t ttl) {
    if (running_.load(std::memory_order_relaxed)) return true;

    dest_ip_  = dest_ip;
    priority_ = priority;
    ttl_      = ttl;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (!sock_valid(sock_)) {
        log::error("sACN sender: socket() failed");
        return false;
    }

    // SO_REUSEADDR
    int yes = 1;
    ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&yes), sizeof(yes));

    if (dest_ip_.empty()) {
        // Multicast: set TTL and disable loopback on same host
        ::setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL,
                     reinterpret_cast<const char*>(&ttl_), sizeof(ttl_));
        uint8_t loop = 0;
        ::setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_LOOP,
                     reinterpret_cast<const char*>(&loop), sizeof(loop));
    }

    running_.store(true, std::memory_order_release);
    log::info("sACN sender: started (priority=%d, dest=%s)",
              priority_, dest_ip_.empty() ? "multicast" : dest_ip_.c_str());
    return true;
}

void SACNSender::stop() {
    if (!running_.load(std::memory_order_relaxed)) return;
    running_.store(false, std::memory_order_release);
    sock_close(sock_);
    sock_ = kInvalidSock;
    log::info("sACN sender: stopped");
}

// ─────────────────────────────────────────────────────────────────────────────
//  send_universe
// ─────────────────────────────────────────────────────────────────────────────
bool SACNSender::send_universe(int universe, const DmxUniverse& univ) {
    if (!running_.load(std::memory_order_relaxed)) return false;
    if (universe < 1 || universe > 63999) return false;

    // sACN universe index into our sequence array (capped at kMaxSACNUniverses)
    int seq_idx = std::min(universe - 1, kMaxSACNUniverses - 1);
    uint8_t seq = seq_[seq_idx]++;

    static constexpr int kBufSize = 638;
    uint8_t buf[kBufSize];

    int pkt_len = build_packet(buf, kBufSize, universe, univ.ch.data(), 512, seq);
    if (pkt_len <= 0) return false;

    // Determine destination address
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(static_cast<uint16_t>(kPort));

    if (dest_ip_.empty()) {
        // E1.31 multicast: 239.255.X.Y
        uint8_t mc_x = static_cast<uint8_t>((universe >> 8) & 0xFF);
        uint8_t mc_y = static_cast<uint8_t>( universe       & 0xFF);
        char mc[32];
        std::snprintf(mc, sizeof(mc), "239.255.%d.%d", mc_x, mc_y);
        dest.sin_addr.s_addr = inet_addr(mc);
    } else {
        dest.sin_addr.s_addr = inet_addr(dest_ip_.c_str());
        if (dest.sin_addr.s_addr == INADDR_NONE) return false;
    }

    int sent = static_cast<int>(::sendto(
        sock_, reinterpret_cast<const char*>(buf), pkt_len, 0,
        reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)));

    return sent == pkt_len;
}

// ─────────────────────────────────────────────────────────────────────────────
//  build_packet — constructs a complete E1.31 Data Packet
// ─────────────────────────────────────────────────────────────────────────────
int SACNSender::build_packet(uint8_t* buf, int buf_size,
                              int universe, const uint8_t* dmx, int chan_count,
                              uint8_t sequence_num) const {
    if (chan_count < 1) chan_count = 1;
    if (chan_count > 512) chan_count = 512;

    // Total packet length = 126 (header) + chan_count (DMX data)
    int total = 126 + chan_count;
    if (total > buf_size) return -1;

    std::memset(buf, 0, static_cast<size_t>(total));

    // ── Preamble (offset 0) ───────────────────────────────────────────────
    be16(buf + 0, 0x0010);   // Preamble Size
    be16(buf + 2, 0x0000);   // Postamble Size
    std::memcpy(buf + 4, kAcnId, 12);  // ACN Packet Identifier

    // ── Root layer PDU (offset 16) ────────────────────────────────────────
    // Flags+Length: covers from offset 16 to end = total - 16
    pdu_len(buf + 16, static_cast<uint16_t>(total - 16));
    be32(buf + 18, 0x00000004u);          // Root Vector = VECTOR_ROOT_E131_EXTENDED
    std::memcpy(buf + 22, cid_.bytes.data(), 16);  // CID (16 bytes)

    // ── Framing layer PDU (offset 38) ─────────────────────────────────────
    // Flags+Length: covers from offset 38 to end = total - 38
    pdu_len(buf + 38, static_cast<uint16_t>(total - 38));
    be32(buf + 40, 0x00000002u);           // Framing Vector = VECTOR_E131_DATA_PACKET

    // Source Name (64 bytes, zero-padded)
    {
        size_t sn_len = std::strlen(kSourceName);
        if (sn_len > 63) sn_len = 63;
        std::memcpy(buf + 44, kSourceName, sn_len);
        // rest already zeroed
    }

    buf[108] = priority_;               // Priority
    be16(buf + 109, 0x0000u);           // Synchronization Address = 0
    buf[111] = sequence_num;            // Sequence Number
    buf[112] = 0x00;                    // Options
    be16(buf + 113, static_cast<uint16_t>(universe));  // Universe

    // ── DMP layer PDU (offset 115) ────────────────────────────────────────
    // Flags+Length: covers from offset 115 to end = total - 115
    pdu_len(buf + 115, static_cast<uint16_t>(total - 115));
    buf[117] = 0x02;                    // DMP Vector = VECTOR_DMP_SET_PROPERTY
    buf[118] = 0xA1;                    // Address Type & Data Type
    be16(buf + 119, 0x0000u);           // First Property Address
    be16(buf + 121, 0x0001u);           // Address Increment
    be16(buf + 123, static_cast<uint16_t>(chan_count + 1)); // Property Count (inc. start code)
    buf[125] = 0x00;                    // DMX512 Start Code = 0

    // ── DMX data ──────────────────────────────────────────────────────────
    std::memcpy(buf + 126, dmx, static_cast<size_t>(chan_count));

    return total;
}

} // namespace idhmfis
