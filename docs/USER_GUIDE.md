# IDHMFIS User Guide

**Version 2.6.1** — Professional laser show programming with NDI-first output.

---

## What's New in v2.6.1

| Feature | Description |
|---------|-------------|
| Safety EULA gate | First-launch safety acknowledgment, persisted to `%APPDATA%\IDHMFIS\eula_accepted` — shown once per machine |
| Safety Blackout Zones | Border crops (per-edge with tilt angle) and rectangular block zones, enforced in the engine at the point buffer level before DAC output |
| Safety zone overlay | Blackout zone boundaries rendered as overlays in the 2D Frame Editor preview |
| FX CrossFade | `crossfade` parameter on both GlobalFxEntry and FrameFxEntry (0 = snap, 1 = smooth low-pass) — prevents stepping artifacts when editing FX live |
| FX custom color pickers | Color A / Color B pickers for all color FX types (Col2, Col3, ColFlick, ColorCycle, ColorPulse, RainbowTrail) via `use_custom_colors` toggle |
| FX speed display | FX rate shown as MM:SS duration equivalent with BPM equivalent in tooltip |
| FX Width gate fix | Width gate (duty-cycle) now correctly gates the count of objects ON rather than a time window; Sync direction behavior corrected |
| Point optimizer: double-anchor fix | `step_anchor` now runs twice — once before and once after `step_path_ordering` — resolving wavy-line artifacts at reordered segment starts |
| StackFanOut FX block | New FX block: distributes phase offset across objects in a stack-fan pattern |
| Saw FX block | New GlobalFxType: sawtooth wave shape for position/intensity FX |
| Group Scale tool | Bounding-box resize of all selected objects in the Frame Editor simultaneously |
| Premade Shapes tool | Mathematical function curve placement (Lissajous, spirograph, polygon, etc.) directly in the Frame Editor |
| EULA persistence | Acceptance state written to `%APPDATA%` and never re-prompted on the same machine |

---

## First Launch

On first launch, IDHMFIS:
1. Presents the **Safety Acknowledgment** dialog. You must accept before any laser output is enabled. This dialog is shown once per machine; acceptance is persisted to `%APPDATA%\IDHMFIS\eula_accepted`.
2. Creates a default project with a "Beams" cue already loaded.
3. Starts an NDI source named "IDHMFIS" on your local network.
4. Opens an Art-Net listener on all interfaces, Universe 0.

**The UI layout:**

```
┌─────────────────────────────────────────────────────────────┐
│  Menu Bar                                                    │
├─────────────────────────────────────────────────────────────┤
│  Transport Bar (Play / Stop / BPM / Master Intensity)        │
├──────────────┬──────────────────────────┬───────────────────┤
│              │                          │                   │
│ Cue Library  │    Laser Preview         │   Inspector       │
│ (left)       │    (center, NDI)         │   (right)         │
│              │                          │                   │
├──────────────┴──────────────────────────┴───────────────────┤
│  Timeline + DMX Activity Monitor (bottom)                    │
└─────────────────────────────────────────────────────────────┘
```

**View modes** (select via Transport Bar buttons or keyboard):
- **Programmer** — Frame Editor + Laser Preview + 3D Preview
- **Show** — Quick Show grid / LivePRO performance view + output preview + BPM window
- **Setup** — Settings panels

All panels are dockable. Use **Windows → Reset Layout** to return to the default arrangement.

---

## First 90 Seconds

**Step 1: Press Play**

Click the Play button (▶) in the Transport Bar, or press `Space`.

You should immediately see:
- A beam pattern in the Laser Preview (green pulsing NDI dot = streaming)
- "IDHMFIS" appearing in NDI Studio Monitor on any machine on your LAN
- The beam simulation in the preview (beams with Gaussian glow in haze)

**Step 2: Connect ArtNet**

From any lighting console or the test sender tool:
```
artnet_test_sender.exe --ip <your machine's IP>
```

The ArtNet status indicator in the Transport Bar turns green when packets arrive. The cue selection and parameters respond within 8 ms.

