#include "fx_registry.h"

// --- Existing blocks ---
#include "blocks/block_grid_scan.h"
#include "blocks/block_wave.h"
#include "blocks/block_rotate.h"
#include "blocks/block_scale.h"
#include "blocks/block_mirror.h"
#include "blocks/block_color_cycle.h"
#include "blocks/block_color_ramp.h"
#include "blocks/block_strobe.h"
#include "blocks/block_lfo.h"
#include "blocks/block_envelope_follower.h"

// --- New blocks ---
// Symmetry
#include "blocks/block_symmetry.h"
#include "blocks/block_kaleidoscope.h"
#include "blocks/block_bilateral_mirror.h"
// Movement
#include "blocks/block_orbit.h"
#include "blocks/block_bounce.h"
#include "blocks/block_pendulum.h"
#include "blocks/block_spiral_scan.h"
#include "blocks/block_scanner_line.h"
#include "blocks/block_tunnel.h"
#include "blocks/block_stack_fan_out.h"
#include "blocks/block_saw.h"
// Dots
#include "blocks/block_scatter.h"
#include "blocks/block_dots_chase.h"
#include "blocks/block_stagger.h"
#include "blocks/block_particle_trail.h"
// Color
#include "blocks/block_gradient_map.h"
#include "blocks/block_chroma_shift.h"
#include "blocks/block_pulse_color.h"
// Distortion
#include "blocks/block_wave_distort.h"
#include "blocks/block_pinch_bulge.h"
#include "blocks/block_turbulence.h"
// Safety
#include "blocks/block_safety_zone.h"
#include "blocks/block_clamp.h"

#include <algorithm>

