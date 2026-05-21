// frame_converter.cpp — BGRA ↔ UYVY pixel format conversion.
// BT.601 coefficients (studio swing, limited range):
//   Y  =  16 + (65.481*R + 128.553*G +  24.966*B) / 255
//   Cb = 128 + (-37.797*R -  74.203*G + 112.0*B)  / 255
//   Cr = 128 + (112.0*R  -  93.786*G -  18.214*B) / 255
//
// Full-range BT.601 (NDI uses full range by default):
//   Y  = (77*R + 150*G +  29*B) >> 8
//   Cb = 128 + (-43*R -  85*G + 128*B) >> 8
//   Cr = 128 + (128*R - 107*G -  21*B) >> 8
//
// SSE4.1 processes 8 BGRA pixels (32 bytes) → 8 UYVY bytes (16 bytes) per iteration.
// NEON processes 8 pixels similarly.

#include "frame_converter.h"

#include <cstring>
#include <algorithm>

// ---- SIMD capability detection ----
#if defined(__x86_64__) || defined(_M_X64)
#define IDHMFIS_USE_SSE41 1
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#define IDHMFIS_USE_NEON 1
#include <arm_neon.h>
#endif

namespace idhmfis {

// ---------------------------------------------------------------------------
// BT.601 full-range integer coefficients (fixed-point 8.8)
// Packed as (y_r, y_g, y_b, 0) etc. — see SSE implementation below.
// ---------------------------------------------------------------------------

// Scalar reference implementation
static inline void convert_2pixels_to_uyvy_scalar(
    uint8_t b0, uint8_t g0, uint8_t r0,
    uint8_t b1, uint8_t g1, uint8_t r1,
    uint8_t* out_u, uint8_t* out_y0,
    uint8_t* out_v, uint8_t* out_y1)
{
    // Y (full-range BT.601)
    int y0 = (  77 * r0 + 150 * g0 + 29 * b0) >> 8;
    int y1 = (  77 * r1 + 150 * g1 + 29 * b1) >> 8;

    // Cb/Cr — average the two pixels horizontally (4:2:2)
    int ra = (static_cast<int>(r0) + r1) >> 1;
    int ga = (static_cast<int>(g0) + g1) >> 1;
    int ba = (static_cast<int>(b0) + b1) >> 1;
    int cb = 128 + ((-43 * ra -  85 * ga + 128 * ba) >> 8);
    int cr = 128 + ((128 * ra - 107 * ga -  21 * ba) >> 8);

    *out_u  = static_cast<uint8_t>(std::clamp(cb,   0, 255));
    *out_y0 = static_cast<uint8_t>(std::clamp(y0,   0, 255));
    *out_v  = static_cast<uint8_t>(std::clamp(cr,   0, 255));
    *out_y1 = static_cast<uint8_t>(std::clamp(y1,   0, 255));
}

[[maybe_unused]] static void bgra_to_uyvy_scalar(const uint8_t* src, uint8_t* dst,
                                  int width, int height)
{
    for (int row = 0; row < height; ++row) {
        const uint8_t* s = src + row * width * 4;
        uint8_t*       d = dst + row * width * 2;
        for (int col = 0; col < width; col += 2) {
            const uint8_t* p0 = s + col * 4;
            const uint8_t* p1 = p0 + 4;
            // BGRA: B=p[0], G=p[1], R=p[2], A=p[3]
            convert_2pixels_to_uyvy_scalar(
                p0[0], p0[1], p0[2],
                p1[0], p1[1], p1[2],
                &d[col * 2 + 0],  // U
                &d[col * 2 + 1],  // Y0
                &d[col * 2 + 2],  // V
                &d[col * 2 + 3]   // Y1
            );
        }
    }
}

[[maybe_unused]] static void uyvy_to_bgra_scalar(const uint8_t* src, uint8_t* dst,
                                  int width, int height)
{
    for (int row = 0; row < height; ++row) {
        const uint8_t* s = src + row * width * 2;
        uint8_t*       d = dst + row * width * 4;
        for (int col = 0; col < width; col += 2) {
            int cb = static_cast<int>(s[col * 2 + 0]) - 128;
            int y0 = static_cast<int>(s[col * 2 + 1]);
            int cr = static_cast<int>(s[col * 2 + 2]) - 128;
            int y1 = static_cast<int>(s[col * 2 + 3]);

            // BT.601 full-range YCbCr → RGB
            auto clamp_u8 = [](int v) -> uint8_t {
                return static_cast<uint8_t>(std::clamp(v, 0, 255));
            };

            auto yuv_to_rgb = [&](int y, uint8_t* bgra) {
                int r = y                    + (359 * cr >> 8);
                int g = y - ( 88 * cb >> 8) - (183 * cr >> 8);
                int b = y + (454 * cb >> 8);
                bgra[0] = clamp_u8(b);
                bgra[1] = clamp_u8(g);
                bgra[2] = clamp_u8(r);
                bgra[3] = 255;
            };

            yuv_to_rgb(y0, d + col * 4);
            yuv_to_rgb(y1, d + (col + 1) * 4);
        }
    }
}

// ---------------------------------------------------------------------------
// SSE4.1 fast-path (x86_64)
// Processes 8 BGRA pixels (32 input bytes) → 16 UYVY bytes per loop.
// ---------------------------------------------------------------------------
#ifdef IDHMFIS_USE_SSE41

// Multiply packed 16-bit integers and shift right by 8 (unsigned high multiply)
// Equivalent to (a * b) >> 8
static inline __m128i mulhi8_u16(__m128i a, __m128i b) {
    return _mm_srli_epi16(_mm_mullo_epi16(a, b), 8);
}

static void bgra_to_uyvy_sse41(const uint8_t* src, uint8_t* dst,
                                  int width, int height)
{
    // BT.601 full-range coefficients (Q8, i.e. ×256 fixed-point)
    // Y  = (77*R + 150*G + 29*B) >> 8
    // Cb = 128 + (-43*R - 85*G + 128*B) >> 8   [use unsigned then subtract]
    // Cr = 128 + (128*R - 107*G - 21*B) >> 8

    const __m128i c_yr  = _mm_set1_epi16(77);
    const __m128i c_yg  = _mm_set1_epi16(150);
    const __m128i c_yb  = _mm_set1_epi16(29);

    // For Cb: use absolute values, then negate partial sums
    const __m128i c_cbr = _mm_set1_epi16(43);
    const __m128i c_cbg = _mm_set1_epi16(85);
    const __m128i c_cbb = _mm_set1_epi16(128);

    const __m128i c_crr = _mm_set1_epi16(128);
    const __m128i c_crg = _mm_set1_epi16(107);
    const __m128i c_crb = _mm_set1_epi16(21);

    const __m128i bias128 = _mm_set1_epi16(128);
    const __m128i zero    = _mm_setzero_si128();

    for (int row = 0; row < height; ++row) {
        const uint8_t* s = src + row * width * 4;
        uint8_t*       d = dst + row * width * 2;

        int col = 0;
        // Process 8 pixels (2 UYVY macro-blocks of 4) per iteration
        for (; col + 8 <= width; col += 8) {
            // Load 8 BGRA pixels = 32 bytes
            __m128i bgra0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + col * 4));
            __m128i bgra1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + col * 4 + 16));

            // Unpack 8-bit channels to 16-bit
            // bgra0 = [B0 G0 R0 A0 | B1 G1 R1 A1 | B2 G2 R2 A2 | B3 G3 R3 A3]
            // Separate into B, G, R channels (16-bit)
            // Use _mm_shuffle_epi8 to extract channels

            // Extract R, G, B from both 128-bit registers (4 pixels each)
            // Pixel channel layout: B G R A B G R A B G R A B G R A
            //                       0 1 2 3 4 5 6 7 8 9 A B C D E F
            const __m128i shuf_b = _mm_set_epi8(
                -1,12, -1, 8, -1, 4, -1, 0,
                -1,12, -1, 8, -1, 4, -1, 0);
            const __m128i shuf_g = _mm_set_epi8(
                -1,13, -1, 9, -1, 5, -1, 1,
                -1,13, -1, 9, -1, 5, -1, 1);
            const __m128i shuf_r = _mm_set_epi8(
                -1,14, -1,10, -1, 6, -1, 2,
                -1,14, -1,10, -1, 6, -1, 2);

            // --- Process first 4 pixels (bgra0) ---
            __m128i b0 = _mm_shuffle_epi8(bgra0, shuf_b);  // [B0 B1 B2 B3 B0 B1 B2 B3]  16-bit
            __m128i g0 = _mm_shuffle_epi8(bgra0, shuf_g);
            __m128i r0 = _mm_shuffle_epi8(bgra0, shuf_r);

            // Extract lower 4 x 16-bit words (first 4 pixels)
            b0 = _mm_and_si128(b0, _mm_set_epi64x(0LL, 0x0000FFFFFFFFFFFF));
            g0 = _mm_and_si128(g0, _mm_set_epi64x(0LL, 0x0000FFFFFFFFFFFF));
            r0 = _mm_and_si128(r0, _mm_set_epi64x(0LL, 0x0000FFFFFFFFFFFF));

            // Unpack to full 16-bit (zero extend)
            b0 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(bgra0,
                _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1, 12, 8, 4, 0, 12, 8, 4, 0)));
            g0 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(bgra0,
                _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1, 13, 9, 5, 1, 13, 9, 5, 1)));
            r0 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(bgra0,
                _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1, 14,10, 6, 2, 14,10, 6, 2)));

            // Y for 4 pixels
            __m128i y0 = _mm_add_epi16(
                _mm_add_epi16(mulhi8_u16(r0, c_yr), mulhi8_u16(g0, c_yg)),
                mulhi8_u16(b0, c_yb));

            // Cb (pair-averaged) — average pixels 0,1 and 2,3
            // Horizontal add neighbouring pairs for averaging
            // For simplicity, replicate the scalar approach with intrinsics:
            // avg_r = (r0 + r1) / 2 for pair (0,1) and (2,3)
            __m128i r0_dup = _mm_shufflelo_epi16(r0, _MM_SHUFFLE(3,3,1,1));
            r0_dup = _mm_shufflehi_epi16(r0_dup, _MM_SHUFFLE(3,3,1,1));
            // ... this gets complex for 4:2:2 horizontal chroma downsampling
            // Use the simpler approach: compute per-pixel Cb/Cr, then average pairs

            __m128i cb0 = _mm_sub_epi16(
                _mm_add_epi16(bias128, mulhi8_u16(b0, c_cbb)),
                _mm_add_epi16(mulhi8_u16(r0, c_cbr), mulhi8_u16(g0, c_cbg)));
            __m128i cr0 = _mm_sub_epi16(
                _mm_add_epi16(bias128, mulhi8_u16(r0, c_crr)),
                _mm_add_epi16(mulhi8_u16(g0, c_crg), mulhi8_u16(b0, c_crb)));

            // Average neighbouring Cb/Cr pairs (horizontal 4:2:2 downsampling)
            // cb_avg[0] = (cb[0] + cb[1]) / 2, cb_avg[1] = (cb[2] + cb[3]) / 2
            __m128i cb0_avg = _mm_srli_epi16(
                _mm_add_epi16(cb0, _mm_shufflelo_epi16(cb0, _MM_SHUFFLE(2,3,0,1))),
                1);
            __m128i cr0_avg = _mm_srli_epi16(
                _mm_add_epi16(cr0, _mm_shufflelo_epi16(cr0, _MM_SHUFFLE(2,3,0,1))),
                1);

            // --- Process second 4 pixels (bgra1) ---
            __m128i b1 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(bgra1,
                _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1, 12, 8, 4, 0, 12, 8, 4, 0)));
            __m128i g1 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(bgra1,
                _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1, 13, 9, 5, 1, 13, 9, 5, 1)));
            __m128i r1 = _mm_cvtepu8_epi16(_mm_shuffle_epi8(bgra1,
                _mm_set_epi8(-1,-1,-1,-1,-1,-1,-1,-1, 14,10, 6, 2, 14,10, 6, 2)));

            __m128i y1 = _mm_add_epi16(
                _mm_add_epi16(mulhi8_u16(r1, c_yr), mulhi8_u16(g1, c_yg)),
                mulhi8_u16(b1, c_yb));

            __m128i cb1 = _mm_sub_epi16(
                _mm_add_epi16(bias128, mulhi8_u16(b1, c_cbb)),
                _mm_add_epi16(mulhi8_u16(r1, c_cbr), mulhi8_u16(g1, c_cbg)));
            __m128i cr1 = _mm_sub_epi16(
                _mm_add_epi16(bias128, mulhi8_u16(r1, c_crr)),
                _mm_add_epi16(mulhi8_u16(g1, c_crg), mulhi8_u16(b1, c_crb)));

            __m128i cb1_avg = _mm_srli_epi16(
                _mm_add_epi16(cb1, _mm_shufflelo_epi16(cb1, _MM_SHUFFLE(2,3,0,1))),
                1);
            __m128i cr1_avg = _mm_srli_epi16(
                _mm_add_epi16(cr1, _mm_shufflelo_epi16(cr1, _MM_SHUFFLE(2,3,0,1))),
                1);

            // Pack 16-bit Y, Cb, Cr down to 8-bit with unsigned saturation
            __m128i y_packed  = _mm_packus_epi16(y0, y1);   // [Y0..Y7]  8 bytes
            __m128i cb_packed = _mm_packus_epi16(cb0_avg, cb1_avg); // [Cb0..Cb3] 8 bytes
            __m128i cr_packed = _mm_packus_epi16(cr0_avg, cr1_avg); // [Cr0..Cr3] 8 bytes

            // Interleave into UYVY format: U Y0 V Y1 U Y2 V Y3 ...
            // y_packed:  Y0 Y1 Y2 Y3 Y4 Y5 Y6 Y7
            // cb_packed: U01 U01 U23 U23 U45 U45 U67 U67  (need to extract even bytes)
            // Extract even Cb/Cr values (every other byte from cb_packed)
            // cb_packed after averaging: positions 0,2,4,6 have the valid values
            // We only need 4 Cb and 4 Cr values for 8 pixels

            // Build output: U0 Y0 V0 Y1  U1 Y2 V1 Y3  U2 Y4 V2 Y5  U3 Y6 V3 Y7
            // y_packed  = [Y0 Y1 Y2 Y3 Y4 Y5 Y6 Y7 | 0 0 0 0 0 0 0 0]
            // cb/cr: take every 2nd entry (indices 0,2,4,6 from low 8 bytes)
            __m128i uv_interleaved = _mm_unpacklo_epi8(cb_packed, cr_packed);
            // = [U0 V0 U0 V0 U1 V1 U1 V1 U2 V2 U2 V2 U3 V3 U3 V3]
            // We want [U0 V0 U1 V1 U2 V2 U3 V3] — take bytes 0,1,4,5,8,9,12,13
            const __m128i shuf_uv = _mm_set_epi8(
                -1,-1,-1,-1,-1,-1,-1,-1, 12, 8, 4, 0, 13, 9, 5, 1);
            // Actually rearrange: bytes 0=U0, 1=V0, 4=U1, 5=V1, ...
            // After unpacklo: b0=U0, b1=V0, b2=U0, b3=V0, b4=U1, b5=V1, b6=U1, b7=V1...
            // We want every pair (b0,b1), (b4,b5), (b8,b9), (b12,b13)
            const __m128i shuf_uv2 = _mm_set_epi8(
                -1,-1,-1,-1,-1,-1,-1,-1, 13,12, 9, 8, 5, 4, 1, 0);
            __m128i uv4 = _mm_shuffle_epi8(uv_interleaved, shuf_uv2);
            // uv4 low 8 bytes: [U0 V0 U1 V1 U2 V2 U3 V3]

            // Now interleave with Y: U0 Y0 V0 Y1  U1 Y2 V1 Y3 ...
            // y_packed low 8 bytes: Y0 Y1 Y2 Y3 Y4 Y5 Y6 Y7
            // Output order: U0 Y0 V0 Y1 U1 Y2 V1 Y3 U2 Y4 V2 Y5 U3 Y6 V3 Y7
            // = interleave [U0 V0 U1 V1 U2 V2 U3 V3] and [Y0 Y1 Y2 Y3 Y4 Y5 Y6 Y7]
            // with specific shuffle
            const __m128i shuf_y = _mm_set_epi8(
                7, -1, 6, -1, 5, -1, 4, -1, 3, -1, 2, -1, 1, -1, 0, -1);
            const __m128i shuf_uv3 = _mm_set_epi8(
                -1, 7,-1, 6,-1, 5,-1, 4,-1, 3,-1, 2,-1, 1,-1, 0);
            __m128i out_low  = _mm_or_si128(
                _mm_shuffle_epi8(y_packed, shuf_y),
                _mm_shuffle_epi8(uv4, shuf_uv3));

            // Store 16 bytes
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + col * 2), out_low);
        }

        // Scalar remainder
        for (; col < width; col += 2) {
            const uint8_t* p0 = s + col * 4;
            const uint8_t* p1 = p0 + 4;
            convert_2pixels_to_uyvy_scalar(
                p0[0], p0[1], p0[2], p1[0], p1[1], p1[2],
                &d[col * 2 + 0], &d[col * 2 + 1],
                &d[col * 2 + 2], &d[col * 2 + 3]);
        }
    }
}