**Step 3: Connect a DAC** (optional)

Plug in a Helios DAC or connect to an EtherDream on the network. IDHMFIS detects it automatically and shows it in the DAC status indicator. The physical laser output follows the same content as the NDI preview.

---

## The Laser Preview

The center panel shows the NDI stream — exactly what is sent over the network. Controls above the preview:

| Control | Function |
|---------|----------|
| Resolution | 720p / 1080p / 4K |
| FPS | 30 / 60 / 120 |
| Beam | Adjust beam thickness (0.1–20px) |
| Bloom | Bloom radius |
| Haze | Atmospheric density (0 = dry, 1 = thick fog) |
| Exposure | HDR exposure (0.1–4.0) |
| Grain | Film grain toggle |
| Fit | Fit preview to panel |
| 100% | Native resolution |
| F11 | Fullscreen preview |

The preview is **not** a camera view — it is the actual GPU render output. What you see is what the NDI receiver gets.

---

## Cue Library

The left panel shows all cues as cards. Each card displays:
- Generator type (icon)
- Cue name
- Color tag (for organization)
- A tiny animated preview

**Creating a cue:**
1. Click **+ New Cue** at the top of the library
2. Choose a generator from the dropdown
3. The cue is added and immediately selected

**Activating a cue:**
- Double-click a cue card, or press `Enter` with the card selected
- Or press `Ctrl+1` through `Ctrl+9` for the first 9 cues
- Fade time is determined by the cue's fade-in setting, or the DMX Master Fade (Ch25)

**Organizing cues:**
- Drag cues to reorder them
- Right-click a cue for: duplicate, rename, delete, change color, export ILDA
- Use the search box at the top to filter by name or generator type

---

## Inspector Panel (Right)

Three tabs:

### Cue Params tab
Shows sliders for the active cue's `GeneratorParams`:
- Speed, Scale, Density
- Generator-specific Param A/B/C (labels change per generator)
- Color A and Color B (color wheels)
- Pan / Tilt offset
- Zoom / Rotation

Every parameter is automatable — right-click a slider to Map to DMX / Map to MIDI / Map to OSC / Automate.

### DMX tab
Shows the 40-channel DMX fader stack for the active cue's DMX patch. Edit the base address and universe directly here. Click **Edit Profile** to open the fixture profile editor.

### Automation tab
Shows keyframe tracks for all automated parameters on the active cue. Click a parameter name to jump to its track in the Timeline. Click **+ Keyframe** to insert a keyframe at the playhead.

---

## Timeline

The bottom panel shows the cue list as blocks on a horizontal timeline.

**Transport controls:**
- `Space` — Play / Pause
- `Home` — Return to start
- Click anywhere on the ruler to move the playhead

**Editing:**
- Drag a cue block to move it in time
- Drag the right edge to trim its duration
- `[` / `]` — trim start/end to playhead position

**Beat markers:**
The ruler shows beat lines based on the current BPM. BPM is either auto-detected from audio or set manually in the Transport Bar.

---

## Safety Blackout Zones (v2.6.1)

Safety Blackout Zones prevent the laser from illuminating restricted areas.
All zones are **engine-enforced**: the point buffer is clipped before it reaches
the DAC or NDI output, regardless of which generator or FX is active.

Open the **Safety/BAM** panel (Windows menu or Ctrl+Shift+B).

### Border Crops

Crop from each edge of the output field independently.

| Crop | Description |
|------|-------------|
| Left | Crop from the left edge inward (0 = no crop, 1 = fully cropped) |
| Right | Crop from the right edge inward |
| Top | Crop from the top edge downward |
| Bottom | Crop from the bottom edge upward |
| Tilt | Rotate the crop boundary up to ±45 degrees (for angled stage lips) |

All values are in normalized output space (0..1 of the field half-width).

### Block Zones

Rectangular masked zones within the output field.

1. Click **+ Add Zone** in the Safety/BAM panel.
2. Drag the zone in the 2D preview overlay to position it.
3. Drag the corner handles to resize.
4. Set **Angle** to rotate the rectangle if needed.
5. Each zone has an **Enable** toggle to disable it without deleting it.

