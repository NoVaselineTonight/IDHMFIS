# Architecture

This document describes how IDHMFIS is put together: which thread owns what, how state crosses thread boundaries, how frames are routed to outputs, and the invariants each subsystem depends on. Read it before changing anything in `src/core/`, `src/dac/` or `src/safety/`.

---

## 1. Source layout

| Directory | Responsibility |
|---|---|
| `src/core/` | Show engine loop, command queue, engine snapshot, render bus, lock-free queues, timing |
| `src/cuelist/` | Cue-list state machine and trigger evaluation |
| `src/fx/` | FX block framework, 33 FX blocks, modulation matrix, expression engine, strobe |
| `src/generators/` | Procedural generators, ILDA sequence playback, SVG import |
| `src/render/` | Point optimizer, D3D12 and Vulkan beam renderers, NDI sender, cue thumbnails |
| `src/dac/` | Hardware drivers (Helios, Ether Dream, LaserDock, IDN, CITP/CAEX) and `DacManager` |
| `src/safety/` | Beam-attenuation map, scan-fail monitor, safety manager |
| `src/zones/` | Output zones and border crops |
| `src/timeline/` | Timeline engine and data types |
| `src/input/` | Art-Net, sACN, MIDI, MIDI learn, OSC, LTC/MTC timecode, DMX fixture profiles |
| `src/audio/` | Audio capture, FFT analysis, timeline audio playback |
| `src/capture/` | Media Foundation virtual camera and shared-memory frame transport |
| `src/project/` | Project model, serialization, autosave, undo/redo, ILDA pool, GDTF export |
| `src/ui/` | Dear ImGui application shell, panels, widgets and theme |

**Boundary rule:** `src/core/` never includes ImGui, and `src/ui/` never instantiates engine subsystems. The command queue and the read-only snapshot are the only things that cross the UI/engine boundary.

---

## 2. Threading model

```
UI thread (120 fps)                     Engine thread (1000 Hz, TIME_CRITICAL)
───────────────────                     ──────────────────────────────────────
ui_app.cpp / layout.cpp                 show_engine.cpp
  │                                       │
  │  engine->push_command(cmd)            │  engine_loop()
  └─────────────────────────────────────► │    process_commands()   ← MpscQueue<EngineCommand>
                                          │    evaluate_cues()
  engine->snapshot()  ◄────────────────── │    build_frame()
  (double-buffered, non-blocking)         │    → safety → optimizer → render bus

DAC output threads (one per device)     Input threads
───────────────────────────────────     ─────────────
Pull frames from the render bus;        Art-Net, sACN, MIDI, OSC, timecode.
TIME_CRITICAL up to four streams,       DMX goes through a dedicated
HIGH above that.                        MpscQueue<pair<int, DmxUniverse>>.
```

- **UI → engine:** only through `ShowEngine::push_command(EngineCommand)`. The UI never touches `ShowEngine` members directly.
- **Engine → UI:** through a double-buffered `EngineSnapshot`. The UI treats it as read-only.
- **The 1 kHz loop never blocks.** No mutexes are taken on the per-tick path. The engine uses an absolute-deadline scheduler, and a watchdog thread monitors a per-tick heartbeat and can restart a stalled engine.
- The one exception is `project_access_mtx_`, which guards engine-owned project fields for the autosave snapshot (see §9). It is never held across a tick.

---

## 3. Command protocol

Every UI action is an `EngineCommand`: a `std::variant` of more than 100 command structs declared in `src/core/show_engine.h`. They cover transport, cues, playbacks, programmer, FX, output patching, safety, timeline and MIDI bindings.

Commands are dispatched with `std::visit` and `if constexpr`:

```cpp
std::visit([&](auto&& cmd) {
    using T = std::decay_t<decltype(cmd)>;
    if constexpr (std::is_same_v<T, cmd::Play>) {
        // ...
    } else if constexpr (std::is_same_v<T, cmd::Stop>) {
        // ...
    }
}, command);
```

`holds_alternative` / `std::get` chains are not used. The visitor keeps dispatch exhaustive and branch-free at runtime.

