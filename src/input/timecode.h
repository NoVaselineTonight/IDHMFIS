#pragma once
// Timecode decoder: LTC (SMPTE linear timecode from audio), MTC (MIDI timecode
// quarter-frame messages), and ArtNet Timecode (OpTimeCode 0x9700, already
// parsed by ArtNetListener::handle_arttimecode — forwarded here).
//
// All three sources feed into a single TimecodeState published via an atomic
// pointer swap for zero-copy consumption on the engine thread.
//
// LTC detection uses a zero-crossing bit-width detector on 8-bit PCM audio.
// MTC reconstruction uses the standard 8-quarter-frame full-frame assembly.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <array>
#include <chrono>
#include <thread>
#include <functional>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  TimecodeSource — which sync source is currently active
// ─────────────────────────────────────────────────────────────────────────────
enum class TimecodeSource : uint8_t {
    Internal   = 0,  // engine clock (free-run)
    LTC        = 1,  // SMPTE LTC from audio
    MTC        = 2,  // MIDI Timecode quarter-frames
    ArtNetTC   = 3,  // Art-Net OpTimeCode 0x9700
};

// ─────────────────────────────────────────────────────────────────────────────
//  SMPTE frame rate codes (matches Art-Net 4 spec §9.141)
// ─────────────────────────────────────────────────────────────────────────────
enum class SmpteRate : uint8_t {
    Fps24      = 0,
    Fps25      = 1,
    Fps2997    = 2,  // 29.97 drop-frame
    Fps30      = 3,
};

