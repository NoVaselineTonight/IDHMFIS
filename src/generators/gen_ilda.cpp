// gen_ilda.cpp -- ILDASequence wrapped as an IGenerator.
//
// The ILDA generator loads a .ild file and plays it back at the given FPS.
// The path is taken from GeneratorParams... via a global per-cue string
// (in production set before calling generate()).

#include "igenerator.h"
#include "ilda_sequence.h"
#include "../core/logger.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

namespace idhmfis {

// Global per-path cache so we don't re-parse the file every frame.
namespace {

struct ILDACache {
    std::mutex mtx;
    std::unordered_map<std::string, std::shared_ptr<ILDASequence>> cache;

    std::shared_ptr<ILDASequence> get(const std::string& path)
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
        auto seq = std::make_shared<ILDASequence>();
        if (seq->load(path))
        {
            cache[path] = seq;
            log::info("ILDACache: loaded '%s' (%d frames)",
                      path.c_str(), seq->frame_count());
            return seq;
        }
        log::warn("ILDACache: failed to load '%s': %s",
                  path.c_str(), seq->error().c_str());
        return nullptr;
    }
};

ILDACache& ilda_cache()
{
    static ILDACache c;
    return c;
}

// Global ILDA path — set by show engine before calling generate().
std::string g_ilda_path;

} // namespace

void set_ilda_path(const std::string& p) { g_ilda_path = p; }

class GenILDA final : public IGenerator {
public:
    const char* name()         const override { return "ilda"; }
    const char* display_name() const override { return "ILDA Sequence"; }
    const char* param_a_label()const override { return "FPS Override"; }
    const char* param_b_label()const override { return "Loop"; }
    const char* param_c_label()const override { return "Brightness"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        if (g_ilda_path.empty()) return {};

        auto seq = ilda_cache().get(g_ilda_path);
        if (!seq || seq->frame_count() == 0) return {};

        float fps = (p.param_a < 0.01f) ? 30.f : p.param_a * 120.f; // 0..120 fps
        fps = std::clamp(fps, 1.f, 120.f);

        PointBuffer pts = seq->frame_at(t * p.speed, fps);

        // Apply brightness from param_c
        float bright = p.param_c;
        if (bright > 0.f && bright < 1.f)
        {
            for (LaserPoint& lp : pts)
            {
                if (!lp.blanked)
                {
                    lp.r = static_cast<uint8_t>(lp.r * bright);
                    lp.g = static_cast<uint8_t>(lp.g * bright);
                    lp.b = static_cast<uint8_t>(lp.b * bright);
                }
            }
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_ilda()
{
    return std::make_unique<GenILDA>();
}

} // namespace idhmfis
