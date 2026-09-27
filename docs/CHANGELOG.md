# Changelog

Notable changes to IDHMFIS. The version number is bumped by 0.01 on every internal build, so released versions are not consecutive.

---

## 5.27 — 2026-05-29

**Rendering and FX**
- Recording and playback now use the same point budget, so a recorded cue renders exactly as it was programmed.
- Movement FX that push geometry outside the scan field now blank both the exit and the re-entry segment. This removes ghost lines from Scale and similar effects.
- Pen-up travel in the keyframe editor is blanked.
- Deselecting a group now renders the same as programmer-only output.

**Multi-output performance**
- DAC output-thread priority adapts to stream count (`TIME_CRITICAL` for up to four streams, `HIGH` above that). This removes choppiness on rigs with 15 or more streams.
- NDI sends are staggered across streams.
- A shared keyframe render budget (point rate ÷ 30) cuts playback latency.

**UI**
- Timeline: pan by dragging the ruler. Events can be moved, dragged, deleted from a right-click menu, or removed with REM. Events are tracked by ID.
- SHIFT multi-select for heads and groups.
- REINIT now rebuilds routing from a snapshot taken before teardown, and has moved to the OUTPUTS panel.

**Engine**
- Fixed a startup hang: virtual camera creation is limited to 4 s, and the camera sidecar is optional.
- Autosave now marks the project dirty on edits, so it saves reliably.
- Manual GO on a cue list now honours the Loop and Jump link modes.

## 5.21 — 2026-05-28

- REINIT is now also available in the STREAMS window, where stream connections are managed.

## 5.01 — 2026-05-28

- Scale FX blanks lit points that leave the `[-1, 1]` field before clamping, which fixes edge-density artefacts.
- INCL latches content per head instead of taking the whole active-stream set at once.
- UPDT keeps the data of streams that aren't active when updating a single head.
- REINIT splits into stop-and-release followed by deferred recreation once the hardware is released.
- Switching to a stream with no saved content clears the programmer, which stops FX bleeding between streams.

## 4.31 — 2026-05-27

**Stability**
- Full audit of seven subsystems:
  - Engine: per-stream X-flip fan-out.
  - DAC: Ether Dream ping-retry and correct Helios buffer accounting.
  - Input: MIDI learn lifetime fix, MTC freshness checks, pitch-bend clamping.
  - FX: floating-point accumulator wrap in eight blocks.
- Safety: the BAM check is now position-only. Blank points are no longer skipped, because skipping them could mask a violation.
- Safety: scan-fail samples the centroid of all lit points and latches until an operator resets it.
- Fixed a data race between autosave and frame building that could crash on save. Autosave now works from a locked snapshot of engine-owned state.
- Hardening against out-of-range and missing values: DMX bounds, FFT time-of-check races, generator divide-by-zero, NaN in zones and cue lists, NDI dimension caps, timeline seek clamping, OSC bundle validation and the sACN universe range.
- The engine loop catches exceptions and recovers instead of terminating. Start/stop is serialised with a lifecycle mutex.
- Orphaned-autosave detection compares process creation time, so a reused PID isn't mistaken for a live session.

**Features**
- Live DMX monitor in the playback configuration popup.
- REINIT button for output streams.
- Show-file load no longer drops into legacy single-bus mode. Stream routing is re-applied once DACs finish stopping.

## 3.75 — 2026-05-22

- Cue reordering with the new `MoveCue` command and a MOVE mode in the cue sheet.
- Mirrored-stream X-flip is applied in every playback path.
- `DacManager` is recreated when a laser's ordinal changes.
- Timeline: external timecode no longer mass-fires events on first sync.

## 3.74 — 2026-05-22

- Groups grid with independent row and column settings.
- Auto Groups with four generation modes, including Odd/Even and Zigzag.
- Fine-grained undo in the frame editor, layered ahead of programmer undo.
- Premade shapes apply symmetry after generation.
- Output health checker with no false alarms after a show loads.

## 3.70 — 2026-05-22

- Auto groups, mirrored stream IDs stored in playback cues, a clean new-show reset, and a cue-stack BPM "set all" action.

## 1.5 — 2026-05-21

The first tracked release. Main pieces:

- **Engine:** 1 kHz show engine with MPSC command queue, double-buffered snapshot and a watchdog.
- **Outputs:** Helios, Ether Dream, LaserDock, IDN-Stream and CITP/CAEX drivers with hot-plug detection. NDI 6 output and a Windows 11 virtual camera.
- **Point optimizer:** seven stages, from path flattening to overscan clipping.
- **Programmer:** multi-head programmer with INCL/UPDT and LTP tracking. Five-state cue list with follow, wait, timecode, MIDI, OSC, DMX and audio triggers. Up to 40 playbacks.
- **FX:** 33 FX blocks, a modulation matrix and a bytecode expression engine.
- **Content:** 30 generators, plus ILDA and SVG import.
- **Show control:** Art-Net 4 (ArtPoll), sACN, MIDI with learn mode, OSC 1.1, and LTC, MTC and Art-Net timecode.
- **Safety:** beam-attenuation map, block zones, border crops, scan-fail monitor and emergency shutoff.
- **Timeline:** multi-track timeline with audio.
- **Projects:** JSON project format with a SHA-256 integrity trailer, atomic saves, autosave and crash recovery.