// Max frames per rate
inline int smpte_max_frames(SmpteRate r) {
    switch (r) {
        case SmpteRate::Fps24:   return 24;
        case SmpteRate::Fps25:   return 25;
        case SmpteRate::Fps2997: return 30;  // 29.97 counts to 29
        case SmpteRate::Fps30:   return 30;
    }
    return 30;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TimecodeState — POD timecode value + source metadata
// ─────────────────────────────────────────────────────────────────────────────
struct TimecodeState {
    uint8_t       hours   = 0;
    uint8_t       minutes = 0;
    uint8_t       seconds = 0;
    uint8_t       frames  = 0;
    SmpteRate     fps     = SmpteRate::Fps25;
    TimecodeSource source = TimecodeSource::Internal;
    bool          valid   = false;  // false = no timecode lock

    // Absolute frame index for chase arithmetic
    int64_t total_frames() const {
        int fps_int = smpte_max_frames(fps);
        return static_cast<int64_t>(hours)   * 3600 * fps_int
             + static_cast<int64_t>(minutes) *   60 * fps_int
             + static_cast<int64_t>(seconds) *        fps_int
             + static_cast<int64_t>(frames);
    }

    // Elapsed seconds as a double
    double elapsed_s() const {
        double fps_d = (fps == SmpteRate::Fps2997) ? 29.97 : static_cast<double>(smpte_max_frames(fps));
        return hours * 3600.0 + minutes * 60.0 + seconds + frames / fps_d;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  MtcDecoder — assembles 8 MTC quarter-frame messages into a full frame
//  Call feed_quarter_frame() from your MIDI callback on any thread.
//  Call state() from any thread — uses atomics for thread safety.
// ─────────────────────────────────────────────────────────────────────────────
class MtcDecoder {
public:
    MtcDecoder() = default;

    // Feed a single MTC quarter-frame byte (status 0xF1, data is the QF byte).
    // piece = high nibble (0-7), value = low nibble (0-F)
    void feed_quarter_frame(uint8_t qf_byte);

    TimecodeState state() const {
        return state_.load(std::memory_order_acquire);
    }

    void reset() {
        piece_count_  = 0;
        frame_valid_  = false;
        TimecodeState blank{};
        state_.store(blank, std::memory_order_release);
    }

private:
    // 8 nibble registers indexed by piece number (0-7)
    uint8_t nibbles_[8] = {};
    int     piece_count_ = 0;
    bool    frame_valid_ = false;
    int     last_piece_  = -1;

    std::atomic<TimecodeState> state_{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  LtcDecoder — SMPTE LTC from a stream of 8-bit PCM audio samples
//
//  LTC encodes time as bi-phase mark (biphase-M) at 80 bits/frame.
//  Bit encoding:
//    0 = no transition at middle of bit cell
//    1 = transition at middle (PLUS always transition at bit boundary)
//  Sync word (end of frame): 0011111111111101
//
//  This decoder:
//    1. Detects zero-crossings to find bit-cell boundaries.
//    2. Measures half-cell widths to distinguish 0 and 1.
//    3. Assembles 80-bit LTC words and validates the sync pattern.
//    4. Publishes TimecodeState when a valid frame is decoded.
// ─────────────────────────────────────────────────────────────────────────────
class LtcDecoder {
public:
    explicit LtcDecoder(int sample_rate = 48000);

    // Process a buffer of mono 8-bit PCM samples (signed, -128..+127).
    // Call from audio callback on any thread.
    void process(const int8_t* samples, int count);

    // Process a buffer of mono float samples (-1.0..+1.0).
    void process_f32(const float* samples, int count);

    TimecodeState state() const {
        return state_.load(std::memory_order_acquire);
    }

    void reset();

private:
    void on_bit(int bit);
    void try_decode_frame();

    int sample_rate_;

    // Zero-crossing detector
    int8_t   last_sample_     = 0;
    int      samples_since_zc_= 0;  // samples since last zero-crossing
    int      half_cell_       = 0;  // measured half-cell width in samples
    bool     in_half_cell_    = false;
    int      half_since_zc_   = 0;

    // Adaptive half-cell width estimator
    double   avg_half_cell_   = 0.0;
    int      half_cell_count_ = 0;

    // Bit accumulator (80 bits per LTC frame, LSB first per byte)
    static constexpr int kBitsPerFrame = 80;
    int      bit_buf_[kBitsPerFrame] = {};
    int      bit_count_ = 0;
    bool     last_polarity_ = false;

    std::atomic<TimecodeState> state_{};
};

// ─────────────────────────────────────────────────────────────────────────────
//  TimecodeRouter — combines all three sources, selects active one
//
//  Priority (highest to lowest): ArtNetTC > MTC > LTC > Internal
//  A source is considered "dead" if it has not updated for > 500 ms.
// ─────────────────────────────────────────────────────────────────────────────
class TimecodeRouter {
public:
    TimecodeRouter() = default;

    // Called by ArtNetListener when an OpTimeCode packet arrives
    void on_artnet_tc(const TimecodeState& tc);

    // Feed the MTC decoder a quarter-frame byte
    void feed_mtc(uint8_t qf_byte);

    // Feed the LTC decoder audio samples
    void feed_ltc(const int8_t* samples, int count, int sample_rate = 48000);
    void feed_ltc_f32(const float* samples, int count, int sample_rate = 48000);

    // Get the current best timecode (engine thread)
    TimecodeState active() const;

    // Force a specific source (0 = auto)
    void set_preferred(TimecodeSource src) {
        preferred_.store(src, std::memory_order_relaxed);
    }

    TimecodeSource preferred() const {
        return preferred_.load(std::memory_order_relaxed);
    }

private:
    using Clock = std::chrono::steady_clock;
    using ns    = std::chrono::nanoseconds;

    static int64_t now_ns() {
        return std::chrono::duration_cast<ns>(
            Clock::now().time_since_epoch()).count();
    }

    MtcDecoder mtc_;
    LtcDecoder ltc_;

    // ArtNet TC is provided pre-decoded (ArtNetListener parses it)
    std::atomic<TimecodeState> artnet_tc_{};
    std::atomic<int64_t>       artnet_updated_ns_{0};

    std::atomic<int64_t>       mtc_updated_ns_{0};
    std::atomic<int64_t>       ltc_updated_ns_{0};

    std::atomic<TimecodeSource> preferred_{TimecodeSource::Internal};

    static constexpr int64_t kStaleNs = 500'000'000LL; // 500 ms
};

} // namespace idhmfis