Block zones are stored per-project. The safety zone overlay is visible in both
the Frame Editor preview and the dedicated Safety/BAM panel preview.

**Master Enable**: the toggle at the top of the Safety/BAM panel enables or
disables all blackout enforcement at once. A red border appears around the
output preview when zones are disabled.

---

## FX System (v2.6.1)

IDHMFIS has two FX layers, both accessible from the Inspector or the cue editor.

### Global Layer FX (intensity / waveform modulation)

Applied to the global intensity output of the cue. Each entry has:

| Parameter | Description |
|-----------|-------------|
| Type | Sine, Square, Saw, Triangle, Flicker, RampUp, RampDown, Bump, Chase, Strobe |
| Blend | Add, Subtract, Absolute, Multiply |
| Rate | Hz (free-running) or BPM-synced |
| Depth | Amplitude 0..1 |
| Duty | Duty cycle for Square/Chase waveforms |
| Offset | Phase offset 0..1 |
| Direction | Sync, Forward, Backward, CentreOut, CentreIn, OddEven, Random |
| Dir Width | Phase spread (0 = all objects in phase, 1 = full wave across all objects) |
| Parts | Number of wave cycles across the group |
| Segs | Objects per segment (N adjacent share the same phase) |
| Width | Fraction of objects ON at any moment (duty-cycle gate) |
| CrossFade | 0 = snap to new value, 1 = smooth low-pass transition (new in v2.6.1) |

**Saw** (new in v2.6.1): sawtooth waveform that rises linearly from 0 to 1 over
one period, then snaps back. Use for one-directional sweep effects.

**StackFanOut** (new in v2.6.1): distributes phase across objects in a stacked
fan pattern. Combine with Forward direction for a traveling-wave look across a
beam stack.

### Frame FX Layer (geometric / color FX on the point buffer)

Applied per-frame to the point buffer geometry and colors.

| Type | Description |
|------|-------------|
| PanX / PanY | Sine pan left-right / up-down |
| Rotate | Sine rotation |
| Scale | Sine scale up/down |
| BounceX / BounceY | Absolute-value bounce (always positive axis) |
| ShakeX / ShakeY | Random jitter |
| ColorCycle | Hue rotation over time |
| ColorPulse | Brightness pulse |
| RainbowTrail | Rainbow color along path |
| Spiral | Simultaneous X and Y pan 90° apart |
| Col2 | 2-color square-wave between object color and hue-shifted version |
| Col3 | 3-color snap cycle through 3 hues 120° apart |
| ColFlick | Color flicker: random per-channel variation (fire/glitch effect) |
| Strobe | Rhythmic blanking: points blanked when phase > depth |

**Custom Color Pickers (new in v2.6.1)**: all color FX types (Col2, Col3,
ColFlick, ColorCycle, ColorPulse, RainbowTrail) have Color A and Color B pickers.
Enable **Use Custom Colors** to override the generator's colors with your own.

### FX Speed Display (new in v2.6.1)

FX rate controls show:
- Primary display: Hz (free-running) or note division (BPM sync mode)
- Tooltip on hover: duration in MM:SS format and BPM equivalent

When **BPM Sync** is active on a playback, the note division label replaces the
Hz display for all synced FX entries.

---

## Frame Editor

The Frame Editor (Programmer view, center panel) allows drawing and editing
laser objects directly.

### Tools

| Tool | Shortcut | Description |
|------|----------|-------------|
| Select | S | Select and move objects |
| Line | L | Draw polyline |
| Bezier | B | Draw cubic Bezier curve |
| Arc | A | Draw arc |
| Circle | C | Draw circle |
| Dot | D | Place point |
| Text | T | Place text label |
| Group Scale | G | Resize bounding box of all selected objects (new in v2.6.1) |
| Premade Shapes | P | Place mathematical curve from a library (new in v2.6.1) |

### Group Scale (new in v2.6.1)

