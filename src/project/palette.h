#pragma once
#include "../core/types.h"
#include <array>
#include <string>
#include <vector>

namespace idhmfis {

// LaserColor is an alias for Color4 (float RGB, 0..1 per channel)
using LaserColor = Color4;

// 24-slot named color palette (matches professional laser software standard)
struct ColorPalette {
    static constexpr int kSlots = 24;

    struct Slot {
        LaserColor color  = {1.f, 1.f, 1.f};
        std::string name;
        bool used = false;
    };

    std::array<Slot, kSlots> slots;
    std::string name = "Default";

    void set(int idx, LaserColor c, const char* n = "");
    LaserColor get(int idx) const;
    void reset_to_defaults(); // sets 24 standard laser colors
};

// 5x5 position palette — 25 named XY positions
struct PositionPalette {
    static constexpr int kCols = 5;
    static constexpr int kRows = 5;
    static constexpr int kSlots = kCols * kRows;

    struct Slot {
        float x = 0.f, y = 0.f; // -1..1
        float scale = 1.f;
        float rotation = 0.f;
        std::string name;
    };

    std::array<Slot, kSlots> slots;
    void reset_to_defaults(); // sensible grid of positions
};

} // namespace idhmfis
