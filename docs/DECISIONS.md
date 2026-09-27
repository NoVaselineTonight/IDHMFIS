# IDHMFIS Architecture Decision Record

This document records the significant architectural choices in IDHMFIS and the reasoning behind each one. Decisions are numbered in the order they were made. A decision is revisited only with a new record that explains why.

---

## D-001 — Hardware DAC Output Is the Primary Path; NDI Is a Parallel Consumer

**Decision:** The DAC output path is primary. The engine produces one optimised frame per stream, and that frame goes to the DAC output threads first. The GPU beam rasterizer and NDI sender read the same frame from the render bus as independent consumers.

**Rationale:** A laser controller has one job it cannot fail at: keeping the galvos fed with safe, correctly timed points. Video output is valuable, and a clean network feed of the beam render is something most laser software doesn't offer, but it must never be able to delay or starve the DAC.

**Implication:** Disabling NDI, or losing the GPU, has no effect on laser output timing. The rasterizer can drop frames, but the DAC path cannot.

---

## D-002 — C++20 Native, No Electron/Web

**Decision:** Native C++20. No Electron, no CEF, no web view, no Python runtime.

**Rationale:** A 1.5-second cold start and 120 fps UI are incompatible with web-based runtimes. Professional operators need sub-millisecond responsiveness in the hot path (DMX input → rendered output). The memory ceiling (600 MB typical) is also incompatible with V8 + a full Chromium instance.

---

## D-003 — D3D12 on Windows (Primary), Vulkan on macOS (Secondary)

**Decision:** D3D12 is the Windows render backend. Vulkan is the macOS/cross-platform backend. OpenGL is explicitly forbidden.

**Rationale:** OpenGL has been deprecated on macOS since 10.14 (2018) and carries significant driver overhead on Windows. D3D12 gives us explicit GPU memory control and compute shader dispatch needed for real-time beam rasterization. Vulkan on macOS via MoltenVK provides a consistent API surface for the secondary platform. Both share the same shader logic, just different API wrappers.

**Note:** D3D11 was considered as a simpler D3D12 alternative. Rejected: D3D11 compute dispatch is more limited, and we need the full UAV pipeline for beam rasterization.

---

## D-004 — Dear ImGui (Docking Branch) for UI

**Decision:** UI is built on Dear ImGui docking branch. No Qt.

**Rationale:** Qt would require either a commercial license (€3,000+/year per developer) or LGPL compliance (which is complex with a static binary distribution). ImGui has no license cost, zero dependencies beyond the render backend, and is the de facto standard for GPU-accelerated tool UI in games and pro applications (Rider, ReShade, many game editors). The docking branch provides multi-panel layouts equivalent to what we need.

**Alternative considered:** egui (Rust). Rejected because the project is C++20 and adding a Rust dependency would complicate the build significantly. Pure ImGui also gives us more control over the custom controls (fader stacks, color wheels) that laser operators expect.

---

## D-005 — SDL3 as Window/Input Layer

**Decision:** SDL3 provides the OS window, keyboard/mouse input, and HiDPI handling for the ImGui integration.

**Rationale:** SDL3 supports Windows and macOS with a unified API, handles HiDPI scaling correctly, and has a high-quality ImGui backend (imgui_impl_sdl3). Alternative was GLFW; SDL3 chosen because it handles OS-level input (e.g. raw HID events) better and is more actively maintained for game-quality input.

---

## D-006 — Lock-Free SPSC/MPSC Queues on All Hot Paths

**Decision:** All inter-thread communication on the render bus and between input threads and the show engine uses Cameron314's readerwriterqueue (SPSC) and concurrentqueue (MPSC). No mutexes in the hot path.

**Rationale:** At 1000 Hz tick rate, a mutex contention of even 50 µs would cause a dropped tick. Lock-free queues provide nanosecond-scale handoff. The readerwriterqueue in particular achieves single-digit nanosecond dequeue latency on modern hardware.

**Exception:** The double-buffered snapshot from engine → UI uses a single mutex (acceptable because the UI thread reads it at 120 fps, not 1000 Hz, and lock contention is extremely rare).

---

## D-007 — 1000 Hz Engine Tick Rate

**Decision:** The show engine ticks at 1000 Hz (1 ms period).