1. Select two or more objects with the Select tool (click + Ctrl+click, or
   drag a selection box).
2. Press **G** or choose the Group Scale tool.
3. A bounding box appears around all selected objects.
4. Drag any handle to resize. All objects scale proportionally within the
   bounding box.
5. Hold Shift to constrain to the original aspect ratio.
6. Press Escape or click outside to confirm.

### Premade Shapes (new in v2.6.1)

1. Press **P** or choose Premade Shapes from the toolbar.
2. A shape picker panel opens with categories:
   - **Polygons**: triangle, square, pentagon, hexagon, up to 32 sides
   - **Stars**: 4-point, 5-point, 6-point, up to 16-point
   - **Curves**: Lissajous (with A/B/delta sliders), Spirograph (R/r/d sliders),
     Rose curve, Hypotrochoid
   - **Abstract**: figure-8, trefoil, infinity symbol
3. Adjust the shape parameters using the sliders in the picker panel.
4. Click in the canvas to place the shape. The shape is placed at click position
   and sized to fit within the current zoom level.
5. After placement, the shape is a normal LaserObject and can be edited with
   any other tool.

### Symmetry

Symmetry is applied at the KeyframeLayer level (affects the entire frame):

| Mode | Description |
|------|-------------|
| None | No symmetry |
| MirrorX | Reflected across vertical center axis |
| MirrorY | Reflected across horizontal center axis |
| MirrorXY | Reflected across both axes (4 copies total) |
| Radial2..12 | Rotational symmetry with 2, 3, 4, 6, 8, or 12 copies |

Set symmetry mode and center point in the Keyframe Layer panel (Inspector →
Keyframe tab).

---

## Generators Reference

### Beams
Produces 1–32 laser beams. Param A controls beam count; in radial mode (default) beams radiate from center. In parallel mode (low Param A) they are horizontal lines.

### Waves
Animating sine wave across the field. Param A = frequency, Param B = amplitude, Param C = phase offset.

### Lissajous
Classic Lissajous figure. Param A = frequency ratio a, Param B = frequency ratio b, Param C = phase δ. Animated by the Speed parameter.

### Tunnel
Concentric rings that zoom toward the viewer. Param A = ring count, Param B = rotation per ring, Param C = zoom speed.

### Text Scroller
Scrolling vector text using the Hershey simplex font. Set text in the text field below the Param sliders. Param A = scroll speed, Param C = letter spacing.

### Oscilloscope
Audio-reactive waveform or spectrum. Param A = gain, Param B = mode (0=waveform, 0.5=spectrum, 1=vectorscope). Connect audio in Settings → Audio to see this animate.

### FFT Bars
Frequency analyzer bars. Param A = bar count (16–64), Param B = gain. Color A = bass, Color B = treble.

### Spirograph
Spirograph/epitrochoid curve. Param A = R/r ratio, Param B = pen arm length d, Param C = trace speed.

### Particle Field
Deterministic particle system derived from time (no randomness — same t gives same output). Param A = particle count, Param B = velocity scale, Param C = gravity.

### Geometric Morph
Interpolates between two regular polygons. Param A = source sides (3–16), Param B = target sides (3–16), Param C = morph amount.

### Ribbon
Flowing Bezier ribbon. Param A = undulation frequency, Param B = ribbon width, Param C = ribbon length.

### Grid
Regular grid lines. Param A = horizontal line count, Param B = vertical line count, Param C = animation speed (slow wave modulation).

### Starburst
N rays from center. Param A = ray count, Param B = length modulation, Param C = rotation speed.

### Fan Sweep
Beams sweep back and forth in a fan arc. Param A = beam count, Param B = sweep arc angle, Param C = sweep speed.

### Cone Sweep
3D cone projection pattern. Param A = aperture angle, Param B = height scale, Param C = rotation speed.

---

## Point Optimizer

IDHMFIS applies a 7-step optimization pipeline to every output frame before
sending it to the DAC or NDI rasterizer. The optimizer is configured in
**Settings → DAC → Optimizer**.

