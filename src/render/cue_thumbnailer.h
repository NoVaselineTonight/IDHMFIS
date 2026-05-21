#pragma once
// cue_thumbnailer.h — background-thread 128x128 thumbnail renderer for cue list previews.

#include "../core/types.h"
#include "../project/project.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace idhmfis {

static constexpr int kThumbW = 128;
static constexpr int kThumbH = 128;
static constexpr int kThumbPixels = kThumbW * kThumbH;

// RGBA thumbnail for one cue
struct CueThumbnail {
    std::array<uint8_t, kThumbPixels * 4> pixels{};  // RGBA, row-major
    uint64_t version = 0;  // incremented each time the thumbnail is updated
    bool ready = false;
};

class CueThumbnailer {
public:
    CueThumbnailer();
    ~CueThumbnailer();

    // Request a thumbnail for a cue. Generator name + GeneratorParams uniquely identify the content.
    void request(int cue_idx, const std::string& generator_name, const GeneratorParams& params);

    // Returns a copy of the current thumbnail for the cue (may be empty if not rendered yet).
    CueThumbnail get(int cue_idx) const;

    // True if any thumbnail was updated since the last call to consume_dirty().
    bool has_dirty() const { return dirty_.load(std::memory_order_relaxed); }
    void consume_dirty() { dirty_.store(false, std::memory_order_relaxed); }

    void start();
    void stop();

private:
    struct Request { int idx; std::string gen_name; GeneratorParams params; };

    void render_loop();
    void render_one(const Request& req);

    // Rasterize a PointBuffer into the 128x128 pixel grid.
    // Points have x,y in ILDA-space [-32767..32767]. Each point is drawn as a
    // bright circle of radius 2 px; blanked points are skipped.
    static void rasterize(const PointBuffer& pts, CueThumbnail& thumb);

    mutable std::mutex thumbs_mtx_;
    std::vector<CueThumbnail> thumbs_;

    mutable std::mutex req_mtx_;
    std::vector<Request> pending_;
    std::atomic<bool> has_pending_{false};

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<bool> dirty_{false};
};

} // namespace idhmfis
