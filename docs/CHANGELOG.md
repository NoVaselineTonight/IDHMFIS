# Changelog

All notable changes to IDHMFIS are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).
Versioning follows [Semantic Versioning](https://semver.org/).

---

## [Unreleased]

## [0.1.0] — 2026-05-13

### Added

**Core Architecture**
- Lock-free SPSC/MPSC queue infrastructure (readerwriterqueue, concurrentqueue)
- 1000 Hz show engine tick loop with watchdog thread and stall recovery
- Double-buffered engine → UI state snapshot (zero-lock reads from UI thread)
- Command queue for thread-safe UI → engine communication

**Render Pipeline**
- D3D12 GPU-accelerated vector rasterizer (Windows primary)
- Vulkan GPU rasterizer (macOS / cross-platform secondary)
- Gaussian beam profile simulation (configurable sigma, additive blending)
- HDR (RGBA16F) intermediate render target for physically correct beam overlap
- Bloom post-processing (separable Gaussian blur)
- Atmospheric haze density simulation
- Exposure tone-mapping with film grain option
- HLSL compute shader: `beam_raster.hlsl`, `post_process.hlsl`
- GLSL shaders: `beam_vert.glsl`, `beam_frag.glsl`, `post_vert.glsl`, `post_frag.glsl`

**NDI Output**
- NDI 6 SDK integration (dynamic load — app runs without NDI installed)
- NDI 5 fallback
- Source name: "IDHMFIS" (configurable)
- UYVY 4:2:2 default; BGRA with alpha channel available
- 1080p/4K, 30/50/60/120 fps configurable
- Audio passthrough from project timeline
- Zero NDI frame drops target: < 1/hour at 1080p60

**DAC Output**
- Helios DAC driver (USB, dynamically loaded libusb-1.0)
- EtherDream driver (Ethernet TCP/UDP, full protocol with flow control)
- IDN-Stream sender (Ethernet, ILDA 2023 protocol)
- LaserDock driver (USB HID, dynamically loaded)
- Hot-plug detection with automatic DAC promotion
- Real-time priority output thread (THREAD_PRIORITY_TIME_CRITICAL on Windows)
- P99 latency measurement (rolling 128-sample stats)
- Soft keep-alive (repeat last frame on DAC idle to prevent safety timeout)
- Emulated Helios DAC for testing without hardware

**ArtNet / DMX**
- Art-Net 4 listener (UDP 6454)
- Full ArtPoll/ArtPollReply (shows in grandMA3, Hog 4, Avolites, Chamsys, EOS)
- 15-bit universe addressing (Net × 256 + SubUni)
- ArtTimeCode parsing
- sACN E1.31 listener (multicast, full PDU stack)
- Default 40-channel fixture profile (cue select, intensity, RGBW, pan/tilt, zoom, strobe, etc.)
- Fixture profile editor with JSON export/import and GDTF import

**MIDI**
- RtMidi integration
- NoteOn → activate cue, NoteOff → deactivate cue
- CC7/CC11 → master intensity, CC64 → blackout toggle
- Pitch bend → intensity modulation

**OSC**
- UDP OSC server (port 8000 default)
- Full OSC 1.0/1.1 packet parser (types: i, f, s, b, T, F, N, I, h, d, t)
- Bundle support with recursive parsing
- Control paths: /idhmfis/play, /stop, /pause, /intensity, /bpm, /cue/activate, etc.

**Audio Analysis**
- WASAPI loopback capture (Windows primary)
- KissFFT 2048-point FFT with Hann windowing
- Beat detection and BPM tracking (autocorrelation-based)
- Sub/mid/high band envelopes with configurable decay
- Audio-reactive oscilloscope, FFT bar generator inputs

**Content Generators (15 built-in)**
- Beams (1–32, parallel or radial)
- Waves (animated sine wave)
- Lissajous (parametric, animating)
- Tunnel (concentric zoom rings)
- Text Scroller (Hershey simplex vector font)
- Oscilloscope (audio-reactive waveform/spectrum)
- FFT Bars (frequency analyzer)
- Spirograph (epitrochoid/hypotrochoid)
- Particle Field (deterministic time-based)
- Geometric Morph (n-gon ↔ m-gon interpolation)
- Ribbon (animated Bezier ribbon)
- Grid (parametric horizontal/vertical lines)
- Starburst (N-ray with per-ray oscillation)
- Fan Sweep (oscillating fan beams)
- Cone Sweep (3D cone projection)

**Content Import**
- ILDA file import (.ild): formats 0, 1, 4, 5 (indexed and true-color, 2D and 3D)
- SVG import → laser vector: M, L, H, V, C, Q, A, Z path commands; bezier flattening; nearest-neighbour path ordering; anchor repeats; blanking insertion

**Project Model**
- Schema-versioned JSON project format (.idhmfis extension)
- Human-diffable, git-friendly (pretty-printed, deterministic)
- 200-step undo/redo with command pattern (persisted per-project)
- Autosave every 60 seconds to %APPDATA%\IDHMFIS\autosave\
- Crash recovery on startup (detects stale autosave)
- Atomic file save (write-to-temp → rename; no corrupt projects on crash)
- Recent files list (up to 20 entries)

**Per-cue Automation**
- Keyframe tracks for every GeneratorParams field
- Linear, Step, and Cubic Bezier interpolation
- DMX channel mapping per parameter (16-bit aware)
- MIDI CC mapping per parameter (with learn mode)
- OSC path mapping per parameter
- Audio analysis mapping: rms, bpm, sub, mid, high, fft:<bin>

**UI**
- Dear ImGui docking branch with SDL3 + D3D12 backend (Windows)
- SDL3 + Vulkan backend (macOS)
- 120 fps target idle; 60 fps minimum under load
- "Obsidian Laser" dark theme (design tokens, documented color palette)
- Light theme and high-contrast theme
- Dockable panel layout: Transport / Cue Library / Laser Preview / Inspector / Timeline+DMX Monitor
- Command palette (Ctrl+K) with fuzzy search across actions, cues, parameters, recent files
- Custom widgets: FaderStack, XYPad, ColorWheel, TimelineRuler, KeyframeTrack, NDIStatusDot, BeamThicknessSlider, LatencyMeter
- Full keyboard shortcut system (25+ shortcuts, all rebindable)

**Infrastructure**
- CMake 3.25+ build system with FetchContent for all third-party deps
- vcpkg for Vulkan SDK
- Compiler warnings as errors (MSVC /W4 /WX, Clang -Wall -Wextra -Werror)
- .clang-tidy with modernize/readability/performance/bugprone checks
- GitHub Actions CI (Windows, macOS, clang-tidy jobs)
- WiX 4 MSI installer script
- Performance benchmark harness (PERF_REPORT.md)
- 4-hour accelerated soak test (SOAK_REPORT.md)

---

## [Future Roadmap]

- macOS-native D3D12 alternative (currently Vulkan via MoltenVK)
- WebGPU backend for browser preview
- GDTF fixture library
- Multi-projector output (multiple DACs simultaneously)
- Timeline sync to SMPTE/LTC
- Capture from physical camera for overlay/comparison
- Plugin API for custom generators
- OSC discovery (Bonjour)
