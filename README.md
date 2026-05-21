# IDHMFIS
### I Don't Have Money For ILDA Software

Professional laser show programming software with **NDI-first output**.

---

## What Is This?

IDHMFIS is a professional laser show programming tool for ILDA-style vector laser projectors. It is differentiated from every existing product (Pangolin Beyond, LaserShowGen, Showtacle, LSX, HE-Laserscan, Dynamics, Laserboy) by one architectural fact:

**The primary output is an NDI video stream.**

Not "also exports NDI." The entire render pipeline is NDI-first. The laser projector path (ILDA over USB DAC or Ethernet DAC) is a secondary consumer of the same internal vector frame buffer.

The NDI stream carries the laser content as a high-frame-rate (60+ fps), visually accurate, GPU-rendered video with:
- Configurable beam thickness
- Gaussian bloom
- Atmospheric haze density
- Exposure and film grain

This means laser operators can integrate their content into VJ rigs, broadcast switchers, Resolume, TouchDesigner, OBS, and media servers — without pointing a camera at a fog-filled room.

---

## Key Features

| Feature | Detail |
|---------|--------|
| **Primary output** | NDI 6 stream, 1080p/4K, 60–120 fps |
| **Laser output** | Helios DAC, EtherDream, IDN-Stream, LaserDock |
| **DMX input** | Art-Net 4 with full ArtPoll (shows in grandMA3, Hog 4, EOS) |
| **MIDI input** | Via RtMidi — any class-compliant device |
| **OSC input** | UDP, any address |
| **Content** | 14+ native generators, ILDA import, SVG import |
| **Audio** | WASAPI loopback, FFT, beat detection, BPM tracking |
| **UI** | ImGui docking, 120 fps, command palette, dark theme |
| **Project format** | Schema-versioned JSON, human-diffable, git-friendly |
| **Platform** | Windows 10/11 x64 (primary), macOS arm64 (secondary) |

---

## Quick Start

1. Install prerequisites (see [BUILDING.md](BUILDING.md))
2. Build: `cmake -B build && cmake --build build`
3. Run: `.\build\IDHMFIS.exe`
4. Press **Play** — a default cue starts
5. Open NDI Studio Monitor — source "IDHMFIS" appears with beam simulation
6. Connect a supported DAC for hardware output

---

## Generators

| Name | Description |
|------|-------------|
| Beams | 1–32 parallel or fan beams |
| Waves | Animated sine wave |
| Lissajous | Parametric Lissajous figure |
| Tunnel | Zoom tunnel with rotating rings |
| Text Scroller | Vector font text |
| Oscilloscope | Audio-reactive scope / spectrum |
| FFT Bars | Frequency bar display |
| Spirograph | Epitrochoid / hypotrochoid |
| Particle Field | Deterministic particle system |
| Geometric Morph | n-gon ↔ m-gon interpolation |
| Ribbon | Animated Bezier ribbon |
| Grid | Parametric grid |
| Starburst | N-ray burst with oscillation |
| Fan Sweep | Scanning fan beams |
| Cone Sweep | 3D cone projection pattern |

---

## Architecture

```
ArtNet ──┐
MIDI  ──┤  Input Layer   ──► Show Engine (1000 Hz)
OSC   ──┤  (lock-free)          │
Audio ──┘                       ▼
                           Generator Layer
                           (job-stealing pool)
                                │
                           Render Bus
                          ┌─────┴─────┐
                       DAC Out     NDI Rasterizer
                    (RT priority)  (GPU — Vulkan/D3D12)
                                        │
                                   NDI Sender ──► Network
```

No mutexes in the hot path. The UI reads a double-buffered snapshot of engine state and writes through a command queue.

---

## DMX Fixture Profile (Default 40ch)

| Ch | Parameter |
|----|-----------|
| 1–2 | Cue select (16-bit) |
| 3 | Intensity |
| 4–7 | R / G / B / W |
| 8–9 | Pan offset (16-bit) |
| 10–11 | Tilt offset (16-bit) |
| 12–13 | Pan size |
| 14–15 | Tilt size |
| 16–17 | Rotation |
| 18 | Zoom |
| 19 | Scan speed |
| 20 | Beam attack |
| 21 | Blackout |
| 22 | Strobe |
| 23–24 | Cue list select |
| 25 | Master fade |
| 26 | Beam shape morph |
| 27 | Audio reactivity gain |
| 28 | BPM divider |
| 29–40 | Reserved |

---

## Performance Targets

| Metric | Target |
|--------|--------|
| UI fps | ≥ 120 idle / ≥ 60 under load |
| ArtNet p99 latency | ≤ 8 ms |
| Cold start | ≤ 1.5 s |
| Project load (200 cues) | ≤ 500 ms |
| Memory (typical show) | ≤ 600 MB |
| NDI dropped frames | < 1/hour |
| 4-hour soak | 0 crashes, 0 point drops |

All metrics are measured automatically by `bench_harness.exe` (see PERF_REPORT.md).

---

## License

IDHMFIS is [license TBD]. Third-party libraries retain their own licenses (MIT, BSD).

NDI® is a trademark of Vizrt Group. The NDI SDK is a separate download and is not included in this repository.

---

## Contributing

See [BUILDING.md](BUILDING.md) for build instructions. All PRs must pass the benchmark harness and the soak test before merging.