| Step | Name | Description |
|------|------|-------------|
| 1 | Flatten | Remove consecutive duplicate points (same position within epsilon) |
| 2 | Anchor | Insert blank-dwell copies before lit segments; insert corner-dwell copies at sharp angles |
| 3 | Color edge interp | Insert 2 interpolated points at abrupt color transitions to reduce color smearing |
| 4 | Blanking | Insert dwell copies at the last lit point before a blank gap |
| 5 | Path ordering | Reorder segments using greedy nearest-neighbor to minimize galvo travel; flip individual segments if doing so reduces travel |
| 6 | Anchor (second pass) | Re-run anchor after reordering so galvos have proper settle time at each reordered segment start |
| 7 | Density norm | Redistribute points along path to match target PPS budget; interpolate sparse segments and skip surplus dense ones |
| 7b | Overscan clip | Remove points outside the output field + margin |

**v2.6.1 fix**: the double-anchor (steps 2 and 6) resolves wavy-line artifacts
that occurred when path-reorder placed a new segment start immediately after a
travel move. The second anchor pass gives galvo settle time in the reordered
output.

### Optimizer Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| Blank Dwell | 10 | Extra blanked-beam copies at blank-to-lit transition |
| Corner Dwell | 4 | Extra copies at sharp corners (angle > threshold) |
| Corner Angle Threshold | 45° | Minimum corner angle to trigger dwell |
| Enable Reorder | ON | Enable greedy path reorder (Step 5) |
| Target PPS | 30000 | Points per second budget for density normalization |
| Enable Overscan Clip | ON | Clip points outside output boundary |
| Overscan Margin | 0.05 | Margin beyond ±1 before clipping |

---

## Quick Show

Quick Show is a performance grid for triggering cues without navigating the
cue library. Access it from the Show view (select **Quick Show** tab in the
operator sidebar or press Q in Show view).

The grid has:
- **32 pages** (switchable with the page selector at the top)
- **6 rows × 10 columns** = 60 slots per page
- 1920 total cue slots across all pages

**Assigning cues to slots:**
1. In Programmer view, right-click any cue in the Cue Library.
2. Choose **Assign to Quick Show Slot**.
3. Click the target slot on the grid.

**Triggering in show:**
- Click any lit slot to activate that cue immediately.
- The active slot is highlighted.
- The next-queued slot (when a playback is running) shows an `is_next` indicator.

**Pages:**
Use the page buttons at the top (1–32) or swipe left/right on a touchscreen
to switch pages without interrupting the active cue.

---

## LivePRO Performance Mode

LivePRO is a simplified full-screen performance interface optimized for live
improvisation and DJ support. Access it from Show view → **LivePRO** tab.

Controls:
- **XY Pad**: real-time pan/tilt of the active output (mapped to `livepro_x`,
  `livepro_y` in normalized output space)
- **Scale fader**: output scale multiplier 0.1×–4×
- **Speed fader**: global animation speed multiplier 0.1×–8×
- **Beat grid**: 16 trigger pads that flash on beat; hold to sustain, release
  to return (Flash Hold toggle locks pads on)
- **Beat labels**: assignable names per pad

The XY pad and scale/speed faders are all MIDI-learnable. Right-click any
control to enter MIDI Learn mode.

---

## Multi-Playback System

IDHMFIS supports up to 40 simultaneous playbacks. Each playback is an
independent cue stack with its own:
- Intensity fader (0–100%)
- GO button / keyboard trigger key
- DMX trigger (one-channel or two-channel mode)
- End behavior (Stop or Loop)
- Blind mode (edit without affecting output)
- BPM sync option (advance cue on each beat; FX rates locked to BPM)

### Accessing Playbacks

The playback fader row appears at the bottom of the Show view and the bottom
of the Programmer view. Each fader has:
- Label (click to rename)
- Intensity fader
- GO button
- Gear icon to open the playback configuration popup (PBCONF)

### PBCONF Popup