**Rationale:** 
- DAC point rates are 8k–60k pps. At 60k pps, a 1 ms tick delivers 60 points, which is the minimum meaningful quantum for trajectory smoothness.
- DMX update rate is typically 40 Hz (25 ms), but we want to respond within 2 ms of a packet arriving to stay within the 8 ms p99 latency target.
- At 1000 Hz, each tick does: process input, evaluate cues, call generator, push to render bus. Each of these is O(n) in point count and sub-millisecond in practice.

---

## D-008 — Project Format: JSON with .idhmfis Extension

**Decision:** Project files are schema-versioned, pretty-printed JSON with the `.idhmfis` extension.

**Rationale:** JSON is human-readable, diffable in git, and editable in a text editor without special tools. The schema_version field enables forward/backward migration. The `.idhmfis` extension distinguishes from plain JSON and enables OS file association.

**Alternative considered:** CBOR or MessagePack (smaller, faster). Rejected: human-diffable show files are a core requirement, because they make shows reviewable and mergeable in version control. File sizes for typical shows (200 cues, parameterized generators) are < 500 KB, making binary formats unnecessary.

---

## D-009 — NDI Dynamic Loading (Not Link-Time)

**Decision:** The NDI SDK is loaded at runtime via dlopen/LoadLibrary, not linked at compile time.

**Rationale:** NDI SDK is not redistributable as a static lib; it must be installed separately. By dynamically loading it, the application ships and runs without NDI installed, gracefully disabling NDI and logging a warning. Users who install the NDI Runtime get NDI output automatically on next launch. This avoids a hard dependency that would prevent the app from starting on machines without NDI.

---

## D-010 — Helios DAC as Primary USB DAC Target

**Decision:** Helios DAC is the primary supported USB DAC. EtherDream is the primary Ethernet DAC. IDN-Stream is supported as a protocol-level option.

**Rationale:** Helios is the most popular open-hardware DAC in the community (open-source firmware, widely available). EtherDream has the best Ethernet protocol documentation. Both have public protocol specs allowing clean implementation without SDK agreements.

---

## D-011 — Audio Input: WASAPI Loopback Primary

**Decision:** WASAPI loopback capture is the primary audio input method on Windows. The analyser's device abstraction also accepts ASIO endpoints and audio files.

**Rationale:** WASAPI loopback requires no audio hardware and captures whatever is playing on the system, which is the typical laser show use case (DJ/VJ audio). ASIO requires specific hardware drivers and is more complex to integrate but offers lower latency for live audio-reactive work, so the input layer is designed to accommodate it.

---

## D-012 — ArtNet 4 Compliance for Console Compatibility

**Decision:** Full Art-Net 4 ArtPoll/ArtPollReply is implemented so IDHMFIS appears automatically in grandMA3, Hog 4, Avolites, Chamsys, and ETC EOS network views.

**Rationale:** These are the consoles laser operators are most likely to be patched into. If IDHMFIS doesn't show up in the console's "Devices" or "Network" view, operators can't patch it, and adoption dies. ArtPollReply must include correct IP, port, universe capability, and name strings.

---

## D-013 — Generator Framework: Pure Functions of (params, t)

**Decision:** All generators are pure functions: PointBuffer generate(const GeneratorParams& p, double t). No generator may hold mutable state visible between calls (exception: ILDASequence loads frames once).

**Rationale:** Pure generators are hot-reloadable (replace implementation at runtime in dev builds), deterministically testable (same inputs → same outputs), and parallelizable (the job-stealing worker pool can call multiple generators simultaneously). The time parameter t is the canonical source of animation.

---

## D-014 — Undo/Redo: Command Pattern, 200-Step Buffer, Per-Project

**Decision:** Undo/redo uses the command pattern (ICommand interface). Buffer depth is 200. The undo stack is serialized per project (survives crash recovery).

**Rationale:** 200 steps is the industry standard (matches Photoshop, Ableton). Persisting the undo stack per project means a user can undo back through an earlier session — important for complex show programming where a mistake might not be noticed until playback.

---

## D-015 — Beam Simulation: Gaussian Profile, Additive Blending, HDR

**Decision:** Beam rasterization uses a Gaussian intensity profile (not a hard-edged line), additive blending, and renders into an HDR (RGBA16F) intermediate target before tonemapping.

**Rationale:** This is how a real laser beam appears in fog or atmosphere: the intensity falls off with a Gaussian distribution from the beam center, and multiple beams add together. Additive blending ensures overlapping beams appear brighter (physically correct). HDR prevents premature clipping of bright beam intersections before exposure/tonemapping.

