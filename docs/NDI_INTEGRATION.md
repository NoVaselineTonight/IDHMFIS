# NDI Integration Guide

IDHMFIS outputs a real-time NDI stream of its laser preview. This guide covers setup, configuration, and integration with common NDI-capable software.

---

## What the NDI Stream Contains

The IDHMFIS NDI stream is a **GPU-rendered laser simulation**, not a screen capture or camera feed. It produces a visually accurate representation of what the laser would look like in a smoke/haze-filled room, including:

- **Beam thickness**: configurable 0.1–20 px Gaussian profile
- **Bloom**: Gaussian glow around each beam
- **Haze/Atmosphere**: volumetric density falloff
- **Exposure**: HDR-to-SDR tone-mapping control
- **Film grain**: optional sensor noise simulation

The stream updates at the configured frame rate (60 fps default) regardless of whether a physical laser DAC is connected.

---

## NDI Stream Properties

| Property | Default | Configurable |
|----------|---------|--------------|
| Source name | `IDHMFIS` | Settings → NDI → Source Name |
| Resolution | 1920×1080 | 1280×720 / 1920×1080 / 3840×2160 |
| Frame rate | 60 fps | 30 / 50 / 60 / 120 fps |
| Color format | UYVY 4:2:2 | UYVY (default) / BGRA (with alpha) |
| Audio | Stereo passthrough | Project timeline audio |
| Alpha channel | No (UYVY mode) | Yes (BGRA mode only) |

**BGRA mode** adds an alpha channel where alpha encodes beam intensity — useful for keying the laser content over a background in a compositor without needing chroma key.

---

## Prerequisites

1. Install the **NDI Runtime** (free) from https://ndi.video/tools/ — this is separate from the NDI SDK
2. All NDI-capable software on your network should discover IDHMFIS automatically via mDNS/Bonjour

If you're on Windows, after installing the NDI Runtime you do **not** need to restart IDHMFIS — it will detect the runtime dynamically on the next launch.

---

## Verifying NDI Output

1. Install **NDI Tools** from https://ndi.video/tools/
2. Launch **NDI Studio Monitor**
3. Start IDHMFIS and press **Play** (or load a project)
4. Select "IDHMFIS" from the source dropdown in NDI Studio Monitor
5. You should see the beam simulation preview within 2 seconds

If IDHMFIS doesn't appear:
- Check that both machines are on the same subnet
- Check Windows Firewall: IDHMFIS needs UDP/TCP port 5353 (mDNS) and port 5960+ (NDI video)
- In IDHMFIS, check Settings → NDI — the status dot should be green

---

## Integration by Application

### Resolume Avenue / Arena

1. **Sources → NDI** panel (or press F2 → Add Media)
2. Click the **+** button → NDI → select "IDHMFIS"
3. The IDHMFIS feed appears as a clip you can drag to any layer
4. Set layer blending to **Add** for authentic laser-over-black look

**Tip:** Use BGRA output mode (Settings → NDI → Format: BGRA) to get a proper alpha channel for keying over backgrounds.

### TouchDesigner

1. Add **NDI In TOP**
2. Set Source Name to `IDHMFIS`
3. The feed arrives as a texture — chain a Level TOP and set Extend to Black for correct additive compositing

### OBS Studio

1. Add **Source → NDI™ Source** (requires OBS-NDI plugin from https://github.com/obs-ndi/obs-ndi)
2. Source Name: `IDHMFIS`
3. Set blending to **Screen** to overlay laser content over footage

### disguise / d3

1. In Notch/Stage → Resources → NDI Source
2. Select IDHMFIS
3. Available as a video texture

### Hippotizer

1. Media Manager → NDI Sources → refresh
2. IDHMFIS appears as an input

### VDMX5

1. Media Sources → NDI Sources → IDHMFIS
2. Drag to a layer

---

## Audio Passthrough

When a project has an audio timeline (imported audio file or NDI audio input), IDHMFIS includes that audio in the NDI stream. In Resolume, this means the audio follows the video when you route IDHMFIS.

---

## Network Configuration

By default IDHMFIS binds the NDI sender to all interfaces. For controlled environments:

1. Settings → NDI → Bind Interface: select the specific network adapter
2. Firewall rules needed: allow outbound on port 5960–5979 (TCP+UDP) and inbound UDP 5353

**Bandwidth:** 1080p60 UYVY ≈ 100–200 Mbit/s (depending on NDI compression level). Use a dedicated gigabit switch between IDHMFIS and your video infrastructure. Do not share this switch with ArtNet traffic (use a separate interface or VLAN).

---

## Latency

NDI inherently adds approximately **16–50ms** of transport latency on a local LAN. This is the expected range for NDI. For tightly synchronized laser + video work, the NDI feed should not be used as the reference — use the laser output as the reference and treat NDI as a preview/recording path.

---

## Troubleshooting

**NDI source not found:**
- Install NDI Runtime
- Restart IDHMFIS
- Check that no firewall blocks port 5353 and 5960+

**NDI feed is black:**
- Press Play in IDHMFIS — the feed goes black when stopped
- Check Settings → NDI → Status indicator (should be green)

**NDI feed shows solid color instead of beam simulation:**
- In IDHMFIS, ensure the NDI rasterizer is using the GPU path: Settings → Render → Backend should not be "CPU Fallback"

**High CPU on NDI sender machine:**
- Reduce NDI resolution to 1280×720
- Reduce frame rate to 30 fps
- Disable film grain (Settings → Render → Film Grain)