| Setting | Description |
|---------|-------------|
| DMX Mode | Off, One Channel (0–255 range), Two Channel (16-bit, 0–65535) |
| DMX Universe / Channel | Which channel triggers this playback |
| DMX Threshold | DMX value above which the playback is activated |
| End Behavior | Stop at end of cue list, or Loop back to first cue |
| Keyboard GO Key | Press [Set] then the key to assign a keyboard GO trigger |
| GO at BPM | Advance one cue per BPM beat (ignores cue timing) |
| FX at BPM | All FX rates run relative to BPM (rate=1 = 1 cycle/beat) |

---

## Cue Stack Editor

The Cue Stack Editor (Windows → Cue Stack Editor) shows the full FullCueEntry
list for the selected playback.

Each row shows:
- Cue number (MagicQ-style, e.g. 1.0, 1.5, 2.0)
- Cue name and comment
- Trigger type (Halt, Follow, Wait, Timecode, MIDI, OSC, DMX, Audio)
- Fade in / Fade out times
- Link mode (Stop, Loop, Next, Jump)
- Color tag

**Adding cues:**
1. Select a cue in the Cue Library.
2. In the Cue Stack Editor, click **+ Append** to add it at the end, or
   **+ Insert** to add it after the selected row.

**Timing:**
Right-click any row → **Edit Timing** to set:
- Fade In / Fade Out (seconds; 0 = snap)
- Delay In / Delay Out
- Hold (time at full before fade out)
- Wait (time before auto-advance; 0 = Halt)
- Interpolation curve (Linear, SCurve, EaseIn, EaseOut, etc.)
- Fan mode (None, FrontBack, Even, CentreOut, EndsOut, Random)
- Split times (independent fade times per parameter class)

**Chaser mode:**
Check **Chaser** in the row editor to turn a cue list entry into a step
sequencer. Set:
- Global Hold (seconds per step)
- Global Crossfade (seconds between steps)
- Beat Sync (advance on BPM beat instead of time)
- Beat Division (1=bar, 2=half, 4=quarter, 8=eighth, 16=sixteenth)
- Steps list: ordered cue references, with per-step hold/xfade overrides

---

## Palette System

Palettes store reference values for color and position that can be applied
to cues quickly.

### Color Palette
- 32 named color slots
- Apply a slot to the active cue by clicking it in the Palette panel
- Record the current cue color into a slot by right-clicking the slot

### Position Palette
- 32 named position slots (pan/tilt pairs)
- Same record/apply workflow as color palette

The Palette panel is accessible from Windows → Palette or from the Inspector's
quick-access row.

### Color Input Modes

Accessible from the color settings icon in the Inspector:

| Mode | Description |
|------|-------------|
| RGB % | Red, Green, Blue as 0–100% |
| RGB Abs | Red, Green, Blue as 0–255 integers |
| CMY | Cyan, Magenta, Yellow (complementary) |
| HSI | Hue (0–360°), Saturation (0–100%), Intensity (0–100%) |

The selected mode persists per session. Color swatches (32 total: 8 default +
24 recordable) appear below the color wheel for quick recall.

---

## MIDI Learn

1. Right-click any parameter slider → **MIDI Learn** (or press Ctrl+Shift+L)
2. The MIDI Learn indicator in the Transport Bar turns yellow
3. Move a physical MIDI controller (knob, fader, or key)
4. The parameter is bound; the indicator returns to gray

Bindings are listed in the **MIDI Learn** panel (Windows menu). Each binding
shows the MIDI channel, CC or note number, and the target parameter. Click the
trash icon to remove a binding.

**BPM tap**: assignable to any MIDI note. Click **Set** next to Tap Key in the
BPM window and press the MIDI note you want to use.

---

## Automation

Every generator parameter can be automated over time using keyframe tracks.

1. In the Timeline, right-click the parameter name in the cue block → **Add Track**.
2. A keyframe track appears below the cue block.
3. Press **K** with the playhead at the desired time to insert a keyframe.
4. Drag keyframes horizontally to move them in time; drag vertically to change
   their value.
5. Right-click a keyframe to change interpolation (Linear, Ease In, Ease Out,
   Step, Bezier).

