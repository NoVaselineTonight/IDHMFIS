// generator_registry.cpp — instantiates all built-in generators and provides
// the all_generators() / find_generator() factory functions.

#include "igenerator.h"
#include "../core/logger.h"
#include <vector>
#include <memory>
#include <string>

// Forward-declare every generator class defined in other .cpp files
namespace idhmfis {

// Each generator .cpp defines a class and registers it here via the vector.
// This translation unit owns the global list.

// Declared in respective .cpp files:
std::unique_ptr<IGenerator> make_gen_beams();
std::unique_ptr<IGenerator> make_gen_waves();
std::unique_ptr<IGenerator> make_gen_lissajous();
std::unique_ptr<IGenerator> make_gen_tunnel();
std::unique_ptr<IGenerator> make_gen_text();
std::unique_ptr<IGenerator> make_gen_oscilloscope();
std::unique_ptr<IGenerator> make_gen_fft_bars();
std::unique_ptr<IGenerator> make_gen_spirograph();
std::unique_ptr<IGenerator> make_gen_particles();
std::unique_ptr<IGenerator> make_gen_geomorph();
std::unique_ptr<IGenerator> make_gen_ribbon();
std::unique_ptr<IGenerator> make_gen_grid();
std::unique_ptr<IGenerator> make_gen_starburst();
std::unique_ptr<IGenerator> make_gen_fan_sweep();
std::unique_ptr<IGenerator> make_gen_cone_sweep();
std::unique_ptr<IGenerator> make_gen_ilda();

// Quick-shape generators
std::unique_ptr<IGenerator> make_gen_circle();
std::unique_ptr<IGenerator> make_gen_rect();
std::unique_ptr<IGenerator> make_gen_triangle();
std::unique_ptr<IGenerator> make_gen_star();
std::unique_ptr<IGenerator> make_gen_polygon();
std::unique_ptr<IGenerator> make_gen_line();
std::unique_ptr<IGenerator> make_gen_dot();
std::unique_ptr<IGenerator> make_gen_spiral();
std::unique_ptr<IGenerator> make_gen_sine_wave();

// Festival / EDM generators (R13)
std::unique_ptr<IGenerator> make_gen_dot_matrix();
std::unique_ptr<IGenerator> make_gen_burst();
std::unique_ptr<IGenerator> make_gen_waterfall();

// High-impact visual generators (R14)
std::unique_ptr<IGenerator> make_gen_thunderbolt();
std::unique_ptr<IGenerator> make_gen_vortex();
std::unique_ptr<IGenerator> make_gen_sierpinski();

// ─────────────────────────────────────────────────────────────────────────────

static std::vector<std::unique_ptr<IGenerator>> g_generators;
static bool g_initialized = false;

static void ensure_initialized()
{
    if (g_initialized) return;
    g_initialized = true;

    g_generators.push_back(make_gen_beams());
    g_generators.push_back(make_gen_waves());
    g_generators.push_back(make_gen_lissajous());
    g_generators.push_back(make_gen_tunnel());
    g_generators.push_back(make_gen_text());
    g_generators.push_back(make_gen_oscilloscope());
    g_generators.push_back(make_gen_fft_bars());
    g_generators.push_back(make_gen_spirograph());
    g_generators.push_back(make_gen_particles());
    g_generators.push_back(make_gen_geomorph());
    g_generators.push_back(make_gen_ribbon());
    g_generators.push_back(make_gen_grid());
    g_generators.push_back(make_gen_starburst());
    g_generators.push_back(make_gen_fan_sweep());
    g_generators.push_back(make_gen_cone_sweep());
    g_generators.push_back(make_gen_ilda());

    // Quick-shape generators
    g_generators.push_back(make_gen_circle());
    g_generators.push_back(make_gen_rect());
    g_generators.push_back(make_gen_triangle());
    g_generators.push_back(make_gen_star());
    g_generators.push_back(make_gen_polygon());
    g_generators.push_back(make_gen_line());
    g_generators.push_back(make_gen_dot());
    g_generators.push_back(make_gen_spiral());
    g_generators.push_back(make_gen_sine_wave());

    // Festival / EDM generators (R13)
    g_generators.push_back(make_gen_dot_matrix());
    g_generators.push_back(make_gen_burst());
    g_generators.push_back(make_gen_waterfall());

    // High-impact visual generators (R14)
    g_generators.push_back(make_gen_thunderbolt());
    g_generators.push_back(make_gen_vortex());
    g_generators.push_back(make_gen_sierpinski());

    log::info("Generator registry: %zu generators loaded",
              g_generators.size());
}

const std::vector<std::unique_ptr<IGenerator>>& all_generators()
{
    ensure_initialized();
    return g_generators;
}

IGenerator* find_generator(const std::string& name)
{
    ensure_initialized();
    for (auto& g : g_generators)
        if (name == g->name()) return g.get();
    return nullptr;
}

} // namespace idhmfis
