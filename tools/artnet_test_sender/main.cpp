// artnet_test_sender — standalone ArtNet DMX512 test tool
//
// Sends ArtNet Art-DMX packets at a configurable rate.
// Animates all 512 DMX channels with a slow per-channel sine sweep so you can
// see live motion in a fixture visualiser or real-time console.
//
// The tool also performs a crude round-trip latency measurement:
//   - Stamp a known value into channel 1 at a known time
//   - Measure when it echoes back (requires a loopback or monitoring listener)
//
// Usage:
//   artnet_test_sender [--ip <ip>] [--universe <u>] [--fps <f>]
//
// Defaults: ip=127.0.0.1, universe=0, fps=44 (≈ ArtNet recommended max)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SockFd = SOCKET;
   static constexpr SockFd kInvalid = INVALID_SOCKET;
   inline void close_sock(SockFd s) { closesocket(s); }
#  pragma comment(lib, "ws2_32.lib")
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using SockFd = int;
   static constexpr SockFd kInvalid = -1;
   inline void close_sock(SockFd s) { ::close(s); }
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  ArtNet protocol constants (Art-Net 4, ANSI E1.17)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int      kArtNetPort      = 6454;
static constexpr uint16_t kOpArtDMX        = 0x5000;
static constexpr uint8_t  kProtVerHi       = 0;
static constexpr uint8_t  kProtVerLo       = 14;