---

## ILDA File Import

1. File → Import ILDA... (or drag and drop an `.ild` file onto the Cue Library)
2. A new cue is created with GeneratorType = ILDASequence
3. The ILDA file path is stored relative to the project file
4. Frame rate defaults to 30 fps; adjust in the Inspector under "ILDA FPS"

---

## SVG Import

1. File → Import SVG... (or drag and drop an `.svg` file)
2. IDHMFIS extracts all `<path>` elements and converts to laser vectors
3. Options panel shows path density, blanking lead time, anchor repeats
4. Optimize Order = ON applies nearest-neighbour path reordering (recommended)
5. Result is a new cue with the SVG content

**Tips for clean SVG imports:**
- Use paths, not shapes (Inkscape: Path → Object to Path)
- Remove fills (outline only)
- Simplify curves (Path → Simplify) to reduce point density

---

## Audio Reactivity

1. Settings → Audio → Input Mode: select WASAPI Loopback (captures system audio)
2. Play audio on your system — the FFT analyzer in the footer bar shows activity
3. On any generator parameter slider, right-click → Map to Audio
4. Choose: rms, bpm, sub, mid, high, or FFT bin
5. The parameter now tracks the selected audio feature

The beat detection and BPM tracking is automatic. The BPM display in the Transport Bar reflects the detected BPM; you can override it manually.

---

## DMX / Art-Net Setup

See [DMX_CHANNEL_MAP.md](DMX_CHANNEL_MAP.md) for the full channel list.

**Quick setup:**
1. Settings → ArtNet → Universe: enter your Art-Net universe number
2. Settings → ArtNet → Source IP: 0.0.0.0 listens on all interfaces
3. IDHMFIS broadcasts an ArtPollReply — it should appear in your console's network devices within 2 seconds

---

## NDI Integration

See [NDI_INTEGRATION.md](NDI_INTEGRATION.md) for detailed integration with Resolume, TouchDesigner, OBS, disguise, etc.

Quick check: install NDI Studio Monitor, start IDHMFIS, press Play. Source "IDHMFIS" appears.

---

## Settings Reference

### Render
- Backend: D3D12 (Windows) / Vulkan (macOS)
- Beam Thickness: 0.1–20 pixels (default 2.5)
- Bloom Radius: 0–30 pixels (default 8.0)
- Bloom Alpha: 0–1 (default 0.25)
- Haze Density: 0–1 (default 0.4)
- Exposure: 0.1–4.0 (default 1.0)
- Film Grain: on/off (default off)

### 3D Preview
- Scan Mode: toggle between IRL projector (default) and scan-mode visualizer
- Scan Speed: scans/sec (scan mode only; default 0.8)
- Trail Length: % of point count shown as trail in scan mode (default 20%)
- Beam Brightness: multiplier on laser alpha (default 1.0)
- Haze Alpha: projector-to-wall haze beam alpha (default 0.35)
- Wall Glow Px: outer glow radius at wall impact (default 5px)
- Beam Width Px: solid wall segment line width (default 1.5px)

### NDI
- Enabled: on/off
- Source Name: "IDHMFIS" (default)
- Resolution: 720p / 1080p / 4K
- FPS: 30 / 50 / 60 / 120
- FPS Numerator/Denominator: for exact fractional rates (e.g. 60000/1001 for 59.94)
- NDI Clocked: true = NDI-clocked frame pacing (default); false = app-clocked
- Format: UYVY (default) / BGRA (with alpha)
- Bind Interface: All / specific adapter
- Virtual Camera: enable DirectShow virtual camera output (for software that cannot receive NDI natively)
- HDMI Window: enable borderless fullscreen output on a second display

### ArtNet Input
- Universe: 0–32767 (default 0)
- Port: 6454 (fixed per spec)
- Short Name: shown in console network view
- Long Name: shown in console device details

### ArtNet Output
- Enabled: on/off (default off)
- Destination IP: e.g. 2.255.255.255 (broadcast) or specific node IP
- Universe Offset: added to output universe numbers

