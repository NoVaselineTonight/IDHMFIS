#pragma once
#include <string>
#include <algorithm>
#include <cmath>

namespace idhmfis {

struct FxParam {
    std::string name;
    float base_value  = 0.f;   // UI-set base value
    float mod_value   = 0.f;   // sum of all modulation contributions
    float min_val     = 0.f;
    float max_val     = 1.f;
    float step        = 0.01f;
    std::string unit;           // "", "Hz", "deg", "s", etc.

    // Current effective value = clamp(base_value + mod_value, min_val, max_val)
    float effective() const {
        return std::clamp(base_value + mod_value, min_val, max_val);
    }

    // Effective value normalised to 0..1 over [min_val, max_val]
    float effective_norm() const {
        float range = max_val - min_val;
        if (range == 0.f) return 0.f;
        return std::clamp((effective() - min_val) / range, 0.f, 1.f);
    }
};

} // namespace idhmfis
