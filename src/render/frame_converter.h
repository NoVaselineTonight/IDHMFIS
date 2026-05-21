#pragma once
// frame_converter.h — CPU-side BGRA ↔ UYVY pixel format conversion.
// Uses SSE4.1 on x86_64, NEON on ARM64.  Scalar fallback for all others.

#include <cstdint>

namespace idhmfis {

// ---------------------------------------------------------------------------
// bgra_to_uyvy
// Converts a row-packed BGRA image to UYVY (YCbCr 4:2:2 packed, BT.601).
// UYVY byte order per 2-pixel macro-block: [U0 Y0 V0 Y1]
//
// width must be even (UYVY requires pairs of pixels).
// src: width * height * 4 bytes (BGRA, B at byte 0, A at byte 3)
// dst: width * height * 2 bytes
// ---------------------------------------------------------------------------
void bgra_to_uyvy(const uint8_t* __restrict src,
                        uint8_t* __restrict dst,
                  int width, int height);

// ---------------------------------------------------------------------------
// uyvy_to_bgra
// Converts a UYVY image back to BGRA.
// dst: width * height * 4 bytes (BGRA)
// src: width * height * 2 bytes (UYVY)
// ---------------------------------------------------------------------------
void uyvy_to_bgra(const uint8_t* __restrict src,
                        uint8_t* __restrict dst,
                  int width, int height);

} // namespace idhmfis