### sACN / E1.31
- Enabled via Settings → ArtNet → sACN tab
- Universe: multicast group derived automatically

### Audio
- Mode: WASAPI Loopback / ASIO / None
- Device: default / specific device
- BPM Range: 60–180 (default)

### DAC
- Point Rate: 8000–60000 pps (default 30000)
- DAC Type: Auto / Helios / EtherDream / IDN / LaserDock
- EtherDream IP: for manual address (bypasses discovery)
- IDN Stream: enable IDN broadcast sidecar on UDP port 7255
- Emulated DAC: enable software-only Helios emulation for testing without hardware

### Safety
- Scan Fail Monitor: enable/disable (default: disabled — enable when real hardware is connected)
- Safety Blackout Zones: master enable/disable (see Safety Blackout Zones section above)
- EULA: show status; re-display acceptance dialog if needed

### Optimizer
See the Point Optimizer section above for all parameters.

### Output
- Master Output Enable: global on/off kill switch (default: hardware output disabled on launch)
- Output Enabled toggle is also available in the Transport Bar (the output indicator button)

---

## Troubleshooting

**No NDI source visible**
→ Install NDI Runtime from ndi.video/tools
→ Check Settings → NDI → Enabled
→ Check firewall (allow UDP 5353, TCP/UDP 5960+)

**ArtNet not working**
→ Check Settings → ArtNet → Universe matches your console
→ Ensure IDHMFIS appears in console's network devices (ArtPoll response)
→ Run `artnet_test_sender.exe --ip 127.0.0.1` to test locally

**DAC not detected**
→ For Helios: install libusb-1.0 (ships with Helios SDK installer)
→ For EtherDream: ensure same subnet; check LED status on unit
→ Settings → DAC → Force Type to manually select

**High latency**
→ Settings → DAC → Point Rate: reduce if system is overloaded
→ Check that the render thread is keeping up: View → Performance Monitor

**Wavy lines at segment starts**
→ This was a known issue resolved in v2.6.1 (double-anchor fix). Ensure you
   are running v2.6.1 or later. If the issue persists, increase Blank Dwell in
   Settings → DAC → Optimizer.

**Safety zone overlay not visible in Frame Editor**
→ Check that Safety Blackout Zones is enabled (Safety/BAM panel → Master Enable)
→ The overlay only shows when at least one zone or border crop is configured

**EULA dialog not appearing on a new machine**
→ The acceptance state is stored at `%APPDATA%\IDHMFIS\eula_accepted`
→ Delete this file to force the dialog on next launch (for multi-user machines
   where each operator should individually acknowledge the safety EULA)

**FX CrossFade causing slow response to FX edits**
→ CrossFade = 1 applies a strong low-pass filter. Reduce CrossFade toward 0
   if you need immediate response when changing FX parameters live.

**Crash on startup**
→ Check log: %APPDATA%\IDHMFIS\idhmfis.log
→ If NDI crash: try Settings → NDI → Enabled: OFF
→ If GPU crash: ensure Vulkan/D3D12 drivers are up to date
→ Autosave recovery: if a crash was detected, IDHMFIS will offer to restore
   the autosave from %APPDATA%\IDHMFIS\autosave\ on next launch

---

## Further Reading

| Document | Description |
|----------|-------------|
| [DMX_CHANNEL_MAP.md](DMX_CHANNEL_MAP.md) | Full 40-channel DMX fixture profile |
| [NDI_INTEGRATION.md](NDI_INTEGRATION.md) | Integration with Resolume, TouchDesigner, OBS, disguise, Hippotizer |
| [KEYBOARD_SHORTCUTS.md](KEYBOARD_SHORTCUTS.md) | All keyboard shortcuts (rebindable) |
| [CHANGELOG.md](CHANGELOG.md) | Release history |
| [BUILDING.md](BUILDING.md) | Build instructions for developers |

---

*IDHMFIS User Guide — Version 2.6.1*
*NDI® is a trademark of Vizrt Group. The NDI SDK is a separate download.*
