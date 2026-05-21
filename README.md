# IDHMFIS
### I Don't Have Money For ILDA Software

Professional laser show programming software. v3.52.

---

## What Is This?

IDHMFIS is a professional laser show programming tool for ILDA-style vector laser projectors. The primary output is ILDA over hardware DAC — Helios (USB), EtherDream (Ethernet), IDN-Stream, or LaserDock. That is what drives the physical laser.

The software also outputs an NDI video stream of the beam simulation (GPU-rendered, Gaussian bloom, atmospheric haze), which is useful for preview, broadcast integration, and VJ rigs. NDI is a secondary, optional output. The application builds and runs without the NDI SDK installed.

The programmer model is MagicQ-style: multi-head selection, LatchProgrammer, INCL/UPDT tracking, groups, and a full cue stack system. The FX system has two layers: a legacy FrameFx stack (geometric and color FX on the point buffer) and a modular IFxBlock system (waveform modulation on intensity and parameters). Both layers run in the hot path before DAC output.

No mutexes in the hot path. The UI reads a double-buffered snapshot of engine state and writes through a command queue.

---

## Key Features

| Feature | Detail |
|---------|--------|
| **Laser output** | Helios DAC (USB), EtherDream (Ethernet), IDN-Stream, LaserDock — primary output |
| **Programmer** | MagicQ-style multi-head selection, LatchProgrammer, INCL/UPDT, groups |
| **FX** | FrameFx stack (geometric/color) + IFxBlock system (waveform modulation) |
| **DMX input** | Art-Net 4 with full ArtPoll (shows in grandMA3, Hog 4, EOS) |
| **MIDI input** | Via RtMidi — any class-compliant device |
| **OSC input** | UDP, any address |
| **Content** | 15 native generators, ILDA import, SVG import |
| **Audio** | WASAPI loopback, FFT, beat detection, BPM tracking |
| **NDI output** | NDI 6 stream, 1080p/4K, 60–120 fps — optional, secondary |
| **UI** | ImGui docking, 120 fps, command palette, dark theme |
| **Project format** | Schema-versioned JSON, human-diffable, git-friendly |
| **Platform** | Windows 10/11 x64 (primary), macOS arm64 (secondary) |

---

## Quick Start

1. Install prerequisites (see [BUILDING.md](docs/BUILDING.md))
2. Build: `cmake -B build && cmake --build build`
3. Run: `.\build\IDHMFIS.exe`
4. Press **Play** — a default cue starts
5. Connect a supported DAC for hardware laser output
6. (Optional) Open NDI Studio Monitor — source "IDHMFIS" appears with beam simulation

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
                           Point Optimizer
                           (7-step pipeline)
                                │
                           Render Bus
                    ┌──────────┴──────────┐
                 DAC Out             NDI Rasterizer
              (RT priority)          (GPU — Vulkan/D3D12)
         Helios / EtherDream               │
         IDN-Stream / LaserDock       NDI Sender ──► Network
                                      (optional)
```

The DAC output thread runs at real-time priority (THREAD_PRIORITY_TIME_CRITICAL on Windows). NDI rasterization runs on the GPU and is independent of DAC output — disabling NDI has no effect on laser output timing.

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
| DAC output jitter | ≤ 1 ms p99 |
| Cold start | ≤ 1.5 s |
| Project load (200 cues) | ≤ 500 ms |
| Memory (typical show) | ≤ 600 MB |
| NDI dropped frames (when enabled) | < 1/hour |
| 4-hour soak | 0 crashes, 0 point drops |

All metrics are measured automatically by `bench_harness.exe` (see docs/PERF_REPORT.md).

---

## License

IDHMFIS is [license TBD]. Third-party libraries retain their own licenses (MIT, BSD).

NDI® is a trademark of Vizrt Group. The NDI SDK is an optional separate download and is not included in this repository. The application builds and runs without it.

---

## Contributing

See [docs/BUILDING.md](docs/BUILDING.md) for build instructions. All PRs must pass the benchmark harness and the soak test before merging.
