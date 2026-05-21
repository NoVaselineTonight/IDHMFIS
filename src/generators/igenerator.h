#pragma once
// IGenerator — the common interface every laser content generator implements.
// Generators are pure functions of (params, t). No internal mutable state is
// permitted; all time-dependent behaviour must derive from the 't' argument.
// Exception: ILDASequence caches parsed file data in load(), not in generate().

#include "../core/types.h"
#include <string>
#include <vector>
#include <memory>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Named generator parameter descriptor
//  Generators declare as many of these as they need (not limited to 3).
//  The UI renders one slider per entry.  Values for params beyond param_a/b/c
//  are stored in GeneratorParams::extra_params by name.
// ─────────────────────────────────────────────────────────────────────────────
struct GeneratorParamDef {
    const char* name;        // key used in extra_params and on_param_changed
    const char* label;       // display label (nullptr → use name)
    float       min_val;
    float       max_val;
    float       default_val;
    const char* fmt;         // printf format (e.g. "%.2f", "%.0f")
};

// ─────────────────────────────────────────────────────────────────────────────
//  Generator interface
// ─────────────────────────────────────────────────────────────────────────────
class IGenerator {
public:
    virtual ~IGenerator() = default;

    // Internal registry name, e.g. "beams", "waves", "lissajous"
    virtual const char* name()         const = 0;

    // Human-readable display name for UI
    virtual const char* display_name() const = 0;

    // Generate a complete frame from params at time t (seconds since engine start).
    // Must return exactly p.point_count points (or close to it).
    // This function MUST be const and re-entrant.
    virtual PointBuffer generate(const GeneratorParams& p, double t) const = 0;

    // Variable-length parameter list.  The default implementation exposes the
    // three legacy slots (param_a, param_b, param_c) using the label/range
    // methods below so old generators work without any changes.
    // Override this to declare a different (larger) set of parameters.
    virtual std::vector<GeneratorParamDef> param_defs() const {
        return {
            { "param_a", param_a_label(), param_a_min(), param_a_max(), 0.5f, param_a_fmt() },
            { "param_b", param_b_label(), param_b_min(), param_b_max(), 0.5f, param_b_fmt() },
            { "param_c", param_c_label(), param_c_min(), param_c_max(), 0.5f, param_c_fmt() },
        };
    }

    // Human labels for the generic param_a/b/c sliders in the UI.
    // Used only by the default param_defs() implementation.
    virtual const char* param_a_label() const { return "Param A"; }
    virtual const char* param_b_label() const { return "Param B"; }
    virtual const char* param_c_label() const { return "Param C"; }

    // Min/max range for param_a/b/c (used by Inspector sliders).
    virtual float param_a_min() const { return 0.f; }
    virtual float param_a_max() const { return 1.f; }
    virtual float param_b_min() const { return 0.f; }
    virtual float param_b_max() const { return 1.f; }
    virtual float param_c_min() const { return 0.f; }
    virtual float param_c_max() const { return 1.f; }

    // Printf format string for each param.
    virtual const char* param_a_fmt() const { return "%.2f"; }
    virtual const char* param_b_fmt() const { return "%.2f"; }
    virtual const char* param_c_fmt() const { return "%.2f"; }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Factory
// ─────────────────────────────────────────────────────────────────────────────

// Returns the global list of all built-in generators, instantiated once.
const std::vector<std::unique_ptr<IGenerator>>& all_generators();

// Find a generator by name(). Returns nullptr if not found.
IGenerator* find_generator(const std::string& name);

} // namespace idhmfis