**Adding a command takes three changes:**

1. Declare `struct cmd::YourCommand { ... };` in `show_engine.h`.
2. Add it to the `EngineCommand` variant.
3. Add the handler in `ShowEngine::process_commands()`.

---

## 4. Frame pipeline

For each tick and each active output stream:

1. **Cue evaluation.** The cue list and playbacks resolve which content is live, with LTP tracking across playbacks.
2. **Generation.** Generators are pure functions of `(params, t)` (see D-013). The ILDA sequence player is the only generator with retained state.
3. **FX.** The FX stack runs in order with wet/dry mix. The modulation matrix writes each parameter's modulation value before the blocks run.
4. **Per-stream transforms.** Mirroring (X-flip), zones and border crops.
5. **Safety.** BAM check, block zones, scan-fail monitor and the emergency shutoff gate.
6. **Preview capture.** The preview is captured *here*, after transforms and safety, so the on-screen preview shows exactly what the DAC receives.
7. **Point optimizer.** Seven stages: flatten → anchor dwell → colour-edge interpolation → blanking → path ordering → density normalisation → overscan clip.
8. **Render bus.** The frame is handed to DAC output threads, the GPU rasterizer and any streaming sidecars.

---

## 5. Output routing

```
Multi-output mode (output_streams_ non-empty):
  SetOutputPatch{cfgs}           → output_streams_ rebuilt
  SetActiveStreams{ids}          → which outputs receive programmer content
  SetMirroredStreams{ids}        → X-flip for the selected stream IDs
  SetStreamTypeEnabled{kind, on} → top-level gate per output kind (DAC / NDI / Art-Net / IDN)

Legacy mode (output_streams_ empty):
  All frames → main bus

Bus slots:
  slot 0  → main bus
  slot 1+ → extra_laser_buses_[n - 1]
```

Invariants:

- `SetOutputPatch` must be followed by `SetActiveStreams` when patching laser outputs. Otherwise programmer content reaches every output.
- An empty `active_stream_ids_` means legacy single-bus mode, not "nothing selected".
- `DacManager::ordinal()` is the sequential patch index (0, 1, 2…), not `cfg.id`. CITP ports are assigned by ordinal.
- The DAC output thread holds a `shared_ptr` to the active device while sending, so a hot-unplug detected by the scan thread cannot free a device mid-write.

---

## 6. Safety subsystem

Safety runs after every other transform and wins over everything else in the chain.

**Emergency shutoff**
- The PANIC button sends `cmd::SetEmergencyShutoff{true}`.
- `Ctrl+Alt+Enter` can only *release* the shutoff. No keyboard shortcut can activate output.
- Releasing the shutoff also sends `SetOutputEnable{true}`.
- Timeline events can never re-enable output. While the shutoff is latched, timeline dispatch is halted.

**Beam-attenuation map (BAM)**
- A 64×64 grid painted by the operator (`src/safety/bam.*`).
- The check is purely positional. It deliberately ignores point colour, so a blanked-but-positioned point is still evaluated.
- The enabled flag is atomic and safe to read from any thread.

**Scan-fail monitor**
- Tracks the centroid of all lit points per frame. If the beam stays still longer than the threshold, output latches off.
- The latch clears only on an explicit `cmd::ResetScanFail` from the operator, following IEC 60825-1 practice. Disabling the monitor does not clear an active latch.

---

## 7. Cue list

`src/cuelist/` implements a five-state machine: `Idle → Playing → Paused → Holding → Releasing`.

- Operations: GO, BACK, JUMP, PAUSE, STOP, FLASH, SWOP.
- Triggers: Halt, Follow, Wait, Timecode, MIDI, OSC, DMX and Audio.
- LTP tracking across up to 40 concurrent playbacks (`kMaxPlaybacks`).
- `FullCueEntry` holds the full cue model. `Project::CueListEntry` is the stable serialized form, and the two are converted at load time (D-020).
- Navigation uses the `CueList` API (`entry_count()`, `entry_at()`), which is the source of truth after insert, delete or move.

---

## 8. Timeline