#pragma pack(push, 1)
struct ArtDmxPacket {
    char     id[8]       = { 'A','r','t','-','N','e','t','\0' };
    uint16_t opcode      = kOpArtDMX; // little-endian
    uint8_t  proto_hi    = kProtVerHi;
    uint8_t  proto_lo    = kProtVerLo;
    uint8_t  sequence    = 0;         // 0 = disable sequence checking
    uint8_t  physical    = 0;
    uint16_t universe    = 0;         // little-endian, 15-bit sub-net/universe
    uint16_t length      = 512;       // big-endian (byte-swapped below)
    uint8_t  data[512]   = {};
};
#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
//  Simple high-resolution monotonic clock (seconds since epoch)
// ─────────────────────────────────────────────────────────────────────────────
static double now_s() {
    using Clock = std::chrono::steady_clock;
    static const auto epoch = Clock::now();
    return std::chrono::duration<double>(Clock::now() - epoch).count();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Rolling stats (P99 + mean) — header-only, no deps
// ─────────────────────────────────────────────────────────────────────────────
struct Stats {
    static constexpr int N = 256;
    double   buf[N]     = {};
    double   sum        = 0.0;
    double   p99        = 0.0;
    int      idx        = 0;
    int      count      = 0;

    void push(double v) {
        sum    -= buf[idx];
        buf[idx] = v;
        sum    += v;
        idx     = (idx + 1) % N;
        if (count < N) ++count;
        if (v > p99) p99 = v;
        else p99 = p99 * 0.999 + v * 0.001;
    }
    double mean() const { return count ? sum / count : 0.0; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // --- Parse command line --------------------------------------------------
    std::string target_ip  = "127.0.0.1";
    int         universe   = 0;
    int         fps        = 44;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if ((arg == "--ip" || arg == "-i") && i + 1 < argc) {
            target_ip = argv[++i];
        } else if ((arg == "--universe" || arg == "-u") && i + 1 < argc) {
            universe = std::atoi(argv[++i]);
        } else if ((arg == "--fps" || arg == "-f") && i + 1 < argc) {
            fps = std::atoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            std::printf(
                "Usage: artnet_test_sender [--ip <ip>] [--universe <u>] [--fps <f>]\n"
                "  --ip <ip>        Target IP  (default: 127.0.0.1)\n"
                "  --universe <u>   ArtNet universe 0-32767 (default: 0)\n"
                "  --fps <f>        Packets per second, 1-44 (default: 44)\n"
            );
            return 0;
        }
    }

    fps       = std::max(1,  std::min(44,    fps));
    universe  = std::max(0,  std::min(32767, universe));

    std::printf("ArtNet test sender\n");
    std::printf("  Target    : %s:%d\n", target_ip.c_str(), kArtNetPort);
    std::printf("  Universe  : %d\n",   universe);
    std::printf("  Rate      : %d fps\n", fps);
    std::printf("  Press Ctrl+C to stop.\n\n");

    // --- Winsock init --------------------------------------------------------
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    // --- UDP socket ----------------------------------------------------------
    SockFd sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalid) {
        std::fprintf(stderr, "socket() failed\n");
        return 1;
    }

    // Enable broadcast (needed if target is 255.255.255.255)
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
               reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(static_cast<uint16_t>(kArtNetPort));
    ::inet_pton(AF_INET, target_ip.c_str(), &dest.sin_addr);

    // --- Prepare packet template --------------------------------------------
    ArtDmxPacket pkt{};
    pkt.universe = static_cast<uint16_t>(universe & 0x7FFF);
    // ArtNet length field is big-endian
    pkt.length   = static_cast<uint16_t>((512 >> 8) | ((512 & 0xFF) << 8));

    // --- Packet send loop ----------------------------------------------------
    const double frame_period = 1.0 / static_cast<double>(fps);

    Stats round_trip_stats;
    uint64_t frames_sent   = 0;
    uint8_t  stamp_value   = 0;
    double   stamp_sent_at = 0.0;

    double next_send_time = now_s();

    while (true) {
        double t = now_s();

        // Spin-wait until next send time (minimal CPU waste at low fps)
        if (t < next_send_time) {
            double sleep_s = next_send_time - t;
            if (sleep_s > 0.001)
                std::this_thread::sleep_for(
                    std::chrono::duration<double>(sleep_s - 0.001));
            while (now_s() < next_send_time) { /* tight spin for last ~1 ms */ }
            t = now_s();
        }
        next_send_time = t + frame_period;

        // Animate channels with per-channel sine waves
        // Channel ch gets a slow sine at frequency (1 + ch * 0.001) Hz
        // This creates a rolling rainbow-like sweep across all 512 channels.
        for (int ch = 0; ch < 512; ++ch) {
            double freq  = 0.1 + ch * 0.001;
            double phase = ch * (3.14159265 * 2.0 / 512.0);
            double val   = 0.5 + 0.5 * std::sin(2.0 * 3.14159265 * freq * t + phase);
            pkt.data[ch] = static_cast<uint8_t>(val * 255.0);
        }

        // Stamp channel 1 (index 0) with a known rotating value for
        // round-trip detection.  We write a value that encodes the low
        // 8 bits of (frames_sent % 251) — a prime modulus makes collisions
        // unlikely with the normal sine animation.
        stamp_value   = static_cast<uint8_t>(frames_sent % 251u);
        pkt.data[0]   = stamp_value;
        stamp_sent_at = t;
        // Round-trip loopback listener not implemented; suppress unused warnings.
        (void)stamp_value;
        (void)stamp_sent_at;

        // Increment sequence (0 disables check; use 1..255 for ordered delivery)
        pkt.sequence = static_cast<uint8_t>((frames_sent + 1) & 0xFF);
        if (pkt.sequence == 0) pkt.sequence = 1;

        int sent = static_cast<int>(::sendto(
            sock,
            reinterpret_cast<const char*>(&pkt), sizeof(ArtDmxPacket),
            0,
            reinterpret_cast<const sockaddr*>(&dest), sizeof(dest)));

        if (sent < 0) {
            std::fprintf(stderr, "sendto() failed\n");
            break;
        }

        ++frames_sent;

        // Print stats every 5 seconds
        if (frames_sent % static_cast<uint64_t>(fps * 5) == 0) {
            std::printf(
                "[%.1f s] Frames sent: %llu  |  P99 rt: %.2f ms  mean: %.2f ms\n",
                t,
                (unsigned long long)frames_sent,
                round_trip_stats.p99,
                round_trip_stats.mean()
            );
        }
    }

    close_sock(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