namespace idhmfis {

FxRegistry& FxRegistry::instance() {
    static FxRegistry inst;
    return inst;
}

void FxRegistry::register_all() {
    entries_.clear();

    // Helper lambda to push an entry
    auto add = [this](const char* n, const char* cat, const char* desc,
                      std::function<std::unique_ptr<IFxBlock>()> fac) {
        entries_.push_back({ n, cat, desc, std::move(fac) });
    };

    // ── Existing blocks ───────────────────────────────────────────────────────

    add("Wave",             "Geometry",   "Sinusoidal/sawtooth/triangle/square wave distortion.",
        [] { return std::make_unique<BlockWave>(); });

    add("Rotate",           "Geometry",   "Rotates all points around a centre. Speed auto-animates.",
        [] { return std::make_unique<BlockRotate>(); });

    add("Scale",            "Geometry",   "Independent X/Y scale around a configurable centre.",
        [] { return std::make_unique<BlockScale>(); });

    add("Mirror",           "Geometry",   "Folds/reflects points. Kaleidoscope mode radially repeats.",
        [] { return std::make_unique<BlockMirror>(); });

    add("ColorCycle",       "Color",      "Animates hue shift across points by path or time.",
        [] { return std::make_unique<BlockColorCycle>(); });

    add("ColorRamp",        "Color",      "Remaps luminance to a hue gradient.",
        [] { return std::make_unique<BlockColorRamp>(); });

    add("Strobe",           "Modulation", "Hardware-style strobe effect.",
        [] { return std::make_unique<BlockStrobe>(); });

    add("LFO",              "Modulation", "Low-frequency oscillator routed via modulation matrix.",
        [] { return std::make_unique<BlockLfo>(); });

    add("EnvelopeFollower", "Modulation", "Audio envelope follower routed via modulation matrix.",
        [] { return std::make_unique<BlockEnvelopeFollower>(); });

    // ── Symmetry ─────────────────────────────────────────────────────────────

    add("Symmetry",         "Symmetry",   "N-fold radial symmetry around a centre point.",
        [] { return std::make_unique<BlockSymmetry>(); });

    add("Kaleidoscope",     "Symmetry",   "Kaleidoscope mirror fold into N sectors.",
        [] { return std::make_unique<BlockKaleidoscope>(); });

    add("BilateralMirror",  "Symmetry",   "Duplicates points mirrored across X, Y, and/or diagonal.",
        [] { return std::make_unique<BlockBilateralMirror>(); });

    // ── Movement ─────────────────────────────────────────────────────────────

    add("Orbit",            "Movement",   "Orbits the whole pattern around a point (circle/ellipse).",
        [] { return std::make_unique<BlockOrbit>(); });

    add("Bounce",           "Movement",   "Physics bounce under gravity with restitution.",
        [] { return std::make_unique<BlockBounce>(); });

    add("Pendulum",         "Movement",   "Lissajous-style pendulum translation.",
        [] { return std::make_unique<BlockPendulum>(); });

    add("SpiralScan",       "Movement",   "Rotates and scales the pattern in a spiral sweep.",
        [] { return std::make_unique<BlockSpiralScan>(); });

    add("ScannerLine",      "Movement",   "Sweeping bright scan line overlay.",
        [] { return std::make_unique<BlockScannerLine>(); });

    add("Tunnel",           "Movement",   "Concentric rings animating inward to simulate a tunnel.",
        [] { return std::make_unique<BlockTunnel>(); });

    add("StackFanOut",      "Movement",   "Collapses objects to a stack point then fans them out over a cycle.",
        [] { return std::make_unique<BlockStackFanOut>(); });

    add("Saw",              "Movement",   "Sawtooth pan/tilt: ramps from side A to side B then snaps back.",
        [] { return std::make_unique<BlockSaw>(); });

    // ── Dots ─────────────────────────────────────────────────────────────────

    add("Scatter",          "Dots",       "Random shimmering scatter displacement.",
        [] { return std::make_unique<BlockScatter>(); });

    add("DotsChase",        "Dots",       "N chasing dots along the path.",
        [] { return std::make_unique<BlockDotsChase>(); });

    add("Stagger",          "Dots",       "Groups of points flash in alternating/chase/random modes.",
        [] { return std::make_unique<BlockStagger>(); });

    add("ParticleTrail",    "Dots",       "Ghost copies of the pattern forming a motion trail.",
        [] { return std::make_unique<BlockParticleTrail>(); });

    // ── Color ─────────────────────────────────────────────────────────────────

    add("GradientMap",      "Color",      "Maps position/index to a two-colour gradient.",
        [] { return std::make_unique<BlockGradientMap>(); });

    add("ChromaShift",      "Color",      "RGB chromatic aberration / channel split.",
        [] { return std::make_unique<BlockChromaShift>(); });

    add("PulseColor",       "Color",      "Pulses colour between two values at a given rate.",
        [] { return std::make_unique<BlockPulseColor>(); });

    // ── Grid / Scan ───────────────────────────────────────────────────────────

    add("GridScan",          "Geometry",   "Appends animated H/V/both grid scan lines (raster laser effect).",
        [] { return std::make_unique<BlockGridScan>(); });

    // ── Distortion ────────────────────────────────────────────────────────────

    add("WaveDistort",      "Distortion", "Sinusoidal mesh distortion.",
        [] { return std::make_unique<BlockWaveDistort>(); });

    add("PinchBulge",       "Distortion", "Lens distortion: pinch or bulge.",
        [] { return std::make_unique<BlockPinchBulge>(); });

    add("Turbulence",       "Distortion", "Fractal noise-based turbulent displacement.",
        [] { return std::make_unique<BlockTurbulence>(); });

    // ── Safety ────────────────────────────────────────────────────────────────

    add("SafetyZone",       "Safety",     "Blanks points inside a circular exclusion zone. Stack for multi-zone rigs.",
        [] { return std::make_unique<BlockSafetyZone>(); });

    add("Clamp",            "Safety",     "Hard-clips output to a configurable bounding box (clamp-to-edge or blank).",
        [] { return std::make_unique<BlockClamp>(); });
}

const std::vector<FxBlockMeta>& FxRegistry::all() const {
    return entries_;
}

std::vector<FxBlockMeta> FxRegistry::by_category(const std::string& cat) const {
    std::vector<FxBlockMeta> result;
    for (const FxBlockMeta& m : entries_) {
        if (m.category == cat) result.push_back(m);
    }
    return result;
}

std::unique_ptr<IFxBlock> FxRegistry::create(const std::string& block_name) const {
    for (const FxBlockMeta& m : entries_) {
        if (m.name == block_name) return m.factory();
    }
    return nullptr;
}

} // namespace idhmfis