#endif // IDHMFIS_USE_SSE41

// ---------------------------------------------------------------------------
// NEON fast-path (AArch64)
// ---------------------------------------------------------------------------
#ifdef IDHMFIS_USE_NEON

static void bgra_to_uyvy_neon(const uint8_t* src, uint8_t* dst,
                                int width, int height)
{
    // BT.601 full-range coefficients (Q8)
    const int16x8_t c_yr  = vdupq_n_s16(77);
    const int16x8_t c_yg  = vdupq_n_s16(150);
    const int16x8_t c_yb  = vdupq_n_s16(29);
    const int16x8_t c_cbr = vdupq_n_s16(-43);
    const int16x8_t c_cbg = vdupq_n_s16(-85);
    const int16x8_t c_cbb = vdupq_n_s16(128);
    const int16x8_t c_crr = vdupq_n_s16(128);
    const int16x8_t c_crg = vdupq_n_s16(-107);
    const int16x8_t c_crb = vdupq_n_s16(-21);
    const int16x8_t bias  = vdupq_n_s16(128);

    for (int row = 0; row < height; ++row) {
        const uint8_t* s = src + row * width * 4;
        uint8_t*       d = dst + row * width * 2;

        int col = 0;
        for (; col + 8 <= width; col += 8) {
            // Load 8 BGRA pixels
            uint8x8x4_t bgra = vld4_u8(s + col * 4);
            // bgra.val[0] = B, [1] = G, [2] = R, [3] = A

            int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(bgra.val[0]));
            int16x8_t g = vreinterpretq_s16_u16(vmovl_u8(bgra.val[1]));
            int16x8_t r = vreinterpretq_s16_u16(vmovl_u8(bgra.val[2]));

            // Y = (77R + 150G + 29B) >> 8
            int16x8_t yr = vshrq_n_s16(vmulq_s16(r, c_yr), 8);
            int16x8_t yg = vshrq_n_s16(vmulq_s16(g, c_yg), 8);
            int16x8_t yb = vshrq_n_s16(vmulq_s16(b, c_yb), 8);
            int16x8_t y  = vaddq_s16(vaddq_s16(yr, yg), yb);

            // Cb = 128 + (-43R - 85G + 128B) >> 8
            int16x8_t cbr = vshrq_n_s16(vmulq_s16(r, c_cbr), 8);
            int16x8_t cbg = vshrq_n_s16(vmulq_s16(g, c_cbg), 8);
            int16x8_t cbb = vshrq_n_s16(vmulq_s16(b, c_cbb), 8);
            int16x8_t cb  = vaddq_s16(bias, vaddq_s16(vaddq_s16(cbr, cbg), cbb));

            // Cr = 128 + (128R - 107G - 21B) >> 8
            int16x8_t crr2= vshrq_n_s16(vmulq_s16(r, c_crr), 8);
            int16x8_t crg = vshrq_n_s16(vmulq_s16(g, c_crg), 8);
            int16x8_t crb = vshrq_n_s16(vmulq_s16(b, c_crb), 8);
            int16x8_t cr  = vaddq_s16(bias, vaddq_s16(vaddq_s16(crr2, crg), crb));

            // Pack to uint8 with saturation
            uint8x8_t y8  = vqmovun_s16(y);
            uint8x8_t cb8 = vqmovun_s16(cb);
            uint8x8_t cr8 = vqmovun_s16(cr);

            // Horizontal average Cb/Cr for 4:2:2 subsampling
            // vpaddq adds adjacent pairs: [cb0+cb1, cb2+cb3, cb4+cb5, cb6+cb7]
            // Then shift right by 1
            uint8x8_t cb_avg = vshrn_n_u16(
                vpaddlq_u8(vcombine_u8(cb8, vdup_n_u8(0))), 1);
            uint8x8_t cr_avg = vshrn_n_u16(
                vpaddlq_u8(vcombine_u8(cr8, vdup_n_u8(0))), 1);

            // Build UYVY output: U0 Y0 V0 Y1 U1 Y2 V1 Y3 ...
            // We have 4 (U,V) pairs and 8 Y values → 16 output bytes
            uint8x8x2_t uy, vy;
            // Interleave cb_avg with y (even positions): U0 Y0 U1 Y2 U2 Y4 U3 Y6
            // Interleave cr_avg with y (odd positions):  V0 Y1 V1 Y3 V2 Y5 V3 Y7
            // Then merge to get 16 bytes
            // Simpler: use vst4 or manual interleave
            // UYVY: [U0 Y0 V0 Y1] [U1 Y2 V1 Y3] [U2 Y4 V2 Y5] [U3 Y6 V3 Y7]
            // Pack into uint8x4x4 or manually:
            uint8_t tmp[16];
            for (int i = 0; i < 4; ++i) {
                tmp[i * 4 + 0] = vget_lane_u8(cb_avg, i);
                tmp[i * 4 + 1] = vget_lane_u8(y8, i * 2);
                tmp[i * 4 + 2] = vget_lane_u8(cr_avg, i);
                tmp[i * 4 + 3] = vget_lane_u8(y8, i * 2 + 1);
            }
            memcpy(d + col * 2, tmp, 16);
        }

        // Scalar remainder
        for (; col < width; col += 2) {
            const uint8_t* p0 = s + col * 4;
            const uint8_t* p1 = p0 + 4;
            convert_2pixels_to_uyvy_scalar(
                p0[0], p0[1], p0[2], p1[0], p1[1], p1[2],
                &d[col * 2 + 0], &d[col * 2 + 1],
                &d[col * 2 + 2], &d[col * 2 + 3]);
        }
    }
}

