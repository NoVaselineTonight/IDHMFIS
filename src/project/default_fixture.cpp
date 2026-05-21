// default_fixture.cpp — Canonical 40-channel IDHMFIS DMX fixture profile.
//
// Channel layout (1-based offsets within the profile address block):
//
//  Ch  1- 2 : Cue select coarse+fine  (16-bit, 0-65535 → cue index)
//  Ch  3    : Intensity               (0-255 → 0.0-1.0)
//  Ch  4    : Red                     (0-255)
//  Ch  5    : Green                   (0-255)
//  Ch  6    : Blue                    (0-255)
//  Ch  7    : White                   (virtual; added to all channels equally)
//  Ch  8- 9 : Pan offset              (16-bit; 32768 = center)
//  Ch 10-11 : Tilt offset             (16-bit; 32768 = center)
//  Ch 12-13 : Pan size                (16-bit)
//  Ch 14-15 : Tilt size               (16-bit)
//  Ch 16-17 : Rotation                (16-bit; 0=0°, 65535=360°)
//  Ch 18    : Zoom                    (0=narrow/0.0, 255=wide/2.0)
//  Ch 19    : Scan speed              (0-255 → 0.0-1.0)
//  Ch 20    : Beam attack             (0-255 → 0.0-1.0)
//  Ch 21    : Blackout                (128+ = blackout)
//  Ch 22    : Strobe                  (0=off; 1-127=slow→fast; 128-255=random)
//  Ch 23-24 : Cue list select         (16-bit)
//  Ch 25    : Master fade             (0-255 → 0.0-1.0)
//  Ch 26    : Beam shape morph        (0-255 → 0.0-1.0 → param_c)
//  Ch 27    : Audio reactivity gain   (0-255 → 0.0-2.0 → param_b)
//  Ch 28    : BPM divider             (0=1/1; 64=1/2; 128=1/4; 192=1/8)
//  Ch 29-40 : Reserved (future use)
//
// Multi-byte 16-bit values: coarse byte is MSB (higher DMX address = lower
// significance), matching the convention used by most moving-head fixtures.
// The fixture receiver logic reconstructs (MSB<<8 | LSB) / 65535.0 to get
// a normalised float.
//
// param_name conventions for special channels:
//   "cue_select_msb" / "cue_select_lsb"   — handled by the engine, not params
//   "cue_list_msb"  / "cue_list_lsb"      — handled by the engine
//   "blackout"                             — maps to params.blanked
//   "strobe"                               — maps to params.param_a
//   "scan_speed"                           — maps to params.speed (scale 0-2)
//   "beam_attack"                          — maps to params.density
//   "beam_morph"                           — maps to params.param_c
//   "audio_gain"                           — maps to params.param_b (scale 0-2)
//   "bpm_divider"                          — maps to params.param_a (shared with strobe
//                                             via engine priority)
//   "master_fade"                          — maps to params.intensity
//   "pan_msb" / "pan_lsb"                 — maps to params.pan (-1..1)
//   "tilt_msb" / "tilt_lsb"              — maps to params.tilt
//   "pan_size_msb" / "pan_size_lsb"      — maps to params.scale
//   "tilt_size_msb" / "tilt_size_lsb"   — internal scale Y (not in GeneratorParams)
//   "rotation_msb" / "rotation_lsb"      — maps to params.rotation (0 .. 2π)
//   "zoom"                                — maps to params.zoom (0..2)
//   "color_r" / "color_g" / "color_b" / "color_w"  — map to color_a channels

#include "project.h"

namespace idhmfis {

DmxFixtureProfile DmxFixtureProfile::make_default_40ch() {
    DmxFixtureProfile p;
    p.name = "IDHMFIS Default 40ch";

    // Helper lambda for brevity
    auto ch = [&](int offset, std::string name, float smin, float smax) {
        p.channels.push_back({ offset, std::move(name), smin, smax });
    };

    // Ch 1-2: Cue select (16-bit)
    ch( 1, "cue_select_msb",  0.f,  255.f);
    ch( 2, "cue_select_lsb",  0.f,  255.f);

    // Ch 3: Intensity → intensity (0..1)
    ch( 3, "intensity",        0.f,    1.f);

    // Ch 4-7: RGBW colour
    ch( 4, "color_r",          0.f,    1.f);
    ch( 5, "color_g",          0.f,    1.f);
    ch( 6, "color_b",          0.f,    1.f);
    ch( 7, "color_w",          0.f,    1.f);   // white adds equally to R/G/B

    // Ch 8-9: Pan offset (16-bit; 32768 = 0.0, 0 = -1.0, 65535 ≈ +1.0)
    ch( 8, "pan_msb",         -1.f,    1.f);
    ch( 9, "pan_lsb",         -1.f,    1.f);

    // Ch 10-11: Tilt offset (16-bit; same mapping as pan)
    ch(10, "tilt_msb",        -1.f,    1.f);
    ch(11, "tilt_lsb",        -1.f,    1.f);

    // Ch 12-13: Pan size (16-bit; 0=no span, 65535=full span)
    ch(12, "pan_size_msb",     0.f,    2.f);
    ch(13, "pan_size_lsb",     0.f,    2.f);

    // Ch 14-15: Tilt size (16-bit)
    ch(14, "tilt_size_msb",    0.f,    2.f);
    ch(15, "tilt_size_lsb",    0.f,    2.f);

    // Ch 16-17: Rotation (16-bit; 0=0°, 65535≈360° i.e. 2π radians)
    ch(16, "rotation_msb",     0.f,    6.283185f);  // 2π
    ch(17, "rotation_lsb",     0.f,    6.283185f);

    // Ch 18: Zoom (0=narrow=0.0, 255=wide=2.0)
    ch(18, "zoom",             0.f,    2.f);

    // Ch 19: Scan speed (maps to speed; 0=0.0, 255=2.0 so 128≈1× normal)
    ch(19, "scan_speed",       0.f,    2.f);

    // Ch 20: Beam attack → density
    ch(20, "beam_attack",      0.f,    1.f);

    // Ch 21: Blackout (threshold 128; mapped as boolean)
    ch(21, "blackout",         0.f,    1.f);

    // Ch 22: Strobe → param_a (0=off, 1-127=0-1 speed, 128-255=random flag)
    ch(22, "strobe",           0.f,    1.f);

    // Ch 23-24: Cue list select (16-bit)
    ch(23, "cue_list_msb",     0.f,  255.f);
    ch(24, "cue_list_lsb",     0.f,  255.f);

    // Ch 25: Master fade → intensity (overrides ch3 if both are active;
    //         the engine applies it after intensity so it acts as a dimmer)
    ch(25, "master_fade",      0.f,    1.f);

    // Ch 26: Beam shape morph → param_c
    ch(26, "beam_morph",       0.f,    1.f);

    // Ch 27: Audio reactivity gain → param_b (0=0.0, 128=1.0, 255=2.0)
    ch(27, "audio_gain",       0.f,    2.f);

    // Ch 28: BPM divider
    //   0   → 1/1 (value 0.0)
    //   64  → 1/2 (value 0.25)
    //   128 → 1/4 (value 0.5)
    //   192 → 1/8 (value 0.75)
    //   255 → 1/16 (value 1.0)
    ch(28, "bpm_divider",      0.f,    1.f);

    // Ch 29-40: Reserved for future use
    for (int i = 29; i <= 40; ++i) {
        ch(i, "reserved_" + std::to_string(i), 0.f, 1.f);
    }

    return p;
}

} // namespace idhmfis