---

## D-016 — Cold Start Target: ≤ 1.5 Seconds

**Decision:** The app must reach a usable, interactive state within 1.5 seconds from launch on a stock i7/Ryzen 7.

**Mitigation strategies:**
- Main window is created and shown immediately (blank dark canvas)
- NDI sender initializes on a background thread
- Generator framework is initialized lazily on first cue selection
- Only the UI shell (ImGui + theme) blocks the main thread at startup
- Default project loads synchronously in < 100 ms

---

## D-017 — No Telemetry, No Analytics, No Phone-Home

**Decision:** IDHMFIS sends zero network traffic except NDI streams and DAC output. No crash reporting, no usage analytics, no license checks.

**Rationale:** Laser operators work in venues with strict network policies (festivals, corporate events). Any unexpected network traffic is unacceptable. Logs are local and user-controllable. Trust is built by being a closed box.

---

## D-018 — Color Theme: "Obsidian Laser"

**Decision:** The primary dark theme uses a palette derived from the laser experience:
- Background: #0D0F12 (void black with a blue tint)
- Accent: #00E5FF (clean cyan — the color of an argon laser at 488nm)
- Accent Warm: #FF6B35 (orange — red HeNe at 633nm warmed)
- All other surfaces are blue-black gradations

**Rationale:** The theme should evoke the environment laser operators work in (dark rooms, fog, light beams). The cyan accent is immediately associated with lasers without being garish. This palette was chosen deliberately and documented as design tokens so it can be consistently applied.

---

## D-019 — cuelist and fx libraries as source files (not separate CMake static libs)

**Decision:** `src/cuelist/` and `src/fx/` contribute their sources to the IDHMFIS executable via the PARENT_SCOPE IDHMFIS_SOURCES accumulation pattern — the same pattern used by core, render, generators, input, dac, ui, audio, and project modules.

**Rationale:** The project builds a single executable. A separate CMake static library target would require re-declaring all transitive dependencies (nlohmann_json, rtmidi, SDL3, etc.) on the new target. This complexity yields no runtime benefit since there is only one binary. The "standalone" architectural property is satisfied by enforcing no UI/engine headers inside these directories.

---

## D-020 — FullCueEntry co-exists with Project::CueListEntry for backward compatibility

**Decision:** The cuelist library introduces `FullCueEntry` (full MagicQ fields). The existing `Project::CueListEntry` (minimal: cue_id, in_time, fade_in/out, auto_next) is preserved as the serialization form. The show engine converts between the two at load time.

**Rationale:** `CueListEntry` is embedded in the project file format (serialized in `serialization.cpp`). Replacing it would invalidate all existing project files. The conversion layer preserves backward compatibility.

---

## D-021 — imgui.ini disabled; DockBuilder is canonical

**Decision:** `io.IniFilename = nullptr` — imgui.ini is not written or read.

**Rationale:** DockBuilder runs once (when `LayoutContext.dockspace_initialised == false`) and sets the correct layout. A stale imgui.ini from any prior session overrides DockBuilder and can restore a broken or outdated layout. Since LayoutContext persists as a member of Application::Impl, the correct layout is always reproducible from the DockBuilder code.

---

## D-022 — ViewportsEnable disabled until renderer supports per-viewport swapchains

**Decision:** `ImGuiConfigFlags_ViewportsEnable` is NOT set.

**Rationale:** Multi-viewport mode requires per-viewport D3D12 swap chains. The current d3d12_renderer.cpp creates a single swap chain for the main window. Enabling ViewportsEnable without per-viewport swap chains causes ImGui to create OS windows that have no D3D12 context — these render as blank windows. Implementing per-viewport swap chains is deferred until after the core show features are complete.

---

## D-023 — BPM Is User-Set Only

**Decision:** BPM is never automatically locked to audio beat detection or any heuristic. The engine exposes a SetBpm command; only explicit user interaction (DragFloat in Transport panel) or external timecode (LTC/MTC) changes BPM. Audio beat detection may run as a read-only analysis visible in the UI (e.g., a "detected: 128.0 BPM" hint label) but must not write to the engine BPM without a user action.

**Rationale:** An auto-BPM lock during a live show causes abrupt cue timing shifts that could be dangerous with high-power lasers.

