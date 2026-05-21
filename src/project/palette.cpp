// palette.cpp — Color and Position palette implementations.

#include "palette.h"
#include <algorithm>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  ColorPalette
// ─────────────────────────────────────────────────────────────────────────────

void ColorPalette::set(int idx, LaserColor c, const char* n)
{
    if (idx < 0 || idx >= kSlots) return;
    slots[static_cast<size_t>(idx)].color = c;
    slots[static_cast<size_t>(idx)].used  = true;
    if (n && *n)
        slots[static_cast<size_t>(idx)].name = n;
}

LaserColor ColorPalette::get(int idx) const
{
    if (idx < 0 || idx >= kSlots) return {1.f, 1.f, 1.f};
    return slots[static_cast<size_t>(idx)].color;
}

void ColorPalette::reset_to_defaults()
{
    // 24 standard laser show colors used by professional laser software
    struct Entry { float r, g, b; const char* name; };
    static constexpr Entry kDefaults[kSlots] = {
        // Row 0 — primaries + white
        { 1.f,  0.f,  0.f,  "Red"        },
        { 0.f,  1.f,  0.f,  "Green"      },
        { 0.f,  0.f,  1.f,  "Blue"       },
        { 1.f,  1.f,  0.f,  "Yellow"     },
        { 0.f,  1.f,  1.f,  "Cyan"       },
        { 1.f,  0.f,  1.f,  "Magenta"    },
        // Row 1 — white + pastels
        { 1.f,  1.f,  1.f,  "White"      },
        { 1.f,  0.5f, 0.f,  "Orange"     },
        { 0.5f, 1.f,  0.f,  "Lime"       },
        { 0.f,  0.5f, 1.f,  "Sky Blue"   },
        { 0.5f, 0.f,  1.f,  "Purple"     },
        { 1.f,  0.4f, 0.4f, "Coral"      },
        // Row 2 — mid-tones
        { 1.f,  0.8f, 0.f,  "Amber"      },
        { 0.f,  1.f,  0.5f, "Spring"     },
        { 0.f,  0.5f, 0.5f, "Teal"       },
        { 0.6f, 0.f,  0.8f, "Violet"     },
        { 1.f,  0.2f, 0.6f, "Hot Pink"   },
        { 0.2f, 0.8f, 1.f,  "Ice Blue"   },
        // Row 3 — darks / laser-specific
        { 0.8f, 0.f,  0.f,  "Dark Red"   },
        { 0.f,  0.6f, 0.f,  "Dark Green" },
        { 0.f,  0.f,  0.8f, "Dark Blue"  },
        { 0.4f, 0.4f, 0.4f, "Grey"       },
        { 1.f,  0.9f, 0.8f, "Warm White" },
        { 0.8f, 0.9f, 1.f,  "Cool White" },
    };

    for (int i = 0; i < kSlots; ++i) {
        auto& s    = slots[static_cast<size_t>(i)];
        s.color    = { kDefaults[i].r, kDefaults[i].g, kDefaults[i].b, 1.f };
        s.name     = kDefaults[i].name;
        s.used     = true;
    }
    name = "Default";
}

// ─────────────────────────────────────────────────────────────────────────────
//  PositionPalette
// ─────────────────────────────────────────────────────────────────────────────

void PositionPalette::reset_to_defaults()
{
    // 5x5 grid of named positions covering the ±1 laser canvas.
    // Positions are spaced evenly from -1 to +1 in both axes.
    // Row 0 = top (y=1), row 4 = bottom (y=-1)
    // Col 0 = left (x=-1), col 4 = right (x=1)

    static const char* kNames[kRows][kCols] = {
        { "Top-Left",    "Top-Center-L",  "Top",         "Top-Center-R",  "Top-Right"    },
        { "Mid-Left-T",  "Upper-Left",    "Upper-Center","Upper-Right",   "Mid-Right-T"  },
        { "Left",        "Center-Left",   "Center",      "Center-Right",  "Right"        },
        { "Mid-Left-B",  "Lower-Left",    "Lower-Center","Lower-Right",   "Mid-Right-B"  },
        { "Bottom-Left", "Bot-Center-L",  "Bottom",      "Bot-Center-R",  "Bottom-Right" },
    };

    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            int idx = row * kCols + col;
            auto& s     = slots[static_cast<size_t>(idx)];
            // Map col 0..4 -> x: -1, -0.5, 0, 0.5, 1
            // Map row 0..4 -> y:  1,  0.5, 0,-0.5,-1  (row 0 = top)
            s.x         = -1.f + (float)col * 0.5f;
            s.y         =  1.f - (float)row * 0.5f;
            s.scale     = 1.f;
            s.rotation  = 0.f;
            s.name      = kNames[row][col];
        }
    }
}

} // namespace idhmfis
