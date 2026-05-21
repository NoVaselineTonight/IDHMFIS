# IDHMFIS DMX Channel Map

Default fixture profile: **IDHMFIS 40ch v1.0**

## Channel List

| Ch | Ch (Hex) | Parameter | Range | Notes |
|----|----------|-----------|-------|-------|
| 1 | 0x01 | Cue Select (Coarse) | 0–255 | MSB of 16-bit cue index |
| 2 | 0x02 | Cue Select (Fine) | 0–255 | LSB. Value = (Ch1 × 256 + Ch2) → cue index |
| 3 | 0x03 | Intensity | 0–255 | 0 = off, 255 = full |
| 4 | 0x04 | Red | 0–255 | Tints output color R channel |
| 5 | 0x05 | Green | 0–255 | Tints output color G channel |
| 6 | 0x06 | Blue | 0–255 | Tints output color B channel |
| 7 | 0x07 | White (Additive) | 0–255 | Adds white to all colors |
| 8 | 0x08 | Pan Offset (Coarse) | 0–255 | MSB of 16-bit pan |
| 9 | 0x09 | Pan Offset (Fine) | 0–255 | LSB. 32768 = center |
| 10 | 0x0A | Tilt Offset (Coarse) | 0–255 | MSB of 16-bit tilt |
| 11 | 0x0B | Tilt Offset (Fine) | 0–255 | LSB. 32768 = center |
| 12 | 0x0C | Pan Size (Coarse) | 0–255 | MSB. 16-bit: 65535 = full field |
| 13 | 0x0D | Pan Size (Fine) | 0–255 | LSB |
| 14 | 0x0E | Tilt Size (Coarse) | 0–255 | MSB. 16-bit: 65535 = full field |
| 15 | 0x0F | Tilt Size (Fine) | 0–255 | LSB |
| 16 | 0x10 | Rotation (Coarse) | 0–255 | MSB. 65535 = 360° rotation |
| 17 | 0x11 | Rotation (Fine) | 0–255 | LSB |
| 18 | 0x12 | Zoom | 0–255 | 0 = narrow (0.1×), 128 = 1×, 255 = wide (4×) |
| 19 | 0x13 | Scan Speed | 0–255 | 0 = slow, 255 = max; reduces point rate |
| 20 | 0x14 | Beam Attack | 0–255 | Attack time 0=instant, 255=2s |
| 21 | 0x15 | Blackout | 0–255 | 0–127 = normal, 128–255 = hard blackout |
| 22 | 0x16 | Strobe | 0–255 | 0 = off, 1–127 = 0.5–30 Hz, 128–255 = random |
| 23 | 0x17 | Cue List Select (MSB) | 0–255 | Select active cue list (MSB) |
| 24 | 0x18 | Cue List Select (LSB) | 0–255 | Select active cue list (LSB) |
| 25 | 0x19 | Master Fade | 0–255 | 0 = instant, 255 = 10s cross-fade on cue change |
| 26 | 0x1A | Beam Shape Morph | 0–255 | 0 = source shape, 255 = target shape (gen-specific) |
| 27 | 0x1B | Audio Reactivity Gain | 0–255 | 128 = normal (1×), 0 = off, 255 = max (4×) |
| 28 | 0x1C | BPM Divider | 0–255 | 0=1/1, 64=1/2, 128=1/4, 192=1/8, 255=1/16 |
| 29–40 | 0x1D–0x28 | Reserved | 0–255 | For future use; currently ignored |

---

## Addressing

IDHMFIS listens on **Art-Net 4**, UDP port **6454**, on all interfaces by default.

Configure in Settings → ArtNet:
- **Universe**: 0–32767 (Art-Net 4 full range, default 0.0.0)
- **Subnet**: 0–15 (default 0)
- **Net**: 0–127 (default 0)

The 15-bit universe number = `(Net × 256) | (Subnet × 16) | Universe`.

---

## 16-Bit Parameters

Parameters using two consecutive channels (marked MSB + Fine above) are 16-bit.
Value = `(MSBch × 256) + LSBch`.

| Parameter | Center | Range |
|-----------|--------|-------|
| Pan Offset | 32768 (Ch8=128, Ch9=0) | ±100% of field |
| Tilt Offset | 32768 (Ch10=128, Ch11=0) | ±100% of field |
| Pan Size | 65535 = full field | 0=zero, 65535=100% |
| Tilt Size | 65535 = full field | 0=zero, 65535=100% |
| Rotation | 0=0°, 65535=360° | Continuous wrap |
| Cue Select | — | 0=first, 65535=last |

---

## Cue Selection

Cue Select (Ch1+Ch2 combined 16-bit value) maps to a cue index:
- **Value 0**: no change / hold current cue
- **Value 1–N**: activate cue at 1-based index
- **Value 65535**: stop/blackout

The transition uses the Master Fade time (Ch25) or the cue's own fade-in time, whichever is longer.

---

## Console Setup Examples

### grandMA3

1. Patch IDHMFIS in MA3: **Add Fixtures → Import GDTF** (export from IDHMFIS Settings → GDTF Export), or manually patch as **Generic RGB 40ch**.
2. Set Universe to match IDHMFIS setting.
3. In Network → Art-Net: IDHMFIS should appear automatically (ArtPoll response) — no manual IP entry needed.

### Avolites Titan

1. **Patch → Auto Patch** → search for "IDHMFIS" in personality library, or manually add as 40-channel fixture.
2. Set DMX address and universe.
3. IDHMFIS responds to ArtPoll and appears in Titan's network devices.

### ETC EOS

1. **Patch → Fixture Type → Add** — import the `.xml` personality from IDHMFIS Settings.
2. Or: add as a generic 40-channel dimmer fixture.
3. Set universe to match IDHMFIS.

---

## Custom Fixture Profiles

In IDHMFIS, open **Settings → DMX → Fixture Profile Editor** to:
- Remap any channel to any generator parameter
- Change the scale range (e.g. restrict intensity to 20–100%)
- Enable/disable 16-bit pairs
- Export profiles as JSON for sharing
