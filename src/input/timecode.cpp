// timecode.cpp — LTC, MTC, and ArtNet Timecode decoder implementation.
//
// LTC algorithm reference:
//   SMPTE ST 12-1:2014 — Temporal Addressing
//   ltc_decoder from libltc (reference, not a dependency — independent impl)
//
// MTC algorithm reference:
//   MIDI 1.0 Detailed Specification, Appendix B (Quarter-Frame messages)
//
// Thread safety:
//   All public methods are safe to call from any thread.
//   Internal state is protected by std::atomic<TimecodeState> (trivially copyable).
//   std::atomic<TimecodeState> requires TimecodeState to be trivially copyable —
//   verified by static_assert below.

#include "timecode.h"

#include <cstring>
#include <cmath>
#include <algorithm>

namespace idhmfis {

static_assert(std::is_trivially_copyable_v<TimecodeState>,
    "TimecodeState must be trivially copyable for std::atomic<TimecodeState>");

// ─────────────────────────────────────────────────────────────────────────────
//  MtcDecoder
// ─────────────────────────────────────────────────────────────────────────────
//
//  MTC quarter-frame message format: 0xF1 nn
//  nn = piece_type (3 bits, high nibble) | data_nibble (4 bits, low nibble)
//
//  Piece types (0-7):
//   0: frames   low  nibble
//   1: frames   high nibble (bits 5-4)
//   2: seconds  low  nibble
//   3: seconds  high nibble (bits 6-4)
//   4: minutes  low  nibble
//   5: minutes  high nibble (bits 6-4)
//   6: hours    low  nibble
//   7: hours    high nibble (bits 4 = hour bit 4; bits 6-5 = frame-rate type)
//
//  After receiving piece 7, assemble a full frame.
//  Direction: pieces 0..7 = forward; 7..0 = reverse (we ignore direction).

void MtcDecoder::feed_quarter_frame(uint8_t qf_byte) {
    int  piece = (qf_byte >> 4) & 0x07;
    uint8_t nib = qf_byte & 0x0F;

    nibbles_[piece] = nib;

    // Count sequential pieces; only commit after 8 unique pieces received
    if (last_piece_ < 0 || piece == ((last_piece_ + 1) & 7)) {
        ++piece_count_;
    } else {
        // Out of sequence — reset count
        piece_count_ = 1;
        frame_valid_ = false;
    }
    last_piece_ = piece;

    // We have a complete set when we've seen 8 sequential pieces
    if (piece_count_ >= 8 && piece == 7) {
        frame_valid_ = true;
        piece_count_ = 0;

        TimecodeState tc{};

        // Frames: nibbles[0] = low 4 bits, nibbles[1] = bits [5:4]
        tc.frames  = static_cast<uint8_t>((nibbles_[1] & 0x01) << 4 | nibbles_[0]);

        // Seconds: nibbles[2] = low 4 bits, nibbles[3] = bits [6:4]
        tc.seconds = static_cast<uint8_t>((nibbles_[3] & 0x03) << 4 | nibbles_[2]);

        // Minutes: nibbles[4] = low 4 bits, nibbles[5] = bits [6:4]
        tc.minutes = static_cast<uint8_t>((nibbles_[5] & 0x03) << 4 | nibbles_[4]);

        // Hours: nibbles[6] = low 4 bits, nibbles[7]: bit3-2 = frame-rate, bit0 = hour bit4
        tc.hours   = static_cast<uint8_t>((nibbles_[7] & 0x01) << 4 | nibbles_[6]);

        // Frame rate: bits 6-5 of nibbles[7] (i.e. bits 2-1 after masking)
        uint8_t rate_bits = (nibbles_[7] >> 1) & 0x03;
        switch (rate_bits) {
            case 0: tc.fps = SmpteRate::Fps24;   break;
            case 1: tc.fps = SmpteRate::Fps25;   break;
            case 2: tc.fps = SmpteRate::Fps2997; break;
            case 3: tc.fps = SmpteRate::Fps30;   break;
        }

        // Clamp to valid ranges
        tc.frames  = std::min(tc.frames,  static_cast<uint8_t>(smpte_max_frames(tc.fps) - 1));
        tc.seconds = std::min(tc.seconds, static_cast<uint8_t>(59));
        tc.minutes = std::min(tc.minutes, static_cast<uint8_t>(59));
        tc.hours   = std::min(tc.hours,   static_cast<uint8_t>(23));

        tc.source = TimecodeSource::MTC;
        tc.valid  = true;

        state_.store(tc, std::memory_order_release);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  LtcDecoder
// ─────────────────────────────────────────────────────────────────────────────
//
//  LTC frame structure (80 bits, LSB first per nibble):
//  Bit  0- 3: Frame units  (BCD)
//  Bit  4- 7: User bits 1
//  Bit  8- 9: Frame tens   (BCD)
//  Bit 10   : Drop-frame flag
//  Bit 11   : Color-frame flag
//  Bit 12-15: User bits 2
//  Bit 16-19: Seconds units (BCD)
//  Bit 20-23: User bits 3
//  Bit 24-26: Seconds tens (BCD, max 5)
//  Bit 27   : Biphase-mark parity bit
//  Bit 28-31: User bits 4
//  Bit 32-35: Minutes units (BCD)
//  Bit 36-39: User bits 5
//  Bit 40-42: Minutes tens (BCD, max 5)
//  Bit 43   : BGF0
//  Bit 44-47: User bits 6
//  Bit 48-51: Hours units (BCD)
//  Bit 52-55: User bits 7
//  Bit 56-57: Hours tens (BCD, max 2)
//  Bit 58   : BGF1 / clock flag
//  Bit 59   : BGF2
//  Bit 60-63: User bits 8
//  Bit 64-79: Sync word = 0011111111111101 (LS bit first → 1011111111111100)
//
//  Biphase-M encoding:
//    - Always a transition at every bit boundary.
//    - A "1" bit additionally transitions at the midpoint of the bit cell.
//    - A "0" bit has no midpoint transition.
//  So: if the interval between transitions is approximately T/2, it's a 1-bit
//      transition (midpoint). If it's approximately T (full bit), it's a
//      boundary-only transition indicating bit 0.
//
//  This decoder operates on raw PCM levels (not pre-filtered).
//  Suitable for 44100 or 48000 Hz audio at 24/25/29.97/30 fps.

LtcDecoder::LtcDecoder(int sample_rate)
    : sample_rate_(sample_rate) {
    reset();
}

void LtcDecoder::reset() {
    last_sample_      = 0;
    samples_since_zc_ = 0;
    half_cell_        = 0;
    in_half_cell_     = false;
    half_since_zc_    = 0;
    avg_half_cell_    = 0.0;
    half_cell_count_  = 0;
    bit_count_        = 0;
    last_polarity_    = false;
    std::memset(bit_buf_, 0, sizeof(bit_buf_));
    TimecodeState blank{};
    state_.store(blank, std::memory_order_release);
}

void LtcDecoder::process_f32(const float* samples, int count) {
    for (int i = 0; i < count; ++i) {
        // Convert float to 8-bit signed
        float f = samples[i];
        if (f >  1.0f) f =  1.0f;
        if (f < -1.0f) f = -1.0f;
        int8_t s = static_cast<int8_t>(f * 127.0f);
        process(&s, 1);
    }
}

void LtcDecoder::process(const int8_t* samples, int count) {
    // Expected half-cell range at common frame rates and sample rates.
    // At 48kHz / 25fps: 80 bits/frame × 25 fps = 2000 bits/s
    // Half-cell = 48000 / (2000 * 2) = 12 samples at full speed.
    // We accept 0.3× to 3.0× the running average to tolerate pitch variations.

    for (int i = 0; i < count; ++i) {
        int8_t s = samples[i];
        ++samples_since_zc_;

        // Detect zero-crossing with hysteresis (L-15).
        // The original comparison (s <= 0 / s >= 0) includes the zero value on
        // both sides, so a near-zero input bouncing around 0 produces multiple
        // spurious crossings per half-cell.  Require the new sample to cross a
        // small dead-band (±3 out of ±127, ~2.4%) before declaring a crossing.
        // The threshold is small enough to be invisible at any real LTC level
        // but large enough to reject 8-bit ADC noise around zero.
        static constexpr int8_t kHyst = 3;
        bool zc = false;
        if (last_sample_ > kHyst  && s < -kHyst) zc = true;
        else if (last_sample_ < -kHyst && s > kHyst)  zc = true;

        if (zc) {
            int interval = samples_since_zc_;
            samples_since_zc_ = 0;

            // Update running half-cell estimate (exponential moving average)
            if (avg_half_cell_ < 1.0) {
                avg_half_cell_ = static_cast<double>(interval);
            } else {
                avg_half_cell_ = avg_half_cell_ * 0.95 + interval * 0.05;
            }
            ++half_cell_count_;

            // Classify: half-cell (~T/2) → interior transition → bit 1 part
            //           full-cell (~T)   → boundary only → bit 0 boundary
            // We determine the bit value at every *second* transition (boundary).
            double t_half = avg_half_cell_;
            double lo = t_half * 0.4;
            double hi = t_half * 1.6;

            bool is_half_cell = (interval < hi && interval > lo * 0.5);

            if (!in_half_cell_) {
                if (is_half_cell) {
                    // We're at the first half of a biphase-1 bit
                    in_half_cell_ = true;
                } else {
                    // Full-cell boundary only → bit 0
                    on_bit(0);
                }
            } else {
                // Second half of biphase-1 → complete bit 1
                in_half_cell_ = false;
                on_bit(1);
            }
        }

        last_sample_ = s;
    }
}

// Called with each decoded bit (biphase-M decoded)
void LtcDecoder::on_bit(int bit) {
    if (bit_count_ < kBitsPerFrame) {
        bit_buf_[bit_count_++] = bit;
    }

    // Try to detect sync word at any position by scanning the last 16 bits
    if (bit_count_ >= 16) {
        // LTC sync word at end of frame (bits 64-79):
        // 0011 1111 1111 1101 (bit 64 = LSB of sync word)
        // In biphase-M decoded bits, the sync pattern in the stream is:
        // check bits [bit_count_-16 .. bit_count_-1] == sync word
        // LTC sync word bits 64-79: 0,0,1,1,1,1,1,1,1,1,1,1,1,1,0,1
        static const int kSync[16] = {0,0,1,1, 1,1,1,1, 1,1,1,1, 1,1,0,1};
        int base = bit_count_ - 16;
        bool match = true;
        for (int j = 0; j < 16; ++j) {
            if (bit_buf_[base + j] != kSync[j]) { match = false; break; }
        }

        if (match && base == 64) {
            // We have exactly 80 bits with sync at end — decode the frame
            try_decode_frame();
            bit_count_ = 0;
        } else if (match && base != 64) {
            // BUG #19 fix: sync found but not at the expected position (base != 64)
            // means the 64 data bits preceding the sync word are not correctly
            // aligned in bit_buf_, so calling try_decode_frame() here would
            // extract garbage fields.  Skip the decode and just reset the
            // bit counter to re-sync on the next incoming frame.
            bit_count_ = 0;
        }
    }

    // Overflow guard: if we've accumulated more than 80 bits without a sync,
    // shift the buffer left by 1 to search for sync word alignment.
    if (bit_count_ >= kBitsPerFrame) {
        std::memmove(bit_buf_, bit_buf_ + 1, (kBitsPerFrame - 1) * sizeof(int));
        bit_count_ = kBitsPerFrame - 1;
    }
}

// Extract BCD digit value from bit_buf_
static int extract_bcd(const int* bits, int start, int num) {
    int val = 0;
    for (int i = 0; i < num; ++i) {
        if (bits[start + i]) val |= (1 << i);
    }
    return val;
}

void LtcDecoder::try_decode_frame() {
    // Extract fields from bit_buf_ (80 bits, LSB-first per nibble)
    // Frame units: bits 0-3
    int frame_units = extract_bcd(bit_buf_, 0, 4);
    // Frame tens: bits 8-9
    int frame_tens  = extract_bcd(bit_buf_, 8, 2);
    // Drop-frame: bit 10
    bool drop_frame  = (bit_buf_[10] != 0);

    // Seconds units: bits 16-19
    int sec_units   = extract_bcd(bit_buf_, 16, 4);
    // Seconds tens: bits 24-26
    int sec_tens    = extract_bcd(bit_buf_, 24, 3);

    // Minutes units: bits 32-35
    int min_units   = extract_bcd(bit_buf_, 32, 4);
    // Minutes tens: bits 40-42
    int min_tens    = extract_bcd(bit_buf_, 40, 3);

    // Hours units: bits 48-51
    int hr_units    = extract_bcd(bit_buf_, 48, 4);
    // Hours tens: bits 56-57
    int hr_tens     = extract_bcd(bit_buf_, 56, 2);

    // Clock flag / BGF1: bit 58 → frame-rate encoding for 25fps timecode
    // In LTC, the frame-rate is NOT encoded per-frame; it depends on the
    // source. We infer it from drop_frame and the hours-tens bits pattern.
    // For practical use: if drop_frame flag is set → 29.97 df; else 25 or 30.
    // We can't reliably distinguish 24/25/30 without external info.
    // Default to 25fps; application can override via TimecodeRouter::set_preferred.
    SmpteRate fps = drop_frame ? SmpteRate::Fps2997 : SmpteRate::Fps25;

    TimecodeState tc{};
    tc.frames  = static_cast<uint8_t>(frame_tens * 10 + frame_units);
    tc.seconds = static_cast<uint8_t>(sec_tens   * 10 + sec_units);
    tc.minutes = static_cast<uint8_t>(min_tens   * 10 + min_units);
    tc.hours   = static_cast<uint8_t>(hr_tens    * 10 + hr_units);
    tc.fps     = fps;
    tc.source  = TimecodeSource::LTC;
    tc.valid   = true;

    // Sanity check
    if (tc.seconds > 59 || tc.minutes > 59 || tc.hours > 23
        || tc.frames >= static_cast<uint8_t>(smpte_max_frames(tc.fps))) {
        return; // garbage frame
    }

    state_.store(tc, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
//  TimecodeRouter
// ─────────────────────────────────────────────────────────────────────────────

void TimecodeRouter::on_artnet_tc(const TimecodeState& tc) {
    TimecodeState t = tc;
    t.source = TimecodeSource::ArtNetTC;
    t.valid  = true;
    artnet_tc_.store(t, std::memory_order_release);
    artnet_updated_ns_.store(now_ns(), std::memory_order_release);
}

void TimecodeRouter::feed_mtc(uint8_t qf_byte) {
    // Remember the frame counter before feeding so we can detect a new decode.
    bool was_valid = mtc_.state().valid;
    int64_t old_frames = was_valid ? mtc_.state().total_frames() : -1;

    mtc_.feed_quarter_frame(qf_byte);

    // Only advance the timestamp when a new complete frame has been assembled.
    // Updating on every quarter-frame would make the source appear fresh even
    // when only 1-of-8 pieces have arrived, keeping a stale timecode value live.
    TimecodeState s = mtc_.state();
    if (s.valid && s.total_frames() != old_frames) {
        mtc_updated_ns_.store(now_ns(), std::memory_order_release);
    }
}

void TimecodeRouter::feed_ltc(const int8_t* samples, int count, int /*sample_rate*/) {
    TimecodeState before = ltc_.state();
    int64_t old_frames = before.valid ? before.total_frames() : -1;
    ltc_.process(samples, count);
    TimecodeState after = ltc_.state();
    if (after.valid && after.total_frames() != old_frames)
        ltc_updated_ns_.store(now_ns(), std::memory_order_release);
}

void TimecodeRouter::feed_ltc_f32(const float* samples, int count, int /*sample_rate*/) {
    TimecodeState before = ltc_.state();
    int64_t old_frames = before.valid ? before.total_frames() : -1;
    ltc_.process_f32(samples, count);
    TimecodeState after = ltc_.state();
    if (after.valid && after.total_frames() != old_frames)
        ltc_updated_ns_.store(now_ns(), std::memory_order_release);
}

TimecodeState TimecodeRouter::active() const {
    int64_t now = now_ns();

    TimecodeSource pref = preferred_.load(std::memory_order_relaxed);
    if (pref != TimecodeSource::Internal) {
        // Forced source
        switch (pref) {
            case TimecodeSource::ArtNetTC: {
                int64_t age = now - artnet_updated_ns_.load(std::memory_order_acquire);
                if (age < kStaleNs) return artnet_tc_.load(std::memory_order_acquire);
                break;
            }
            case TimecodeSource::MTC: {
                int64_t age = now - mtc_updated_ns_.load(std::memory_order_acquire);
                if (age < kStaleNs) return mtc_.state();
                break;
            }
            case TimecodeSource::LTC: {
                int64_t age = now - ltc_updated_ns_.load(std::memory_order_acquire);
                if (age < kStaleNs) return ltc_.state();
                break;
            }
            default: break;
        }
    }

    // Auto-priority: ArtNetTC > MTC > LTC > Internal
    {
        int64_t age = now - artnet_updated_ns_.load(std::memory_order_acquire);
        if (age < kStaleNs) return artnet_tc_.load(std::memory_order_acquire);
    }
    {
        int64_t age = now - mtc_updated_ns_.load(std::memory_order_acquire);
        if (age < kStaleNs) return mtc_.state();
    }
    {
        int64_t age = now - ltc_updated_ns_.load(std::memory_order_acquire);
        if (age < kStaleNs) return ltc_.state();
    }

    // Internal — return zero timecode
    TimecodeState blank{};
    blank.source = TimecodeSource::Internal;
    blank.valid  = false;
    return blank;
}

} // namespace idhmfis
