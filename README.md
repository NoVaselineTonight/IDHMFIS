# IDHMFIS

**A real-time laser show controller for ILDA vector projectors, written in C++20.**

IDHMFIS drives galvo laser projectors over Helios, Ether Dream, LaserDock, IDN and CITP. It runs a 1 kHz show engine with lock-free inter-thread messaging and a console-style programmer modelled on MagicQ and grandMA workflows. The same frames can also be streamed as a GPU-rendered beam simulation over NDI.

The name stands for *I Don't Have Money For ILDA Software*. Commercial laser control suites cost more than many of the projectors they drive. IDHMFIS is an attempt to build one that holds up next to them.

![Version](https://img.shields.io/badge/version-5.27-00E5FF) ![C++20](https://img.shields.io/badge/C%2B%2B-20-blue) ![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-lightgrey) ![License](https://img.shields.io/badge/license-source--available-orange)

---

## Highlights

- **1000 Hz deterministic show engine.** Cue evaluation, FX, safety and output routing run on a dedicated `TIME_CRITICAL` thread with an absolute-deadline scheduler and a watchdog. Nothing on the hot path takes a lock: the UI talks to the engine only through an MPSC command queue and reads back a double-buffered snapshot.
- **Multi-head output routing.** Any number of lasers can be patched as independent output streams, each with its own programmer content, geometry, FX and mirroring. Recording and playback preserve per-head state, so one cue can put different content on every projector in the rig.
- **Seven-stage point optimizer.** Frames are rebuilt for galvo physics before they reach a DAC: path flattening, anchor dwell, colour-edge interpolation, blanking insertion, nearest-neighbour path ordering, density normalisation and overscan clipping. Dwell, corner angle and target PPS are all tunable.
- **Bytecode expression engine.** Any parameter can be driven by a math expression such as `sin(t*tau*bpm/60) * rms`. Expressions compile to stack-based bytecode and evaluate with zero heap allocation. They can read time, beat and bar position, BPM, audio bands and per-point position.
- **Modulation matrix.** LFOs, envelope followers, audio bands and expression outputs can be routed to any FX parameter, with signed depth per route, in the style of a modular synth.
- **Safety systems built into the frame path.** A 64×64 beam-attenuation map (BAM), per-edge border crops and block zones, a scan-fail monitor that latches until an operator resets it (following IEC 60825-1 practice), and a hard emergency shutoff that timeline events cannot override. Safety runs last, after every transform, so what it checks is exactly what the DAC receives.
- **Console integration.** Full Art-Net 4 with ArtPoll/ArtPollReply, so IDHMFIS shows up natively in grandMA3, Hog 4, Avolites, ChamSys and ETC EOS. It also supports sACN (E1.31), a 40-channel fixture profile with 16-bit parameters, GDTF export, MIDI with learn mode, OSC 1.1 with bundles, and LTC, MTC and Art-Net timecode chase.
- **GPU beam simulation.** D3D12 (Windows) and Vulkan renderers draw beams with a Gaussian profile into an HDR RGBA16F target, followed by additive blending, bloom, atmospheric haze and exposure tone-mapping. The render is published as an NDI 6 source, and on Windows 11 as a Media Foundation virtual camera.

---

## Feature overview

| Area | Capabilities |
|---|---|
| **Laser output** | Helios (USB), Ether Dream (Ethernet, full flow control), LaserDock (HID), IDN-Stream (ILDA Digital Network), CITP/CAEX, plus the CLS1 TCP/UDP streaming protocol. Hot-plug detection and per-DAC point-rate negotiation. |
| **Programmer** | Multi-head selection, groups (including auto-generated odd/even/zigzag groups), INCL/UPDT, programmer latching, LTP tracking and per-stream mirroring. |
| **Cue system** | Five-state cue-list machine (Idle → Playing → Paused → Holding → Releasing). GO, BACK, JUMP, PAUSE, FLASH and SWOP. Follow, wait, timecode, MIDI, OSC, DMX and audio triggers. Up to 40 concurrent playbacks. |
| **Timeline** | Multi-track timeline with audio playback, timecode sync (internal, LTC, MTC, Art-Net TC), wait-for-GO events and O(log n) event lookup. |
| **Content** | 30 procedural generators, ILDA import (formats 0, 1, 4 and 5), SVG import with Bézier flattening, image-to-vector import, and an in-app frame editor with symmetry tools and onion-skin. |
| **FX** | 33 composable FX blocks (geometry, colour, temporal and utility) with wet/dry mix, plus a modulation matrix and expression-driven parameters. |
| **Live performance** | Quick Show grid (10×6, 32 pages), a LivePRO XY and beat pad, colour and position palettes, macros, and a command palette (Ctrl+K). |
| **Audio** | WASAPI loopback capture, 2048-point FFT, sub/mid/high band envelopes and beat detection. Detected BPM is shown as a hint and never auto-applied, so a mistaken lock can't change timing mid-show. |
| **Project files** | Schema-versioned JSON (`.idhmfis`) that diffs cleanly in git. Saves are atomic (write to temp, then rename) and carry a SHA-256 integrity trailer. Autosave with crash recovery, and 200-step undo. |
| **Privacy** | No telemetry, analytics or licence checks. The only network traffic is the output you configure. |

---

## Architecture

```
 Art-Net / sACN ─┐
 MIDI ───────────┤                     ┌───────────────────────────────┐
 OSC ────────────┼─► lock-free queues ─►  Show Engine  (1000 Hz, RT)   │
 LTC / MTC ──────┤                     │  cue list · programmer · FX   │
 Audio (FFT) ────┘                     │  modulation · timeline        │
                                       └──────────────┬────────────────┘
        UI thread (120 fps)                           │ per-stream frames
        ▲  double-buffered snapshot                   ▼
        │                              ┌───────────────────────────────┐
        └──────────────────────────────┤  Safety  (BAM · zones · scan) │
                                       │  Point optimizer (7 stages)   │
                                       └──────────────┬────────────────┘
                                                      │ render bus
                     ┌────────────────────────────────┼─────────────────────┐
                     ▼                                ▼                     ▼
          DAC output threads               GPU rasterizer            CLS1 / CITP
   Helios · Ether Dream · LaserDock        D3D12 / Vulkan            streaming
            IDN-Stream                     → NDI · virtual cam
```

The DAC output threads are independent of the GPU path. Disabling NDI or the preview has no effect on laser timing. For thread ownership, the command protocol and subsystem invariants, see **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**.

---

## Getting started

**Requirements:** Windows 10/11 x64, Visual Studio 2022 (MSVC 17.8+), CMake 3.25+, Ninja, and vcpkg for the Vulkan headers. The NDI 6 SDK is optional.

```powershell
git clone https://github.com/NoVaselineTonight/IDHMFIS.git
cd IDHMFIS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --parallel
.\build\bin\IDHMFIS.exe
```

CMake fetches every other dependency (SDL3, Dear ImGui, nlohmann/json, RtMidi, KissFFT, the Cameron314 queues, VMA and hidapi). Full instructions, build options and the MSI installer are covered in **[docs/BUILDING.md](docs/BUILDING.md)**.

No hardware? Turn on the emulated Helios DAC under *Settings → DAC* to run the whole output pipeline without a projector.

---

## Documentation

| Document | Contents |
|---|---|
| [User Guide](docs/USER_GUIDE.md) | Operating the software, from first launch to cue stacks, FX, timeline and safety zones |
| [Architecture](docs/ARCHITECTURE.md) | Threading model, command protocol, output routing and subsystem invariants |
| [Design Decisions](docs/DECISIONS.md) | Architecture decision records and the reasoning behind them |
| [Building](docs/BUILDING.md) | Toolchain, build options, installer and hardware verification |
| [Performance](docs/PERFORMANCE.md) | Latency and throughput targets, and how to measure them |
| [DMX Channel Map](docs/DMX_CHANNEL_MAP.md) | Default 40-channel fixture profile and console patching |
| [NDI Integration](docs/NDI_INTEGRATION.md) | Using the NDI feed in Resolume, TouchDesigner, OBS, disguise and others |
| [Keyboard Shortcuts](docs/KEYBOARD_SHORTCUTS.md) | Shortcut reference (every binding can be remapped) |
| [Changelog](docs/CHANGELOG.md) | Release history |

---

## Laser safety

IDHMFIS controls Class 3B and Class 4 laser equipment, which can cause permanent eye injury, burns and fire. The software's safety features support safe operation but do not replace it. Operators are responsible for compliance with local regulations, audience-scanning rules and aviation restrictions. Use of the software is subject to the safety terms in the [EULA](EULA_Laser_Control_Software.pdf).

---

## License

IDHMFIS is **source-available**, not open source. The code is published so people can read it, evaluate it and learn from it. Installing and using the software is governed by the [End User License Agreement](EULA_Laser_Control_Software.pdf). See [LICENSE.md](LICENSE.md) for details.

Third-party libraries keep their own licenses (MIT, BSD and zlib). NDI® is a registered trademark of Vizrt NDI AB. The NDI SDK is not included and is loaded at runtime only if it is installed.

© 2026 Onni Kauppinen