#endif // IDHMFIS_USE_NEON

// ---------------------------------------------------------------------------
// Public API — dispatch to best available implementation
// ---------------------------------------------------------------------------
void bgra_to_uyvy(const uint8_t* src, uint8_t* dst, int width, int height)
{
#ifdef IDHMFIS_USE_SSE41
    bgra_to_uyvy_sse41(src, dst, width, height);
#elif defined(IDHMFIS_USE_NEON)
    bgra_to_uyvy_neon(src, dst, width, height);
#else
    bgra_to_uyvy_scalar(src, dst, width, height);
#endif
}

void uyvy_to_bgra(const uint8_t* src, uint8_t* dst, int width, int height)
{
    // SSE4.1 path for the inverse conversion
    // The inverse is bandwidth-bound so the scalar path is a reasonable start.
    // A SIMD inverse path uses similar lane manipulation.
#ifdef IDHMFIS_USE_SSE41
    // Process 8 pixels (4 UYVY macro-blocks) per iteration
    const __m128i bias = _mm_set1_epi16(128);
    const __m128i c_vr = _mm_set1_epi16(359);  // (359/256)*256 ≈ 1.403
    const __m128i c_ug = _mm_set1_epi16(88);
    const __m128i c_vg = _mm_set1_epi16(183);
    const __m128i c_ub = _mm_set1_epi16(454);

    for (int row = 0; row < height; ++row) {
        const uint8_t* s = src + row * width * 2;
        uint8_t*       d = dst + row * width * 4;

        int col = 0;
        for (; col + 8 <= width; col += 8) {
            // Load 16 bytes: U0 Y0 V0 Y1 | U1 Y2 V1 Y3 | U2 Y4 V2 Y5 | U3 Y6 V3 Y7
            __m128i uyvy = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s + col * 2));

            // Extract Y: bytes 1,3,5,7,9,11,13,15
            const __m128i shuf_y = _mm_set_epi8(
                -1,15,-1,13,-1,11,-1, 9,-1, 7,-1, 5,-1, 3,-1, 1);
            __m128i y = _mm_shuffle_epi8(uyvy, shuf_y);  // 8 × 16-bit Y values

            // Extract U (Cb): bytes 0,4,8,12 → replicate to 8 values (0,0,4,4,8,8,12,12)
            const __m128i shuf_u = _mm_set_epi8(
                -1,12,-1,12,-1, 8,-1, 8,-1, 4,-1, 4,-1, 0,-1, 0);
            __m128i u = _mm_shuffle_epi8(uyvy, shuf_u);
            u = _mm_sub_epi16(u, bias);

            // Extract V (Cr): bytes 2,6,10,14 → replicate
            const __m128i shuf_v = _mm_set_epi8(
                -1,14,-1,14,-1,10,-1,10,-1, 6,-1, 6,-1, 2,-1, 2);
            __m128i v = _mm_shuffle_epi8(uyvy, shuf_v);
            v = _mm_sub_epi16(v, bias);

            // R = Y + (359 * Cr) >> 8
            __m128i r16 = _mm_add_epi16(y, _mm_srai_epi16(_mm_mullo_epi16(v, c_vr), 8));
            // G = Y - (88 * Cb + 183 * Cr) >> 8
            __m128i g16 = _mm_sub_epi16(y,
                _mm_srai_epi16(_mm_add_epi16(
                    _mm_mullo_epi16(u, c_ug),
                    _mm_mullo_epi16(v, c_vg)), 8));
            // B = Y + (454 * Cb) >> 8
            __m128i b16 = _mm_add_epi16(y, _mm_srai_epi16(_mm_mullo_epi16(u, c_ub), 8));

            // Pack to uint8 with saturation
            __m128i r8 = _mm_packus_epi16(r16, r16);
            __m128i g8 = _mm_packus_epi16(g16, g16);
            __m128i b8 = _mm_packus_epi16(b16, b16);
            const __m128i alpha = _mm_set1_epi8(static_cast<char>(255u));

            // Interleave to BGRA: B G R A B G R A ...
            __m128i bg_lo = _mm_unpacklo_epi8(b8, g8);
            __m128i ra_lo = _mm_unpacklo_epi8(r8, alpha);
            __m128i bgra0 = _mm_unpacklo_epi16(bg_lo, ra_lo);
            __m128i bgra1 = _mm_unpackhi_epi16(bg_lo, ra_lo);

            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + col * 4),      bgra0);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(d + col * 4 + 16), bgra1);
        }

        // Scalar remainder
        for (; col < width; col += 2) {
            int cb = static_cast<int>(s[col * 2 + 0]) - 128;
            int y0 = static_cast<int>(s[col * 2 + 1]);
            int cr = static_cast<int>(s[col * 2 + 2]) - 128;
            int y1 = static_cast<int>(s[col * 2 + 3]);

            auto clamp_u8 = [](int v) -> uint8_t {
                return static_cast<uint8_t>(std::clamp(v, 0, 255));
            };
            auto yuv_to_bgra_p = [&](int y, uint8_t* out) {
                out[0] = clamp_u8(y + (454 * cb >> 8));
                out[1] = clamp_u8(y - ( 88 * cb >> 8) - (183 * cr >> 8));
                out[2] = clamp_u8(y + (359 * cr >> 8));
                out[3] = 255;
            };
            yuv_to_bgra_p(y0, d + col * 4);
            yuv_to_bgra_p(y1, d + (col + 1) * 4);
        }
    }
#else
    uyvy_to_bgra_scalar(src, dst, width, height);
#endif
}

} // namespace idhmfis