| File | Role |
|---|---|
| `src/timeline/timeline_engine.*` | Playback, event collection, timecode chase |
| `src/timeline/timeline_types.h` | Data model (schema 2.8.0) |
| `src/audio/timeline_audio_player.*` | Sample-accurate audio playback (miniaudio) |
| `src/ui/panel_timelines.cpp`, `panel_timeline_view.cpp` | Transport and track/event editor |

- Event lookup uses `std::lower_bound` over sorted events, so it costs O(log n) per tick.
- Seeks are clamped to `[0, length]`.
- When the timecode signal is invalid, the source reports `Disabled` rather than free-running.
- On the first sync to an external timecode source, the engine suppresses a mass-fire of every event between zero and the current position.

---

## 9. Project persistence

- **Format:** schema-versioned, pretty-printed JSON (`.idhmfis`), handled by nlohmann/json.
- **Integrity:** a SHA-256 trailer is appended on save (BCrypt on Windows) and checked on load. A mismatch shows a warning instead of silently loading. Files without a trailer still load.
- **Atomic writes:** each save writes to a temporary file and then renames it.
- **Autosave:** writes a PID-suffixed temporary project. On startup, orphaned autosaves are found by checking the owning process's creation time (so a reused PID isn't mistaken for a live session) and offered for recovery.
- **Thread safety:** `snapshot_project_for_save()` copies engine-owned fields (cue list, playback cue lists, timelines) under `project_access_mtx_`. The autosave path never writes those fields itself.

---

## 10. Outputs other than DACs

| Output | Notes |
|---|---|
| NDI | NDI 6 (with a v5 fallback), loaded at runtime. UYVY or BGRA. Frame size capped at 8192². Sends are staggered when many streams are active. |
| Virtual camera | A Media Foundation `IMFMediaSource` in `IDHMFISCapture.dll` (Windows 11+), fed through shared memory. Creation is time-boxed, so an unavailable camera stack can't stall startup. |
| CITP / CAEX | A virtual DAC for media-server and visualiser integration. |
| CLS1 | A streaming protocol over a TCP server (port 7256) and UDP broadcast, for external receivers. |

---

## 11. Code standards

- **C++20, MSVC `/W4 /WX /permissive-`.** Warnings are errors. Unused parameters are commented out (`int /*name*/`) rather than suppressed.
- **No naked `new`.** Ownership goes through smart pointers.
- **Dear ImGui (docking branch) on SDL3 + D3D12.** `imgui.ini` is disabled, and the DockBuilder layout in `layout.cpp` is the canonical layout (D-021). Multi-viewport is off until per-viewport swap chains exist (D-022).
- **Stable ImGui window names.** DockBuilder targets them by name.
- `.clang-format` and `.clang-tidy` live at the repo root. CI builds with warnings as errors.

---

## 12. Where to make common changes

| Task | Primary file(s) | Also touch |
|---|---|---|
| New engine command | `src/core/show_engine.h` | `src/core/show_engine.cpp` |
| New FX block | `src/fx/blocks/block_*.h/.cpp` | `src/fx/fx_registry.*` |
| New generator | `src/generators/gen_*.cpp` | `src/generators/generator_registry.*` |
| Transport / cue playback | `src/core/show_engine.cpp` | `src/cuelist/cuelist.*` |
| Output routing / patching | `src/core/show_engine.cpp` | `src/ui/panel_streams.cpp`, `src/ui/panel_patch.cpp` |
| Safety | `src/safety/*` | `src/core/show_engine.cpp` |
| DAC behaviour | `src/dac/dac_manager.cpp` | the specific `*_dac.cpp` |
| Save / load | `src/project/serialization.cpp`, `src/project/project.cpp` | `src/ui/ui_app.cpp` |
| Timecode | `src/input/timecode.cpp` | `src/timeline/timeline_engine.cpp` |

New `.cpp` files in existing directories are picked up automatically by the glob in each module's `CMakeLists.txt`. New directories need their own `CMakeLists.txt`.

The version string lives only in `src/version.h`. Every display site reads the `IDHMFIS_VERSION` macro.
